#include "LoopUnroll.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <functional>

using namespace llvm;

namespace {

/// 最大完全展开阈值（迭代次数小于此值时完全展开）
static const unsigned MAX_FULL_UNROLL_THRESHOLD = 16;

/// 值映射类型（从原始值到克隆值）
using ValueMapTy = DenseMap<Value*, Value*>;

/// 获取循环的常量迭代次数
/// 如果循环没有常量迭代次数，返回 0
static uint64_t
getConstantTripCount(Loop* L, ScalarEvolution& SE)
{
  // 使用 SCEV 获取循环的退出次数
  const SCEV* BackedgeCount = SE.getBackedgeTakenCount(L);

  // 如果不是常量，返回 0
  auto* ConstBECount = dyn_cast<SCEVConstant>(BackedgeCount);
  if (!ConstBECount)
    return 0;

  // 检查是否过大
  if (ConstBECount->getAPInt().getActiveBits() > 62)
    return 0;

  uint64_t Count = ConstBECount->getAPInt().getZExtValue();
  // BackedgeTakenCount = TripCount - 1
  return Count + 1;
}

/// 克隆单条指令并更新操作数
static Instruction*
cloneInstruction(Instruction* I, ValueMapTy& VMap)
{
  Instruction* Cloned = I->clone();

  // 更新操作数引用
  for (unsigned i = 0; i < Cloned->getNumOperands(); ++i) {
    Value* Op = Cloned->getOperand(i);
    auto It = VMap.find(Op);
    if (It != VMap.end()) {
      Cloned->setOperand(i, It->second);
    }
  }

  // 更新名称（添加后缀以便调试）
  if (!I->getName().empty()) {
    Cloned->setName(I->getName() + ".unroll");
  }

  return Cloned;
}

/// 克隆基本块
static BasicBlock*
cloneBasicBlock(BasicBlock* BB, ValueMapTy& VMap, const Twine& NameSuffix,
                Function* F)
{
  // 创建新的基本块
  BasicBlock* Cloned = BasicBlock::Create(
    BB->getContext(), BB->getName() + NameSuffix, F);

  // 克隆每条指令
  for (Instruction& I : *BB) {
    Instruction* ClonedInst = cloneInstruction(&I, VMap);
    ClonedInst->insertInto(Cloned, Cloned->end());
    VMap[&I] = ClonedInst;
  }

  // 更新 PHI 节点的入边（需要在所有块克隆完成后处理）
  return Cloned;
}

/// 更新基本块中的分支目标
static void
updateBranchTargets(BasicBlock* BB, ValueMapTy& VMap)
{
  Instruction* Term = BB->getTerminator();
  if (!Term)
    return;

  for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
    BasicBlock* Succ = Term->getSuccessor(i);
    auto It = VMap.find(Succ);
    if (It != VMap.end()) {
      Term->setSuccessor(i, cast<BasicBlock>(It->second));
    }
  }
}

/// 更新 PHI 节点
static void
updatePHINodes(BasicBlock* ClonedBB, BasicBlock* OrigBB, ValueMapTy& VMap,
               BasicBlock* OldPred, BasicBlock* NewPred)
{
  for (PHINode& PHI : ClonedBB->phis()) {
    for (unsigned i = 0; i < PHI.getNumIncomingValues(); ++i) {
      if (PHI.getIncomingBlock(i) == OldPred) {
        PHI.setIncomingBlock(i, NewPred);
        break;
      }
    }
  }
}

/// 对循环进行完全展开
/// 返回 true 表示成功展开
static bool
fullyUnrollLoop(Loop* L, uint64_t TripCount, ScalarEvolution& SE,
                LoopInfo& LI, DominatorTree& DT)
{
  BasicBlock* Header = L->getHeader();
  BasicBlock* Latch = L->getLoopLatch();
  BasicBlock* Preheader = L->getLoopPreheader();
  BasicBlock* ExitBlock = L->getExitBlock();

  // 验证循环结构
  if (!Header || !Latch || !Preheader || !ExitBlock)
    return false;

  // 获取终止指令（条件分支）
  BranchInst* LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!LatchBr || !LatchBr->isConditional())
    return false;

  // 确定哪个后继是 header，哪个是 exit
  BasicBlock* LatchHeaderSucc = nullptr;
  BasicBlock* LatchExitSucc = nullptr;
  for (unsigned i = 0; i < LatchBr->getNumSuccessors(); ++i) {
    BasicBlock* Succ = LatchBr->getSuccessor(i);
    if (Succ == Header || L->contains(Succ))
      LatchHeaderSucc = Succ;
    else
      LatchExitSucc = Succ;
  }

  if (!LatchHeaderSucc || !LatchExitSucc)
    return false;

  // 获取循环体中的所有基本块（不包括子循环的块）
  SmallVector<BasicBlock*, 8> LoopBlocks;
  for (BasicBlock* BB : L->blocks()) {
    if (LI.getLoopFor(BB) == L)
      LoopBlocks.push_back(BB);
  }

  // 收集需要更新的 PHI 节点
  SmallVector<PHINode*, 8> HeaderPHIs;
  for (auto& PHI : Header->phis())
    HeaderPHIs.push_back(&PHI);

  // 存储每次展开的 header 和 latch 块
  SmallVector<BasicBlock*, 8> UnrolledHeaders;
  UnrolledHeaders.push_back(Header);

  SmallVector<BasicBlock*, 8> UnrolledLatches;
  UnrolledLatches.push_back(Latch);

  // 存储每个克隆块的值映射
  SmallVector<ValueMapTy*, 8> CloneVMaps;

  Function* F = Header->getParent();

  // ===== 创建展开后的循环体副本 =====
  for (unsigned i = 1; i < TripCount; ++i) {
    ValueMapTy* VMap = new ValueMapTy();
    CloneVMaps.push_back(VMap);

    // 克隆循环体中的所有基本块
    SmallVector<BasicBlock*, 8> ClonedBlocks;
    for (BasicBlock* BB : LoopBlocks) {
      BasicBlock* Cloned = cloneBasicBlock(BB, *VMap,
                                           ".unroll" + Twine(i), F);
      ClonedBlocks.push_back(Cloned);
      (*VMap)[BB] = Cloned;
    }

    // 更新克隆块中的分支目标
    for (BasicBlock* ClonedBB : ClonedBlocks) {
      updateBranchTargets(ClonedBB, *VMap);
    }

    // 获取克隆的 header 和 latch
    BasicBlock* ClonedHeader = cast<BasicBlock>((*VMap)[Header]);
    BasicBlock* ClonedLatch = cast<BasicBlock>((*VMap)[Latch]);

    // 更新克隆 header 的 PHI 节点：将 preheader 入边改为上一次展开的 latch
    for (PHINode& PHI : ClonedHeader->phis()) {
      for (unsigned op = 0; op < PHI.getNumIncomingValues(); ++op) {
        if (PHI.getIncomingBlock(op) == Preheader) {
          PHI.setIncomingBlock(op, UnrolledLatches.back());
          break;
        }
      }
    }

    // 为原始 header 的 PHI 节点添加来自本次展开 latch 的入边
    for (PHINode* PHI : HeaderPHIs) {
      for (unsigned op = 0; op < PHI->getNumIncomingValues(); ++op) {
        if (PHI->getIncomingBlock(op) == Latch) {
          Value* IncomingVal = PHI->getIncomingValue(op);
          auto It = VMap->find(IncomingVal);
          Value* ClonedVal = It != VMap->end() ? It->second : IncomingVal;
          PHI->addIncoming(ClonedVal, ClonedLatch);
          break;
        }
      }
    }

    UnrolledHeaders.push_back(ClonedHeader);
    UnrolledLatches.push_back(ClonedLatch);
  }

  // ===== 重新连接控制流 =====

  // 原始 latch 跳转到第二次展开的 header（而不是回到原始 header）
  if (TripCount > 1) {
    for (unsigned i = 0; i < LatchBr->getNumSuccessors(); ++i) {
      if (LatchBr->getSuccessor(i) == LatchHeaderSucc) {
        LatchBr->setSuccessor(i, UnrolledHeaders[1]);
        break;
      }
    }
  }

  // 每次展开的 latch 跳转到下一次展开的 header
  for (unsigned i = 1; i < TripCount - 1; ++i) {
    BranchInst* ClonedLatchBr =
      cast<BranchInst>(UnrolledLatches[i]->getTerminator());
    for (unsigned j = 0; j < ClonedLatchBr->getNumSuccessors(); ++j) {
      BasicBlock* Succ = ClonedLatchBr->getSuccessor(j);
      if (L->contains(Succ)) {
        ClonedLatchBr->setSuccessor(j, UnrolledHeaders[i + 1]);
        break;
      }
    }
  }

  // 最后一次展开的 latch 直接跳转到 exit block
  BranchInst* LastLatchBr =
    cast<BranchInst>(UnrolledLatches.back()->getTerminator());
  for (unsigned i = 0; i < LastLatchBr->getNumSuccessors(); ++i) {
    BasicBlock* Succ = LastLatchBr->getSuccessor(i);
    if (L->contains(Succ)) {
      LastLatchBr->setSuccessor(i, ExitBlock);
      break;
    }
  }

  // ===== 更新 exit block 的 PHI 节点 =====
  for (PHINode& PHI : ExitBlock->phis()) {
    // 找到原始 latch 入边
    int OrigLatchIdx = -1;
    for (unsigned op = 0; op < PHI.getNumIncomingValues(); ++op) {
      if (PHI.getIncomingBlock(op) == Latch) {
        OrigLatchIdx = op;
        break;
      }
    }

    if (OrigLatchIdx >= 0) {
      Value* OrigVal = PHI.getIncomingValue(OrigLatchIdx);

      // 为每次展开添加入边
      for (unsigned i = 1; i < TripCount; ++i) {
        auto* VMap = CloneVMaps[i - 1];
        auto It = VMap->find(OrigVal);
        Value* ClonedVal = It != VMap->end() ? It->second : OrigVal;
        PHI.addIncoming(ClonedVal, UnrolledLatches[i]);
      }
    }
  }

  // ===== 将克隆块添加到模块和分析结果中 =====
  for (unsigned i = 1; i < TripCount; ++i) {
    BasicBlock* ClonedHeader = UnrolledHeaders[i];
    // 将克隆块插入到函数中（在 header 之后）
    F->insert(std::next(Header->getIterator()), ClonedHeader);

    // 更新支配树
    DT.addNewBlock(ClonedHeader, DT.getNode(Header)->getIDom()->getBlock());
  }

  // 清理
  for (auto* VMap : CloneVMaps)
    delete VMap;

  return true;
}

} // anonymous namespace

PreservedAnalyses
LoopUnroll::run(Module& mod, ModuleAnalysisManager& mam)
{
  int totalUnrolled = 0;

  // 获取分析管理器
  auto& proxy = mam.getResult<FunctionAnalysisManagerModuleProxy>(mod);
  FunctionAnalysisManager& fam = proxy.getManager();

  SmallPtrSet<Function*, 4> modifiedFunctions;

  for (Function& F : mod) {
    if (F.isDeclaration())
      continue;

    auto& LI = fam.getResult<LoopAnalysis>(F);
    auto& SE = fam.getResult<ScalarEvolutionAnalysis>(F);
    auto& DT = fam.getResult<DominatorTreeAnalysis>(F);

    // 按后序遍历收集所有循环（先处理内层循环）
    SmallVector<Loop*, 8> postorderLoops;
    for (Loop* L : LI) {
      std::function<void(Loop*)> collectPostOrder = [&](Loop* lp) {
        for (Loop* sub : *lp)
          collectPostOrder(sub);
        postorderLoops.push_back(lp);
      };
      collectPostOrder(L);
    }

    // 依次尝试展开每个循环
    for (Loop* L : postorderLoops) {
      // 跳过已展开的循环（可能被后续优化删除）
      if (!LI.getLoopFor(L->getHeader()))
        continue;

      // 获取常量迭代次数
      uint64_t TripCount = getConstantTripCount(L, SE);
      if (TripCount == 0 || TripCount == 1)
        continue;

      // 只处理小循环的完全展开
      if (TripCount <= MAX_FULL_UNROLL_THRESHOLD) {
        if (fullyUnrollLoop(L, TripCount, SE, LI, DT)) {
          ++totalUnrolled;
          modifiedFunctions.insert(&F);
        }
      }
    }
  }

  mOut << "LoopUnroll running...\n"
       << "Unrolled " << totalUnrolled << " loops\n";

  if (totalUnrolled == 0)
    return PreservedAnalyses::all();

  // 失效被修改函数的分析结果
  for (Function* F : modifiedFunctions) {
    fam.invalidate(*F, PreservedAnalyses::none());
  }

  // 循环展开改变了 CFG 结构
  return PreservedAnalyses::none();
}
