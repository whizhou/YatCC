#pragma once

#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 死存储消除 Pass
/// 基于内存位置分析，删除被后续存储覆盖而从未被读取的死存储
/// 同时删除仅被存储而从未被加载的 alloca 上的所有 store
/// 实现思路类似于 DCE：
/// 1. 在基本块内反向遍历，跟踪已被后续 store 覆盖的内存位置
/// 2. 若 store 的目标位置已被覆盖且未被读取，则该 store 为死存储
/// 3. 若 alloca 仅被 store 而从未被 load，则所有对其的 store 均为死存储
class DeadStoreElimination : public llvm::PassInfoMixin<DeadStoreElimination>
{
public:
  explicit DeadStoreElimination(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
