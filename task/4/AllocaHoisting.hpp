#pragma once

#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// AllocaHoisting Pass
/// 将非入口基本块中的 alloca 指令移动到函数入口块的开头。
///
/// 问题背景：FunctionInlining 将含有数组 alloca 的函数内联到循环体后，
/// alloca 位于循环内的非入口块中，每次迭代都会执行，导致栈空间不断增长，
/// 最终栈溢出（SIGSEGV）。
///
/// 解决方案：将所有 alloca 移动到入口块，确保每个 alloca 只执行一次。
class AllocaHoisting : public llvm::PassInfoMixin<AllocaHoisting>
{
public:
  explicit AllocaHoisting(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
