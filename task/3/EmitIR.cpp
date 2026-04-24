#include "EmitIR.hpp"
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <iostream>

#define self (*this)

using namespace asg;

EmitIR::EmitIR(Obj::Mgr& mgr, llvm::LLVMContext& ctx, llvm::StringRef mid)
  : mMgr(mgr)
  , mMod(mid, ctx)
  , mCtx(ctx)
  , mIntTy(llvm::Type::getInt32Ty(ctx))
  , mCurIrb(std::make_unique<llvm::IRBuilder<>>(ctx))
  , mCtorTy(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false))
  , mCurFunc(nullptr)
{
}

llvm::Module&
EmitIR::operator()(asg::TranslationUnit* tu)
{
  for (auto&& i : tu->decls)
    self(i);
  return mMod;
}

//==============================================================================
// 类型
//==============================================================================

llvm::Type*
EmitIR::operator()(const Type* type)
{
  if (type->texp == nullptr) {
    switch (type->spec) {
      case Type::Spec::kInt:
        return llvm::Type::getInt32Ty(mCtx);
      // TODO: 在此添加对更多基础类型的处理
      default:
        ABORT();
    }
  }

  Type subt;
  subt.spec = type->spec;
  subt.qual = type->qual;
  subt.texp = type->texp->sub;

  // TODO: 在此添加对指针类型、数组类型和函数类型的处理

  if (auto p = type->texp->dcst<PointerType>()) {
    auto pointee = self(&subt);
    return pointee->getPointerTo();
  }

  if (auto p = type->texp->dcst<ArrayType>()) {
    auto elemTy = self(&subt);
    return llvm::ArrayType::get(elemTy, p->len);
  }

  if (auto p = type->texp->dcst<FunctionType>()) {
    std::vector<llvm::Type*> pty;
    // TODO: 在此添加对函数参数类型的处理
    for (auto param : p->params) {
      pty.push_back(self(param));
    }
    return llvm::FunctionType::get(self(&subt), std::move(pty), false);
  }

  ABORT();
}

//==============================================================================
// 表达式
//==============================================================================

llvm::Value*
EmitIR::operator()(Expr* obj)
{
  // TODO: 在此添加对更多表达式处理的跳转
  if (auto p = obj->dcst<IntegerLiteral>())
    return self(p);

  if (auto p = obj->dcst<InitListExpr>())
    return self(p);

  if (auto p = obj->dcst<DeclRefExpr>())
    return self(p);

  if (auto p = obj->dcst<ImplicitCastExpr>())
    return self(p);

  if (auto p = obj->dcst<BinaryExpr>())
    return self(p);

  if (auto p = obj->dcst<UnaryExpr>())
    return self(p);

  if (auto p = obj->dcst<ParenExpr>())
    return self(p);

  if (auto p = obj->dcst<CallExpr>())
    return self(p);

  ABORT();
}

llvm::Constant*
EmitIR::operator()(IntegerLiteral* obj)
{
  return llvm::ConstantInt::get(self(obj->type), obj->val);
}

// TODO: 在此添加对更多表达式类型的处理

llvm::Value*
EmitIR::operator()(UnaryExpr* obj)
{
  /*
  struct UnaryExpr : Expr
  {
    enum Op
    {
      kINVALID,
      kPos,
      kNeg,
      kNot
    };

    Op op{ kINVALID };
    Expr* sub{ nullptr };
  };
  */
  llvm::Value* sub = self(obj->sub);

  auto& irb = *mCurIrb;

  switch (obj->op) {

    case UnaryExpr::kPos:
      return sub;
    
    case UnaryExpr::kNeg:
      return irb.CreateNeg(sub);

    default:
      ABORT();
  }
}

llvm::Value*
EmitIR::operator()(BinaryExpr* obj)
{
  llvm::Value *lftVal, *rhtVal;

  lftVal = self(obj->lft);
  rhtVal = self(obj->rht);

  auto& irb = *mCurIrb;
  switch (obj->op) {

    case BinaryExpr::kIndex: {
      // 左操作数是指针类型（经过array-to-pointer decay）
      // GEP需要的是指针所指向的元素类型，而不是指针类型本身
      Type subt;
      subt.spec = obj->lft->type->spec;
      subt.qual = obj->lft->type->qual;
      subt.texp = obj->lft->type->texp->sub;  // 获取指针指向的类型
      auto elemTy = self(&subt);
      return irb.CreateGEP(elemTy, lftVal, rhtVal);
    }

    case BinaryExpr::kAssign: {
      irb.CreateStore(rhtVal, lftVal);
      return rhtVal;
    }

    case BinaryExpr::kAdd:
      return irb.CreateAdd(lftVal, rhtVal);

    case BinaryExpr::kSub:
      return irb.CreateSub(lftVal, rhtVal);

    case BinaryExpr::kMul:
      return irb.CreateMul(lftVal, rhtVal);

    case BinaryExpr::kDiv:
      return irb.CreateSDiv(lftVal, rhtVal);

    case BinaryExpr::kMod:
      return irb.CreateSRem(lftVal, rhtVal);

    default:
      ABORT();
  }
}

llvm::Value*
EmitIR::operator()(asg::ParenExpr* obj)
{
  /*
  struct ParenExpr : Expr
  {
    Expr* sub{ nullptr };
  };
  */
  return self(obj->sub);
}

llvm::Value*
EmitIR::operator()(ImplicitCastExpr* obj)
{
  auto sub = self(obj->sub);

  auto& irb = *mCurIrb;
  switch (obj->kind) {
    case ImplicitCastExpr::kLValueToRValue: {
      auto ty = self(obj->sub->type);
      auto loadVal = irb.CreateLoad(ty, sub);
      return loadVal;
    }

    case ImplicitCastExpr::kArrayToPointerDecay: {
      // Array decays to pointer to first element
      // sub is a pointer to the array, we need to get a pointer to the first element
      std::vector<llvm::Value *> indices{
        irb.getInt64(0), irb.getInt64(0)
      };
      return irb.CreateGEP(self(obj->sub->type), sub, indices);
    }

    case ImplicitCastExpr::kFunctionToPointerDecay: {
      return sub;
    }

    default:
      ABORT();
  }
}

llvm::Constant*
EmitIR::operator()(ImplicitInitExpr* obj)
{
  // ImplicitInitExpr represents zero-initialization
  return llvm::Constant::getNullValue(self(obj->type));
}

llvm::Value*
EmitIR::operator()(DeclRefExpr* obj)
{
  // 在LLVM IR层面，左值体现为返回指向值的指针
  // 在ImplicitCastExpr::kLValueToRValue中发射load指令从而变成右值
  return reinterpret_cast<llvm::Value*>(obj->decl->any);
}

llvm::Value*
EmitIR::operator()(InitListExpr* obj)
{
  /*
  struct InitListExpr : Expr
  {
    std::vector<Expr*> list;
  };
  */
  ABORT();
}

llvm::Value*
EmitIR::operator()(asg::CallExpr* obj)
{
  /*
  struct CallExpr : Expr
  {
    Expr* head{ nullptr };
    std::vector<Expr*> args;
  };
  */
  // Get the function pointer
  auto funcVal = self(obj->head);

  // Evaluate arguments
  std::vector<llvm::Value*> args;
  for (auto arg : obj->args) {
    args.push_back(self(arg));
  }

  // Create the call instruction
  auto& irb = *mCurIrb;
  if (auto func = llvm::dyn_cast<llvm::Function>(funcVal)) {
    return irb.CreateCall(func, args);
  }
  ABORT();
}

//==============================================================================
// 语句
//==============================================================================

void
EmitIR::operator()(Stmt* obj)
{
  // TODO: 在此添加对更多Stmt类型的处理的跳转

  if (auto p = obj->dcst<CompoundStmt>())
    return self(p);

  if (auto p = obj->dcst<ReturnStmt>())
    return self(p);

  if (auto p = obj->dcst<DeclStmt>())
    return self(p);

  if (auto p = obj->dcst<ExprStmt>())
    return self(p);

  ABORT();
}

// TODO: 在此添加对更多Stmt类型的处理

void
EmitIR::operator()(CompoundStmt* obj)
{
  // TODO: 可以在此添加对符号重名的处理
  for (auto&& stmt : obj->subs)
    self(stmt);
}

void
EmitIR::operator()(DeclStmt* obj)
{
  /*
  struct DeclStmt : Stmt
  {
    std::vector<Decl*> decls;
  };
  */

  for (auto&& decl : obj->decls)
    self(decl);

}

void
EmitIR::operator()(ExprStmt* obj)
{
  /*
  struct ExprStmt : Stmt
  {
    Expr* expr{ nullptr };
  };
  */
  if (obj->expr != nullptr)
    self(obj->expr);
}

void
EmitIR::operator()(ReturnStmt* obj)
{
  auto& irb = *mCurIrb;

  llvm::Value* retVal;
  if (!obj->expr)
    retVal = nullptr;
  else
    retVal = self(obj->expr);

  mCurIrb->CreateRet(retVal);

  auto exitBb = llvm::BasicBlock::Create(mCtx, "return_exit", mCurFunc);
  mCurIrb->SetInsertPoint(exitBb);
}

//==============================================================================
// 声明
//==============================================================================

void
EmitIR::operator()(Decl* obj)
{
  // TODO: 添加变量声明处理的跳转
  if (auto p = obj->dcst<VarDecl>())
    return self(p);

  if (auto p = obj->dcst<FunctionDecl>())
    return self(p);

  ABORT();
}

// TODO: 添加变量声明的处理

void
EmitIR::operator()(FunctionDecl* obj)
{
  /*
  struct FunctionDecl : Decl
  {
    std::vector<Decl*> params;
    CompoundStmt* body{ nullptr };
  };
  */
  // 创建函数
  auto fty = llvm::dyn_cast<llvm::FunctionType>(self(obj->type));
  auto func = llvm::Function::Create(
    fty, llvm::GlobalVariable::ExternalLinkage, obj->name, mMod);

  obj->any = func;

  if (obj->body == nullptr)
    return;
  auto entryBb = llvm::BasicBlock::Create(mCtx, "entry", func);
  mCurIrb->SetInsertPoint(entryBb);
  auto& entryIrb = *mCurIrb;

  // TODO: 添加对函数参数的处理
  auto argIter = func->arg_begin();
  for (auto param : obj->params) {
    // 设置 LLVM 参数名称
    argIter->setName(param->name);

    // 为参数创建 alloca，使得参数可以被修改
    auto ty = self(param->type);
    auto alloca = entryIrb.CreateAlloca(ty, nullptr, param->name);

    // 将参数值存储到 alloca 中
    entryIrb.CreateStore(argIter, alloca);

    // 设置 param->any 以便 DeclRefExpr 可以引用
    param->any = alloca;

    ++argIter;
  }

  // 翻译函数体
  mCurFunc = func;
  self(obj->body);
  mCurFunc = nullptr;  // 重置 mCurFunc
  auto& exitIrb = *mCurIrb;

  if (fty->getReturnType()->isVoidTy())
    exitIrb.CreateRetVoid();
  else
    exitIrb.CreateUnreachable();
}

void
EmitIR::trans_init(llvm::Value* val, Expr* obj)
{
  auto& irb = *mCurIrb;

  // Handle integer literal initialization
  if (auto p = obj->dcst<IntegerLiteral>()) {
    auto initVal = llvm::ConstantInt::get(self(p->type), p->val);
    irb.CreateStore(initVal, val);
    return;
  }

  // Handle implicit initialization (zero-init)
  if (auto p = obj->dcst<ImplicitInitExpr>()) {
    // auto initVal = self(p);
    // irb.CreateStore(initVal, val);
    return;
  }

  // Handle initializer list for arrays
  if (auto p = obj->dcst<InitListExpr>()) {
    auto aty = llvm::dyn_cast<llvm::ArrayType>(self(obj->type));
    if (!aty) {
      ABORT();
    }

    // Initialize each element
    for (size_t i = 0; i < p->list.size(); ++i) {
      std::vector<llvm::Value *> indices{
        irb.getInt64(0), irb.getInt64(i)
      };
      auto elemPtr = irb.CreateGEP(aty, val, indices);
      trans_init(elemPtr, p->list[i]);
    }
    return;
  }

  // Handle other expressions (e.g., array subscript expressions)
  auto initVal = self(obj);
  if (initVal) {
    irb.CreateStore(initVal, val);
    return;
  }

  ABORT();
}

void
EmitIR::operator()(VarDecl* obj)
{
  auto ty = self(obj->type);

  if (mCurFunc != nullptr) {
    // 局部变量
    auto& irb = *mCurIrb;
    auto alloca = irb.CreateAlloca(ty, nullptr, obj->name);

    obj->any = alloca;

    if (obj->init != nullptr) {
      llvm::Value* zeroInit = llvm::Constant::getNullValue(ty);
      irb.CreateStore(zeroInit, alloca);
      trans_init(alloca, obj->init);
    }
  }
  else {
    // 全局变量
    auto gvar = new llvm::GlobalVariable(
      mMod, ty, false, llvm::GlobalVariable::ExternalLinkage, nullptr, obj->name);

    obj->any = gvar;

    // 默认初始化为 0
    gvar->setInitializer(llvm::Constant::getNullValue(ty));

    if (obj->init == nullptr)
      return;

    // 创建构造函数用于初始化
    mCurFunc = llvm::Function::Create(
      mCtorTy, llvm::GlobalVariable::PrivateLinkage, "ctor_" + obj->name, mMod);
    llvm::appendToGlobalCtors(mMod, mCurFunc, 65535);

    auto entryBb = llvm::BasicBlock::Create(mCtx, "entry", mCurFunc);
    mCurIrb->SetInsertPoint(entryBb);
    trans_init(gvar, obj->init);
    mCurIrb->CreateRet(nullptr);
    mCurFunc = nullptr;  // 重置 mCurFunc
  }
}

