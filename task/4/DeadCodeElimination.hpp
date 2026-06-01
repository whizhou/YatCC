#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 死代码消除 Pass
/// 基于 use-def 链分析，删除无副作用且无使用者的指令
/// 对于函数调用，检查函数是否有副作用以扩大消除范围
class DeadCodeElimination : public llvm::PassInfoMixin<DeadCodeElimination>
{
public:
  explicit DeadCodeElimination(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
