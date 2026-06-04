#include "DeadStoreElimination.hpp"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>

using namespace llvm;

/// 获取指针的规范形式（剥离指针类型转换）
/// 两个经过此函数处理后相等的指针一定互为别名
static Value*
canonicalPointer(Value* P)
{
  return P->stripPointerCasts();
}

/// 检查一个 alloca 是否从被来没有被 load 过
/// 递归检查所有使用该 alloca 的指令（包括 GEP 和 bitcast），
/// 判断是否存在 load 指令
static bool
isAllocaNeverLoaded(AllocaInst* AI)
{
  // 使用工作列表递归查找所有用户
  SmallVector<Value*, 16> worklist;
  SmallPtrSet<Value*, 16> visited;

  for (User* U : AI->users()) {
    if (visited.insert(U).second)
      worklist.push_back(U);
  }

  while (!worklist.empty()) {
    Value* V = worklist.pop_back_val();

    if (isa<LoadInst>(V))
      return false;

    // 如果 alloca 被传递给函数调用，被调用函数可能通过指针读取它
    if (auto* CB = dyn_cast<CallBase>(V)) {
      if (!CB->doesNotAccessMemory())
        return false;
    }

    // 通过 GEP 和 bitcast 间接访问，继续追踪其用户
    if (isa<GetElementPtrInst>(V) || isa<BitCastInst>(V) ||
        isa<AddrSpaceCastInst>(V)) {
      for (User* U : V->users()) {
        if (visited.insert(U).second)
          worklist.push_back(U);
      }
    }
  }

  return true;
}

/// 收集 alloca 中从未被 load 的死存储
/// 递归遍历 alloca 的所有使用链（包括多层 GEP 和 bitcast），
/// 收集所有 store 指令并删除
/// 返回收集到的死存储数量
static int
eliminateAllocaDeadStores(Function& func)
{
  int eliminated = 0;

  // 收集所有 alloca 指令
  SmallVector<AllocaInst*, 16> allocas;
  for (auto& bb : func) {
    for (auto& inst : bb) {
      if (auto* AI = dyn_cast<AllocaInst>(&inst))
        allocas.push_back(AI);
    }
  }

  // 对每个从未被 load 的 alloca，删除所有对其的 store
  for (AllocaInst* AI : allocas) {
    if (!isAllocaNeverLoaded(AI))
      continue;

    // 使用工作列表递归遍历所有用户，收集所有 store
    SmallVector<StoreInst*, 16> deadStores;
    SmallVector<Value*, 16> worklist;
    SmallPtrSet<Value*, 16> visited;

    for (User* U : AI->users()) {
      if (visited.insert(U).second)
        worklist.push_back(U);
    }

    while (!worklist.empty()) {
      Value* V = worklist.pop_back_val();

      if (auto* SI = dyn_cast<StoreInst>(V)) {
        deadStores.push_back(SI);
      } else if (isa<GetElementPtrInst>(V) || isa<BitCastInst>(V) ||
                 isa<AddrSpaceCastInst>(V)) {
        // 通过 GEP/bitcast 间接访问，继续追踪其用户
        for (User* U : V->users()) {
          if (visited.insert(U).second)
            worklist.push_back(U);
        }
      }
    }

    // 删除所有死存储
    for (StoreInst* SI : deadStores) {
      SI->eraseFromParent();
      ++eliminated;
    }
  }

  return eliminated;
}

/// 基本块内的死存储消除
/// 反向遍历基本块，跟踪已被后续 store 覆盖的内存位置
/// 若 store 的目标位置已被覆盖且未被读取，则该 store 为死存储
static int
eliminateIntraBlockDeadStores(Function& func)
{
  int eliminated = 0;

  for (auto& bb : func) {
    // killed: 已被后续 store 覆盖的内存位置（规范指针）
    SmallPtrSet<Value*, 16> killed;
    // dead: 待删除的死存储
    SmallVector<StoreInst*, 16> dead;

    for (auto it = bb.rbegin(); it != bb.rend(); ++it) {
      Instruction& inst = *it;

      if (auto* SI = dyn_cast<StoreInst>(&inst)) {
        // 易失性存储与原子存储具有不可忽略的副作用，保守跳过
        if (SI->isVolatile() || SI->isAtomic()) {
          killed.clear();
          continue;
        }

        Value* ptr = canonicalPointer(SI->getPointerOperand());

        if (killed.count(ptr)) {
          // 该 store 的写入在后续被另一个 store 覆盖，之间没有读取
          dead.push_back(SI);
        }
        killed.insert(ptr);
      } else if (auto* LI = dyn_cast<LoadInst>(&inst)) {
        // 易失性加载具有不可忽略的副作用，保守处理
        if (LI->isVolatile()) {
          killed.clear();
          continue;
        }

        Value* ptr = canonicalPointer(LI->getPointerOperand());
        // load 读取了该位置，回溯中的 store 变为活跃
        killed.erase(ptr);
      } else if (inst.mayReadFromMemory()) {
        // 任何可能读取内存的指令（如函数调用），保守假设
        // 它可能读取任何被跟踪的内存位置
        killed.clear();
      }
      // 仅写入内存（不读取）的指令不影响 killed 集合：
      // 因为我们不知道它写入的具体位置，无法用于判定更早的 store 是否死亡
    }

    // 删除死存储
    for (StoreInst* SI : dead) {
      SI->eraseFromParent();
      ++eliminated;
    }
  }

  return eliminated;
}

PreservedAnalyses
DeadStoreElimination::run(Module& mod, ModuleAnalysisManager& mam)
{
  int eliminated = 0;

  for (auto& func : mod) {
    // 阶段 1：消除从未被 load 的 alloca 上的所有 store
    eliminated += eliminateAllocaDeadStores(func);

    // 阶段 2：基本块内死存储消除
    eliminated += eliminateIntraBlockDeadStores(func);
  }

  mOut << "DeadStoreElimination running...\n"
       << "Eliminated " << eliminated << " dead stores\n";

  if (eliminated == 0)
    return PreservedAnalyses::all();

  // DSE 不会改变 CFG，但删除了 store 指令
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
