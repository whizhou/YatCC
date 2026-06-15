#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 循环无关变量移动 (LICM) Pass
/// 将循环中不随迭代变化且可安全提升的指令移动到循环前导块 (preheader) 中，
/// 以减少循环体内的冗余计算。
///
/// 算法概要：
/// 1. 按后序遍历所有循环（内层循环优先处理）
/// 2. 对每个循环，迭代地识别循环无关指令：
///    - 所有操作数都是常量或在循环外定义
///    - 对于 load 指令，额外检查所加载的内存位置在循环内不被修改
///    - 对于 call 指令，检查被调用函数是否无副作用（只读内存或不访问内存）
/// 3. 对循环无关指令，检查是否可以安全提升：
///    - 可投机执行（不会触发陷阱），或
///    - 指令所在基本块支配所有循环出口基本块
/// 4. 将满足条件的指令移动到 preheader 的 terminator 之前
/// 5. 重复步骤 2-4 直到没有新指令可提升（因为提升一条指令可能使其他指令变为循环无关）
class LICM : public llvm::PassInfoMixin<LICM>
{
public:
  explicit LICM(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
