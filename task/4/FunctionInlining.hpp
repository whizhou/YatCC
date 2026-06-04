#pragma once

#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 函数内联 Pass
/// 将不参与递归调用的函数内联到调用处，消除函数调用开销，
/// 并为后续优化（如 CSE、常量传播）提供更大的优化窗口。
///
/// 算法概要：
/// 1. 构造函数调用图（Call Graph）
/// 2. 检测调用图中的环，不在环中的函数可以内联
/// 3. 克隆被调用函数的基本块，嵌入调用处
/// 4. 替换函数参数为实际传入参数
/// 5. 处理返回值（PHI 节点合并多返回点）
/// 6. 删除无调用者的函数
class FunctionInlining : public llvm::PassInfoMixin<FunctionInlining>
{
public:
  explicit FunctionInlining(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
