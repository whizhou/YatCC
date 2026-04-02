#include "Ast2Asg.hpp"
#include <unordered_map>

#define self (*this)

namespace asg {

// 符号表，保存当前作用域的所有声明
struct Ast2Asg::Symtbl : public std::unordered_map<std::string, Decl*>
{
  Ast2Asg& m;
  Symtbl* mPrev;

  Symtbl(Ast2Asg& m)
    : m(m)
    , mPrev(m.mSymtbl)
  {
    m.mSymtbl = this;
  }

  ~Symtbl() { m.mSymtbl = mPrev; }

  Decl* resolve(const std::string& name);
};

Decl*
Ast2Asg::Symtbl::resolve(const std::string& name)
{
  auto iter = find(name);
  if (iter != end())
    return iter->second;
  ASSERT(mPrev != nullptr); // 标识符未定义
  return mPrev->resolve(name);
}

TranslationUnit*
Ast2Asg::operator()(ast::TranslationUnitContext* ctx)
{
  auto ret = make<asg::TranslationUnit>();
  if (ctx == nullptr)
    return ret;

  Symtbl localDecls(self);

  for (auto&& i : ctx->externalDeclaration()) {
    if (auto p = i->declaration()) {
      auto decls = self(p);
      ret->decls.insert(ret->decls.end(),
                        std::make_move_iterator(decls.begin()),
                        std::make_move_iterator(decls.end()));
    }

    else if (auto p = i->functionDefinition()) {
      auto funcDecl = self(p);
      ret->decls.push_back(funcDecl);

      // 添加到声明表
      localDecls[funcDecl->name] = funcDecl;
    }

    else
      ABORT();
  }

  return ret;
}

//==============================================================================
// 类型
//==============================================================================

Ast2Asg::SpecQual
Ast2Asg::operator()(ast::DeclarationSpecifiersContext* ctx)
{
  SpecQual ret = { Type::Spec::kINVALID, Type::Qual() };

  for (auto&& i : ctx->declarationSpecifier()) {
    if (auto p = i->typeSpecifier()) {
      if (ret.first == Type::Spec::kINVALID) {
        if (p->Int())
          ret.first = Type::Spec::kInt;
        else
          ABORT(); // 未知的类型说明符
      }

      else
        ABORT(); // 未知的类型说明符
    }

    else if (auto p = i->typeQualifier()) {
      if (p->Const())
        ret.second.const_ = true;
      else
        ABORT(); // 未知的类型限定符
    }

    else
      ABORT();
  }

  return ret;
}

std::pair<TypeExpr*, std::string>
Ast2Asg::operator()(ast::DeclaratorContext* ctx, TypeExpr* sub)
{
  return self(ctx->directDeclarator(), sub);
}

static int
eval_arrlen(Expr* expr)
{
  if (auto p = expr->dcst<IntegerLiteral>())
    return p->val;

  if (auto p = expr->dcst<DeclRefExpr>()) {
    if (p->decl == nullptr)
      ABORT();

    auto var = p->decl->dcst<VarDecl>();
    if (!var || !var->type->qual.const_)
      ABORT(); // 数组长度必须是编译期常量

    switch (var->type->spec) {
      case Type::Spec::kChar:
      case Type::Spec::kInt:
      case Type::Spec::kLong:
      case Type::Spec::kLongLong:
        return eval_arrlen(var->init);

      default:
        ABORT(); // 长度表达式必须是数值类型
    }
  }

  if (auto p = expr->dcst<UnaryExpr>()) {
    auto sub = eval_arrlen(p->sub);

    switch (p->op) {
      case UnaryExpr::kPos:
        return sub;

      case UnaryExpr::kNeg:
        return -sub;

      default:
        ABORT();
    }
  }

  if (auto p = expr->dcst<BinaryExpr>()) {
    auto lft = eval_arrlen(p->lft);
    auto rht = eval_arrlen(p->rht);

    switch (p->op) {
      case BinaryExpr::kAdd:
        return lft + rht;

      case BinaryExpr::kSub:
        return lft - rht;

      case BinaryExpr::kMul:
        return lft * rht;

      case BinaryExpr::kDiv:
        return lft / rht;

      case BinaryExpr::kMod:
        return lft % rht;

      default:
        ABORT();
    }
  }

  if (auto p = expr->dcst<InitListExpr>()) {
    if (p->list.empty())
      return 0;
    return eval_arrlen(p->list[0]);
  }

  ABORT();
}

std::pair<TypeExpr*, std::string>
Ast2Asg::operator()(ast::DirectDeclaratorContext* ctx, TypeExpr* sub)
{
  if (auto p = ctx->Identifier())
    return { sub, p->getText() };

  if (ctx->LeftBracket()) {
    auto arrayType = make<ArrayType>();
    arrayType->sub = sub;

    if (auto p = ctx->assignmentExpression())
      arrayType->len = eval_arrlen(self(p));
    else
      arrayType->len = ArrayType::kUnLen;

    return self(ctx->directDeclarator(), arrayType);
  }

  if (ctx->LeftParen()) {
    auto funcType = make<FunctionType>();
    funcType->sub = sub;

    // 清空之前的参数名列表
    mParamNames.clear();

    if (auto p = ctx->parameterTypeList()) {
      auto params = self(p);  // 返回 vector<ParamInfo>
      for (auto& [type, name] : params) {
        funcType->params.push_back(type);
        mParamNames.push_back(name);  // 收集参数名
      }
    } else if (auto p = ctx->identifierList()) {
      // identifierList 用于 K&R 风格函数声明
      auto names = self(p);
      for (const auto& name : names) {
        auto paramType = make<Type>();
        paramType->spec = Type::Spec::kInt;
        funcType->params.push_back(paramType);
        mParamNames.push_back(name);  // 收集参数名
      }
    }
    // 如果是 directDeclarator LeftParen RightParen，则参数列表为空

    return self(ctx->directDeclarator(), funcType);
  }

  ABORT();
}

std::vector<Ast2Asg::ParamInfo>
Ast2Asg::operator()(ast::ParameterTypeListContext* ctx)
{
  return self(ctx->parameterList());
}

std::vector<Ast2Asg::ParamInfo>
Ast2Asg::operator()(ast::ParameterListContext* ctx)
{
  std::vector<ParamInfo> ret;

  // 递归处理：parameterList 可能包含另一个 parameterList
  if (auto subList = ctx->parameterList()) {
    ret = self(subList);
  }

  // 添加当前层的 parameterDeclaration
  if (auto decl = ctx->parameterDeclaration()) {
    ret.push_back(self(decl));
  }

  return ret;
}

Ast2Asg::ParamInfo
Ast2Asg::operator()(ast::ParameterDeclarationContext* ctx)
{
  auto sq = self(ctx->declarationSpecifiers());

  if (auto decl = ctx->declarator()) {
    auto [texp, name] = self(decl, nullptr);
    auto type = make<Type>();
    type->spec = sq.first;
    type->qual = sq.second;
    type->texp = texp;
    return { type, name };  // 返回类型和参数名
  }

  // 只有 declarationSpecifiers，没有 declarator
  auto type = make<Type>();
  type->spec = sq.first;
  type->qual = sq.second;
  return { type, "" };  // 无参数名的情况
}

std::vector<std::string>
Ast2Asg::operator()(ast::IdentifierListContext* ctx)
{
  std::vector<std::string> ret;
  for (auto&& i : ctx->Identifier()) {
    ret.push_back(i->getText());
  }
  return ret;
}

//==============================================================================
// 表达式
//==============================================================================

Expr*
Ast2Asg::operator()(ast::ExpressionContext* ctx)
{
  auto list = ctx->assignmentExpression();
  Expr* ret = self(list[0]);

  for (unsigned i = 1; i < list.size(); ++i) {
    auto node = make<BinaryExpr>();
    node->op = node->kComma;
    node->lft = ret;
    node->rht = self(list[i]);
    ret = node;
  }

  return ret;
}

Expr*
Ast2Asg::operator()(ast::AssignmentExpressionContext* ctx)
{
  if (auto p = ctx->additiveExpression())
    return self(p);

  auto ret = make<BinaryExpr>();
  ret->op = ret->kAssign;
  ret->lft = self(ctx->unaryExpression());
  ret->rht = self(ctx->assignmentExpression());
  return ret;
}
Expr*
Ast2Asg::operator()(ast::MultiplicativeExpressionContext* ctx)
{
  auto children = ctx->children;
  Expr* ret = self(dynamic_cast<ast::UnaryExpressionContext*>(children[0]));

  for (unsigned i = 1; i < children.size(); ++i) {
    auto node = make<BinaryExpr>();

    auto token = dynamic_cast<antlr4::tree::TerminalNode*>(children[i])
                   ->getSymbol()
                   ->getType();
    switch (token) {
      case ast::Times:
        node->op = node->kMul;
        break;

      case ast::Divide:
        node->op = node->kDiv;
        break;

      case ast::Modulo:
        node->op = node->kMod;
        break;

      default:
        ABORT();
    }

    node->lft = ret;
    node->rht = self(dynamic_cast<ast::UnaryExpressionContext*>(children[++i]));
    ret = node;
  }

  return ret;
}

Expr*
Ast2Asg::operator()(ast::AdditiveExpressionContext* ctx)
{
  auto children = ctx->children;
  Expr* ret = self(dynamic_cast<ast::MultiplicativeExpressionContext*>(children[0]));

  for (unsigned i = 1; i < children.size(); ++i) {
    auto node = make<BinaryExpr>();

    auto token = dynamic_cast<antlr4::tree::TerminalNode*>(children[i])
                   ->getSymbol()
                   ->getType();
    switch (token) {
      case ast::Plus:
        node->op = node->kAdd;
        break;

      case ast::Minus:
        node->op = node->kSub;
        break;

      default:
        ABORT();
    }

    node->lft = ret;
    node->rht = self(dynamic_cast<ast::MultiplicativeExpressionContext*>(children[++i]));
    ret = node;
  }

  return ret;
}

Expr*
Ast2Asg::operator()(ast::UnaryExpressionContext* ctx)
{
  if (auto p = ctx->postfixExpression())
    return self(p);

  auto ret = make<UnaryExpr>();

  switch (
    dynamic_cast<antlr4::tree::TerminalNode*>(ctx->unaryOperator()->children[0])
      ->getSymbol()
      ->getType()) {
    case ast::Plus:
      ret->op = ret->kPos;
      break;

    case ast::Minus:
      ret->op = ret->kNeg;
      break;

    default:
      ABORT();
  }

  ret->sub = self(ctx->unaryExpression());

  return ret;
}

Expr*
Ast2Asg::operator()(ast::PostfixExpressionContext* ctx)
{
  // 处理左递归语法生成的嵌套解析树
  // 检查是否是基础情况：primaryExpression
  if (auto primary = ctx->primaryExpression()) {
    return self(primary);
  }

  // 递归情况：postfixExpression 后跟操作
  Expr* ret = self(ctx->postfixExpression());

  // 检查是数组下标访问还是函数调用
  if (ctx->LeftBracket()) {
    // 数组下标访问：postfixExpression '[' expression ']'
    auto node = make<BinaryExpr>();
    node->op = node->kIndex;
    node->lft = ret;
    node->rht = self(ctx->expression());
    ret = node;
  }
  else if (ctx->LeftParen()) {
    // 函数调用：postfixExpression '(' ')' 或 postfixExpression '(' argumentExpressionList ')'
    auto node = make<CallExpr>();
    node->head = ret;
    
    // 检查是否有参数列表
    if (auto argList = ctx->argumentExpressionList()) {
      node->args = self(argList);
    }
    ret = node;
  }

  return ret;
}

std::vector<Expr*>
Ast2Asg::operator()(ast::ArgumentExpressionListContext* ctx)
{
  std::vector<Expr*> ret;
  for (auto&& arg : ctx->assignmentExpression()) {
    ret.push_back(self(arg));
  }
  return ret;
}

Expr*
Ast2Asg::operator()(ast::PrimaryExpressionContext* ctx)
{

  if (auto p = ctx->Identifier()) {
    auto name = p->getText();
    auto ret = make<DeclRefExpr>();
    ret->decl = mSymtbl->resolve(name);
    return ret;
  }

  if (auto p = ctx->Constant()) {
    auto text = p->getText();

    auto ret = make<IntegerLiteral>();

    ASSERT(!text.empty());
    if (text[0] != '0')
      ret->val = std::stoll(text);

    else if (text.size() == 1)
      ret->val = 0;

    else if (text[1] == 'x' || text[1] == 'X')
      ret->val = std::stoll(text.substr(2), nullptr, 16);

    else
      ret->val = std::stoll(text.substr(1), nullptr, 8);

    return ret;
  }

  if (ctx->LeftParen()) {
    auto ret = make<ParenExpr>();
    ret->sub = self(ctx->expression());
    return ret;
  }

  ABORT();
}

Expr*
Ast2Asg::operator()(ast::InitializerContext* ctx)
{
  if (auto p = ctx->assignmentExpression())
    return self(p);

  auto ret = make<InitListExpr>();

  if (auto p = ctx->initializerList()) {
    for (auto&& i : p->initializer()) {
      // 将初始化列表展平
      auto expr = self(i);
      if (auto p = expr->dcst<InitListExpr>()) {
        for (auto&& sub : p->list)
          ret->list.push_back(sub);
      } else {
        ret->list.push_back(expr);
      }
    }
  }

  return ret;
}

//==============================================================================
// 语句
//==============================================================================

Stmt*
Ast2Asg::operator()(ast::StatementContext* ctx)
{
  if (auto p = ctx->compoundStatement())
    return self(p);

  if (auto p = ctx->expressionStatement())
    return self(p);

  if (auto p = ctx->jumpStatement())
    return self(p);

  ABORT();
}

CompoundStmt*
Ast2Asg::operator()(ast::CompoundStatementContext* ctx)
{
  auto ret = make<CompoundStmt>();

  if (auto p = ctx->blockItemList()) {
    Symtbl localDecls(self);

    for (auto&& i : p->blockItem()) {
      if (auto q = i->declaration()) {
        auto sub = make<DeclStmt>();
        sub->decls = self(q);
        ret->subs.push_back(sub);
      }

      else if (auto q = i->statement())
        ret->subs.push_back(self(q));

      else
        ABORT();
    }
  }

  return ret;
}

Stmt*
Ast2Asg::operator()(ast::ExpressionStatementContext* ctx)
{
  if (auto p = ctx->expression()) {
    auto ret = make<ExprStmt>();
    ret->expr = self(p);
    return ret;
  }

  return make<NullStmt>();
}

Stmt*
Ast2Asg::operator()(ast::JumpStatementContext* ctx)
{
  if (ctx->Return()) {
    auto ret = make<ReturnStmt>();
    ret->func = mCurrentFunc;
    if (auto p = ctx->expression())
      ret->expr = self(p);
    return ret;
  }

  ABORT();
}

//==============================================================================
// 声明
//==============================================================================

std::vector<Decl*>
Ast2Asg::operator()(ast::DeclarationContext* ctx)
{
  std::vector<Decl*> ret;

  auto specs = self(ctx->declarationSpecifiers());

  if (auto p = ctx->initDeclaratorList()) {
    for (auto&& j : p->initDeclarator())
      ret.push_back(self(j, specs));
  }

  // 如果 initDeclaratorList 为空则这行声明语句无意义
  return ret;
}

FunctionDecl*
Ast2Asg::operator()(ast::FunctionDefinitionContext* ctx)
{
  auto ret = make<FunctionDecl>();
  mCurrentFunc = ret;

  auto type = make<Type>();
  ret->type = type;

  auto sq = self(ctx->declarationSpecifiers());
  type->spec = sq.first, type->qual = sq.second;

  auto [texp, name] = self(ctx->directDeclarator(), nullptr);
  auto funcType = texp->dcst<FunctionType>();
  if (!funcType) {
    // 如果 directDeclarator 没有产生 FunctionType，则创建一个
    funcType = make<FunctionType>();
    funcType->sub = texp;
  }
  type->texp = funcType;
  ret->name = std::move(name);

  Symtbl localDecls(self);

  // 函数定义在签名之后就加入符号表，以允许递归调用
  (*mSymtbl)[ret->name] = ret;

  // 为参数创建 VarDecl 并加入符号表
  size_t paramCount = funcType->params.size();
  for (size_t i = 0; i < paramCount; ++i) {
    auto paramDecl = make<VarDecl>();
    paramDecl->type = funcType->params[i];
    // 设置参数名并加入符号表
    if (i < mParamNames.size() && !mParamNames[i].empty()) {
      paramDecl->name = mParamNames[i];
      (*mSymtbl)[paramDecl->name] = paramDecl;
    }
    ret->params.push_back(paramDecl);
  }

  // 处理 K&R 风格的 declarationList（如果有）
  if (auto declList = ctx->declarationList()) {
    auto decls = self(declList);
    // K&R 风格：declarationList 中的声明用于补充参数类型信息
    // 这里简化处理：将声明加入符号表
    for (auto decl : decls) {
      (*mSymtbl)[decl->name] = decl;
    }
  }

  ret->body = self(ctx->compoundStatement());

  return ret;
}

std::vector<Decl*>
Ast2Asg::operator()(ast::DeclarationListContext* ctx)
{
  std::vector<Decl*> ret;

  // 递归处理：declarationList 可能包含另一个 declarationList
  if (auto subList = ctx->declarationList()) {
    ret = self(subList);
  }

  // 添加当前层的 declaration
  if (auto decl = ctx->declaration()) {
    auto sub = self(decl);
    ret.insert(ret.end(),
               std::make_move_iterator(sub.begin()),
               std::make_move_iterator(sub.end()));
  }

  return ret;
}

Decl*
Ast2Asg::operator()(ast::InitDeclaratorContext* ctx, SpecQual sq)
{
  auto [texp, name] = self(ctx->declarator(), nullptr);
  Decl* ret;

  if (auto funcType = texp->dcst<FunctionType>()) {
    auto fdecl = make<FunctionDecl>();
    auto type = make<Type>();
    fdecl->type = type;

    type->spec = sq.first;
    type->qual = sq.second;
    type->texp = funcType;

    fdecl->name = std::move(name);
    for (auto p : funcType->params) {
      auto paramDecl = make<VarDecl>();
      paramDecl->type = p;
      fdecl->params.push_back(paramDecl);
    }

    if (ctx->initializer())
      ABORT();
    fdecl->body = nullptr;

    ret = fdecl;
  }

  else {
    auto vdecl = make<VarDecl>();
    auto type = make<Type>();
    vdecl->type = type;

    type->spec = sq.first;
    type->qual = sq.second;
    type->texp = texp;
    vdecl->name = std::move(name);

    if (auto p = ctx->initializer())
      vdecl->init = self(p);
    else
      vdecl->init = nullptr;

    ret = vdecl;
  }

  // 这个实现允许符号重复定义，新定义会取代旧定义
  (*mSymtbl)[ret->name] = ret;
  return ret;
}

} // namespace asg

