#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 常量传播与常量折叠合并 Pass
/// 内部循环执行常量传播和常量折叠，直到没有新的变换发生
class ConstantPropagation : public llvm::PassInfoMixin<ConstantPropagation>
{
public:
  explicit ConstantPropagation(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
