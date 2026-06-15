#pragma once

#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 循环展开 Pass
/// 将具有常量迭代次数的循环展开多次，减少循环开销（分支指令、归纳变量更新），
/// 并增加指令级并行性，便于后续优化（如指令调度、向量化）。
///
/// 算法概要：
/// 1. 识别具有常量迭代次数的循环（通过 SCEV 分析或简单的归纳变量分析）
/// 2. 根据展开因子复制循环体
/// 3. 处理余数迭代（epilogue loop）
/// 4. 更新 PHI 节点和分支条件
class LoopUnroll : public llvm::PassInfoMixin<LoopUnroll>
{
public:
  explicit LoopUnroll(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
