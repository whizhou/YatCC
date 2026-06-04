#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 强度削弱 Pass
/// 将高计算复杂度的指令转化为低复杂度的指令
/// 例如：
///   mul x, 8   -> shl x, 3
///   udiv x, 8  -> lshr x, 3
///   sdiv x, 8  -> 带偏置的 ashr
///   urem x, 8  -> and x, 7
///   srem x, 8  -> 带符号修正的 and
///   mul x, C   -> shift + add/sub 组合
class StrengthReduction
  : public llvm::PassInfoMixin<StrengthReduction>
{
public:
  explicit StrengthReduction(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
