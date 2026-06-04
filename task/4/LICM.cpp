#include "LICM.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Passes/PassBuilder.h>

using namespace llvm;

namespace {

/// 检查 load 指令所加载的内存位置是否在循环内被修改
/// 遍历循环中的所有 store 指令，检查是否有对同一底层对象的写入。
/// 使用 getUnderlyingObject 比较底层对象，避免不同 GEP 指令计算相同地址却比较失败的问题。
/// 同时保守地认为任何可能写内存的 call 指令都会修改该位置。
static bool
isLoadInvariantInLoop(LoadInst* LI, Loop* L)
{
  Value* loadObj = getUnderlyingObject(LI->getPointerOperand());

  for (BasicBlock* BB : L->blocks()) {
    for (Instruction& I : *BB) {
      // 检查同底层对象的 store
      if (auto* SI = dyn_cast<StoreInst>(&I)) {
        Value* storeObj = getUnderlyingObject(SI->getPointerOperand());
        if (storeObj == loadObj)
          return false;
      }
      // 保守处理：任何可能写内存的 call 都可能修改该位置
      if (auto* CI = dyn_cast<CallInst>(&I)) {
        if (CI->mayWriteToMemory()) {
          Function* callee = CI->getCalledFunction();
          // 如果 call 是间接调用或有副作用，认为不安全
          if (!callee || !callee->onlyReadsMemory())
            return false;
        }
      }
    }
  }
  return true;
}

/// 检查指令是否可投机执行（不会触发陷阱或产生副作用）
/// 优先使用 LLVM 内置的 isSafeToSpeculativelyExecute，
/// 并对 load 指令做额外宽松处理（只要内存位置在循环内不变即可）。
static bool
isSafeToSpeculativelyExecute(Instruction* I, Loop* L)
{
  // LLVM 内置检查：排除除法、间接调用等可能陷阱的指令
  if (llvm::isSafeToSpeculativelyExecute(I))
    return true;

  // Load 指令：LLVM 内置检查对 load 较保守（要求 dereferenceable 指针）。
  // 在 LICM 的上下文中，如果 load 的内存位置在循环内未被修改，
  // 即使指针不是严格 dereferenceable，也可安全提升。
  if (auto* LI = dyn_cast<LoadInst>(I)) {
    return isLoadInvariantInLoop(LI, L);
  }

  return false;
}

/// 检查指令所在基本块是否支配循环的所有出口基本块
/// 若指令支配所有出口，则它在循环每次迭代时都会执行，
/// 提升到 preheader 不会改变程序的语义（不是投机执行）。
static bool
dominatesAllExitBlocks(Instruction* I, Loop* L, DominatorTree& DT)
{
  SmallVector<BasicBlock*, 8> exitBlocks;
  L->getExitBlocks(exitBlocks);

  return all_of(exitBlocks, [&](BasicBlock* EB) {
    return DT.dominates(I->getParent(), EB);
  });
}

/// 检查提升指令到 preheader 是否安全
/// 安全条件（满足其一即可）：
/// 1. 指令可投机执行（不会陷阱/无副作用）
/// 2. 指令所在基本块支配所有循环出口（保证每次迭代都会执行）
static bool
safeToHoist(Instruction* I, Loop* L, DominatorTree& DT)
{
  if (isSafeToSpeculativelyExecute(I, L))
    return true;

  return dominatesAllExitBlocks(I, L, DT);
}

/// 检查指令是否为循环无关变量
/// 循环无关指令满足：
/// 1. 类型适合提升（排除 phi、alloca、终止指令等）
/// 2. 对于 load，内存位置在循环内未被修改
/// 3. 对于 call，被调用函数无副作用（只读或不访问内存）
/// 4. 所有操作数都是常量、在循环外定义、或已被提升到 preheader
static bool
isLoopInvariant(Instruction* I, Loop* L, LoopInfo& LI,
                SmallPtrSetImpl<Instruction*>& hoisted)
{
  // PHI 节点的值来自不同迭代的前驱块，不是循环无关的
  if (isa<PHINode>(I))
    return false;

  // Alloca 指令分配新内存，每次执行效果不同
  if (isa<AllocaInst>(I))
    return false;

  // 终止指令不能移动
  if (I->isTerminator())
    return false;

  // Store 指令有副作用，不能作为循环无关变量提升
  if (isa<StoreInst>(I))
    return false;

  // Load 指令：额外检查内存位置是否在循环内被修改
  if (auto* LI = dyn_cast<LoadInst>(I)) {
    if (!isLoadInvariantInLoop(LI, L))
      return false;
  }

  // Call 指令：检查被调用函数是否无副作用
  if (auto* CI = dyn_cast<CallInst>(I)) {
    Function* callee = CI->getCalledFunction();
    // 间接调用保守地认为不循环无关
    if (!callee)
      return false;
    // 只有纯函数（不写内存）的调用结果才是循环无关的
    if (!callee->doesNotAccessMemory() && !callee->onlyReadsMemory())
      return false;
  }

  // 检查所有操作数
  for (Use& U : I->operands()) {
    Value* op = U.get();

    // 常量在循环外定义，始终无关
    if (isa<Constant>(op))
      continue;

    // 指令类型的操作数：检查是否在循环外定义或已被提升
    if (auto* opI = dyn_cast<Instruction>(op)) {
      // 在循环外定义：无关
      if (!L->contains(opI->getParent()))
        continue;
      // 已被提升到 preheader：无关
      if (hoisted.count(opI))
        continue;
    } else {
      // 非指令值（函数参数、全局变量等）：在循环外定义，无关
      continue;
    }

    // 还有操作数在循环内且未被提升，则该指令不是循环无关的
    return false;
  }

  return true;
}

/// 从单个循环中提升循环无关指令到 preheader
/// 重复迭代直到收敛，因为提升一条指令可能使依赖它的指令也变为循环无关
static int
hoistFromLoop(Loop* L, LoopInfo& LI, DominatorTree& DT,
              SmallPtrSetImpl<Instruction*>& hoistedSet)
{
  BasicBlock* preheader = L->getLoopPreheader();
  if (!preheader)
    return 0;

  int count = 0;
  bool changed = true;

  // 迭代直到没有新的指令可以提升
  while (changed) {
    changed = false;

    // 收集当前循环直接包含的基本块（排除子循环的基本块）
    SmallVector<BasicBlock*, 32> loopBlocks;
    for (BasicBlock* BB : L->blocks()) {
      if (LI.getLoopFor(BB) == L)
        loopBlocks.push_back(BB);
    }

    for (BasicBlock* BB : loopBlocks) {
      // 遍历指令，使用迭代器安全地在移动指令时继续遍历
      for (auto it = BB->begin(), end = BB->end(); it != end;) {
        Instruction* I = &*it++;

        // 跳过已提升的指令
        if (hoistedSet.count(I))
          continue;

        // 跳过终止指令和 PHI 节点
        if (I->isTerminator() || isa<PHINode>(I))
          continue;

        // 检查是否为循环无关变量
        if (!isLoopInvariant(I, L, LI, hoistedSet))
          continue;

        // 检查是否可以安全提升
        if (!safeToHoist(I, L, DT))
          continue;

        // 提升指令到 preheader（插入到 terminator 之前）
        I->moveBefore(preheader->getTerminator());
        hoistedSet.insert(I);
        ++count;
        changed = true;
      }
    }
  }

  return count;
}

} // anonymous namespace

PreservedAnalyses
LICM::run(Module& mod, ModuleAnalysisManager& mam)
{
  int totalHoisted = 0;

  // 使用管线的 FunctionAnalysisManager，避免创建独立的 FAM 导致后续 pass 使用过时分析结果
  auto& proxy = mam.getResult<FunctionAnalysisManagerModuleProxy>(mod);
  FunctionAnalysisManager& fam = proxy.getManager();

  SmallPtrSet<Function*, 4> modifiedFunctions;

  for (Function& F : mod) {
    if (F.isDeclaration())
      continue;

    auto& LI = fam.getResult<LoopAnalysis>(F);
    auto& DT = fam.getResult<DominatorTreeAnalysis>(F);

    // 按后序遍历收集所有循环（先处理内层循环）
    SmallVector<Loop*, 8> postorderLoops;
    for (Loop* L : LI) {
      // DFS 后序遍历
      std::function<void(Loop*)> collectPostOrder = [&](Loop* lp) {
        for (Loop* sub : *lp)
          collectPostOrder(sub);
        postorderLoops.push_back(lp);
      };
      collectPostOrder(L);
    }

    // 依次处理每个循环（内层优先）
    // hoistedSet 在函数内所有循环间共享，确保外层循环能看到内层循环已提升的指令
    SmallPtrSet<Instruction*, 16> hoistedSet;
    int before = totalHoisted;
    for (Loop* L : postorderLoops) {
      totalHoisted += hoistFromLoop(L, LI, DT, hoistedSet);
    }
    if (totalHoisted > before)
      modifiedFunctions.insert(&F);
  }

  mOut << "LICM running...\n"
       << "Hoisted " << totalHoisted << " loop-invariant instructions\n";

  if (totalHoisted == 0)
    return PreservedAnalyses::all();

  // 失效被修改函数的分析结果，确保后续 pass 使用正确的 LoopInfo 和 DominatorTree
  for (Function* F : modifiedFunctions) {
    fam.invalidate(*F, PreservedAnalyses::none());
  }

  // LICM 只移动指令，不改变 CFG 结构
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
