#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 公共子表达式消除 Pass
/// 在基本块内，向后搜索窗口范围内的指令，若发现相同的指令则替换为已有的结果
/// 时间复杂度 O(kN)，k 为窗口大小，N 为指令总数
class CommonSubexpressionElimination
  : public llvm::PassInfoMixin<CommonSubexpressionElimination>
{
public:
  explicit CommonSubexpressionElimination(llvm::raw_ostream& out,
                                          unsigned windowSize = 256)
    : mOut(out)
    , mWindowSize(windowSize)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
  unsigned mWindowSize;
};
