#include "DeadCodeElimination.hpp"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>

using namespace llvm;

/// 检查全局变量是否被任何 load 指令读取
/// 如果全局变量只有 store 而没有 load，则对它的写入是死存储
static bool
hasLoadsFromGlobal(GlobalVariable* GV)
{
  for (User* U : GV->users()) {
    if (isa<LoadInst>(U))
      return true;
    if (auto* GEP = dyn_cast<GetElementPtrInst>(U)) {
      for (User* GEPU : GEP->users()) {
        if (isa<LoadInst>(GEPU))
          return true;
      }
    }
  }
  return false;
}

/// 判断指令是否有副作用（不可安全删除）
/// 对于函数调用，检查函数属性以判断是否有副作用
static bool
hasSideEffects(Instruction* inst)
{
  // 终止指令不可删除
  if (inst->isTerminator())
    return true;

  // store 指令通常有副作用（修改内存），但若目标是全局变量
  // 且该全局变量从未被 load，则可以安全删除
  if (auto* SI = dyn_cast<StoreInst>(inst)) {
    Value* ptr = SI->getPointerOperand();
    // 如果 store 目标是全局变量且该全局变量从未被读取，
    // 则这个 store 是死存储，无副作用
    if (auto* GV = dyn_cast<GlobalVariable>(ptr)) {
      if (!hasLoadsFromGlobal(GV))
        return false;
    }
    return true;
  }

  // fence 指令有副作用
  if (isa<FenceInst>(inst))
    return true;

  // volatile 指令有副作用
  if (inst->isVolatile())
    return true;

  // 函数调用：检查函数是否有副作用
  if (auto* call = dyn_cast<CallBase>(inst)) {
    Function* callee = call->getCalledFunction();

    // 间接调用（函数指针），保守地认为有副作用
    if (!callee)
      return true;

    // 内联汇编有副作用
    if (call->hasOperandBundles())
      return true;

    // 检查函数属性：若函数不访问内存或只读内存，则无副作用
    // 注意：结果未被使用时，只读调用也可以删除
    if (callee->doesNotAccessMemory())
      return false;
    if (callee->onlyReadsMemory())
      return false;

    // 未知函数或有写内存行为的函数，保守地认为有副作用
    return true;
  }

  // 其他指令（算术、比较、GEP、PHI、select 等）无副作用
  return false;
}

/// 死代码消除的核心算法
/// 使用 worklist 迭代删除无用指令：
/// 1. 收集所有 use_empty() 且无副作用的指令
/// 2. 删除指令，并检查其操作数是否因此变为死代码
/// 3. 重复直到 worklist 为空
static int
eliminateDeadCode(Module& mod)
{
  int eliminated = 0;

  // worklist: 待处理的死代码指令
  std::vector<Instruction*> worklist;

  // 第一遍扫描：收集所有无使用者且无副作用的指令
  for (auto& func : mod) {
    for (auto& bb : func) {
      for (auto& inst : bb) {
        if (inst.use_empty() && !hasSideEffects(&inst))
          worklist.push_back(&inst);
      }
    }
  }

  // 迭代处理 worklist
  while (!worklist.empty()) {
    Instruction* inst = worklist.back();
    worklist.pop_back();

    // 可能已被之前的迭代删除
    if (inst->getParent() == nullptr)
      continue;

    // 再次确认：指令可能在处理过程中被其他指令引用
    // （例如 PHI 节点合并），需要重新检查
    if (!inst->use_empty())
      continue;

    // 收集操作数，在删除前保存
    std::vector<Value*> operands;
    for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
      Value* op = inst->getOperand(i);
      if (op)
        operands.push_back(op);
    }

    // 从父基本块中删除指令
    inst->eraseFromParent();
    ++eliminated;

    // 检查操作数是否因此变为死代码
    for (Value* op : operands) {
      if (auto* opInst = dyn_cast<Instruction>(op)) {
        if (opInst->use_empty() && !hasSideEffects(opInst)) {
          worklist.push_back(opInst);
        }
      }
    }
  }

  return eliminated;
}

PreservedAnalyses
DeadCodeElimination::run(Module& mod, ModuleAnalysisManager& mam)
{
  int eliminated = eliminateDeadCode(mod);

  mOut << "DeadCodeElimination running...\n"
       << "Eliminated " << eliminated << " dead instructions\n";

  if (eliminated == 0)
    return PreservedAnalyses::all();

  // DCE 可能删除了分支指令的目标，从而改变了 CFG
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
