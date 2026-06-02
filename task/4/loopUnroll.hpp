#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 循环展开 (Loop Unrolling) Pass
/// 对已知迭代次数且次数较小的循环执行完全展开，消除循环控制开销，
/// 并为后续优化（常量传播、指令合并等）暴露更多机会。
///
/// 算法概要：
/// 1. 对每个函数获取 LoopInfo 和 DominatorTree 分析结果
/// 2. 按后序遍历所有循环（内层循环优先处理，因为内层展开后外层更易处理）
/// 3. 对每个循环：
///    a. 识别基本归纳变量（header 中的 PHI 节点，步长为常量）
///    b. 从退出条件计算迭代次数（trip count）
///    c. 若迭代次数为常量且 ≤ 阈值，执行完全展开：
///       - 将循环体克隆 tripCount 份
///       - 每份副本中用常数值替换归纳变量
///       - 修复 PHI 节点和分支目标
///       - 连接各份副本形成直线代码
///    d. 删除原始循环基本块
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
