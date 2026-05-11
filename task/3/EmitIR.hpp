#include "asg.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <vector>

class EmitIR
{
public:
  Obj::Mgr& mMgr;
  llvm::Module mMod;

  EmitIR(Obj::Mgr& mgr, llvm::LLVMContext& ctx, llvm::StringRef mid = "-");

  llvm::Module& operator()(asg::TranslationUnit* tu);

private:
  llvm::LLVMContext& mCtx;

  llvm::Type* mIntTy;
  llvm::FunctionType* mCtorTy;

  llvm::Function* mCurFunc;
  std::unique_ptr<llvm::IRBuilder<>> mCurIrb;

  /// 当前循环的结束块（用于 break 跳转）
  llvm::BasicBlock* mCurLoopEndBB{ nullptr };
  /// 当前循环的条件块（用于 continue 跳转）
  llvm::BasicBlock* mCurLoopCondBB{ nullptr };

  /// 复合语句栈帧保存栈：每进入一个复合语句，将 llvm.stacksave 的返回值
  /// 压入此栈；在 return/break/continue 等提前退出路径上，遍历此栈并
  /// 依次调用 llvm.stackrestore 以回收所有嵌套复合语句的栈空间。
  std::vector<llvm::Value*> mSavedStacks;

  /// 对于每个嵌套循环层级，记录循环体开始执行时 mSavedStacks 的大小。
  /// break/continue 只应恢复在此之后压入的 stacksave（即循环体内部的
  /// CompoundStmt），而不应恢复外层循环或函数体的栈帧。
  std::vector<size_t> mLoopSavedStackSize;

  //============================================================================
  // 类型
  //============================================================================

  llvm::Type* operator()(const asg::Type* type);

  //============================================================================
  // 表达式
  //============================================================================

  llvm::Value* operator()(asg::Expr* obj);

  llvm::Constant* operator()(asg::IntegerLiteral* obj);

  // TODO: 添加表达式处理相关声明

  llvm::Value* operator()(asg::UnaryExpr* obj);

  llvm::Value* operator()(asg::BinaryExpr* obj);

  llvm::Value* operator()(asg::ParenExpr* obj);

  llvm::Value* operator()(asg::ImplicitCastExpr* obj);

  llvm::Value* operator()(asg::DeclRefExpr* obj);

  llvm::Value* operator()(asg::InitListExpr* obj);

  llvm::Constant* operator()(asg::ImplicitInitExpr* obj);

  llvm::Value* operator()(asg::CallExpr* obj);

  //============================================================================
  // 语句
  //============================================================================

  void operator()(asg::Stmt* obj);

  void operator()(asg::CompoundStmt* obj);

  void operator()(asg::ReturnStmt* obj);

  // TODO: 添加语句处理相关声明

  void operator()(asg::DeclStmt* obj);

  void operator()(asg::ExprStmt* obj);

  void operator()(asg::IfStmt* obj);

  void operator()(asg::WhileStmt* obj);

  void operator()(asg::BreakStmt* obj);

  void operator()(asg::ContinueStmt* obj);

  //============================================================================
  // 声明
  //============================================================================

  void operator()(asg::Decl* obj);

  void operator()(asg::FunctionDecl* obj);

  // TODO: 添加声明处理相关声明

  void trans_init(llvm::Value* val, asg::Expr* obj);
  void operator()(asg::VarDecl* obj);
};
