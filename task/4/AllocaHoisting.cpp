#include "AllocaHoisting.hpp"

#include <llvm/IR/Instructions.h>
#include <vector>

using namespace llvm;

/// 将函数中非入口块的 alloca 指令移动到入口块的开头
static int
hoistAllocas(Function& func)
{
  if (func.isDeclaration())
    return 0;

  BasicBlock& entry = func.getEntryBlock();
  int hoisted = 0;

  // 收集非入口块中的所有 alloca（避免迭代器失效）
  std::vector<AllocaInst*> allocas;
  for (BasicBlock& bb : func) {
    if (&bb == &entry)
      continue;
    for (Instruction& inst : bb) {
      if (auto* ai = dyn_cast<AllocaInst>(&inst))
        allocas.push_back(ai);
    }
  }

  if (allocas.empty())
    return 0;

  // 找到入口块中第一个非 PHI 指令的位置
  Instruction* insertPos = entry.getFirstNonPHI();

  for (AllocaInst* ai : allocas) {
    ai->moveBefore(insertPos);
    ++hoisted;
  }

  return hoisted;
}

PreservedAnalyses
AllocaHoisting::run(Module& mod, ModuleAnalysisManager& mam)
{
  int totalHoisted = 0;

  for (Function& func : mod)
    totalHoisted += hoistAllocas(func);

  mOut << "AllocaHoisting running...\n"
       << "Hoisted " << totalHoisted << " allocas to entry blocks\n";

  if (totalHoisted == 0)
    return PreservedAnalyses::all();

  // 移动 alloca 不改变控制流，只改变指令位置
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
