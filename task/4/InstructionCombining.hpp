#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 指令合并 Pass
/// 利用算术代数规则简化指令，合并常量运算链
/// 例如：add(add(x, 1), 1) -> add(x, 2)
class InstructionCombining
  : public llvm::PassInfoMixin<InstructionCombining>
{
public:
  explicit InstructionCombining(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
