#include "loopUnroll.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Passes/PassBuilder.h>

using namespace llvm;

namespace {

/// 最大完全展开的迭代次数
static const unsigned MAX_FULL_UNROLL_COUNT = 32;

/// 基本归纳变量信息
struct InductionVariable
{
  PHINode* phi;           ///< header 中的 PHI 节点
  ConstantInt* initial;   ///< 初始值（来自 preheader）
  ConstantInt* step;      ///< 步长绝对值
  int64_t direction;      ///< +1 表示递增 (Add), -1 表示递减 (Sub)
  Instruction* stepInst;  ///< 定义步长的指令
};

/// 尝试识别循环中的基本归纳变量
/// 归纳变量在 loop header 中形如：
///   %iv = phi [init, %preheader], [stepVal, %latch]
/// 其中 init 是 ConstantInt，stepVal 是形如 add/sub %iv, step 的指令
static bool
identifyInductionVariable(Loop* L, InductionVariable& iv)
{
  BasicBlock* header = L->getHeader();
  BasicBlock* preheader = L->getLoopPreheader();
  BasicBlock* latch = L->getLoopLatch();

  if (!header || !preheader || !latch)
    return false;

  for (PHINode& phi : header->phis()) {
    Value* initVal = phi.getIncomingValueForBlock(preheader);
    Value* stepVal = phi.getIncomingValueForBlock(latch);

    auto* init = dyn_cast<ConstantInt>(initVal);
    if (!init)
      continue;

    auto* stepInst = dyn_cast<Instruction>(stepVal);
    if (!stepInst)
      continue;

    auto* binOp = dyn_cast<BinaryOperator>(stepInst);
    if (!binOp)
      continue;

    unsigned opcode = binOp->getOpcode();
    int64_t direction;
    if (opcode == Instruction::Add)
      direction = 1;
    else if (opcode == Instruction::Sub)
      direction = -1;
    else
      continue;

    // 一步操作数必须是 phi 自身，另一个是常量
    ConstantInt* stepConst = nullptr;
    if (binOp->getOperand(0) == &phi)
      stepConst = dyn_cast<ConstantInt>(binOp->getOperand(1));
    else if (binOp->getOperand(1) == &phi)
      stepConst = dyn_cast<ConstantInt>(binOp->getOperand(0));

    if (!stepConst)
      continue;

    iv.phi = &phi;
    iv.initial = init;
    iv.step = stepConst;
    iv.direction = direction;
    iv.stepInst = stepInst;
    return true;
  }

  return false;
}

/// 计算循环的迭代次数（trip count）
/// 分析退出基本块中的条件分支和 icmp 指令来推导
/// outExitBlock 输出退出块（不在循环内的后继）
/// 返回 -1 表示无法确定
static int64_t
computeTripCount(Loop* L, InductionVariable& iv, BasicBlock*& outExitBlock)
{
  BasicBlock* exiting = L->getExitingBlock();
  if (!exiting)
    return -1;

  auto* br = dyn_cast<BranchInst>(exiting->getTerminator());
  if (!br || !br->isConditional())
    return -1;

  auto* icmp = dyn_cast<ICmpInst>(br->getCondition());
  if (!icmp)
    return -1;

  Value* lhs = icmp->getOperand(0);
  Value* rhs = icmp->getOperand(1);

  // 确定哪一侧是 IV，哪一侧是常量界限
  ConstantInt* bound = nullptr;
  if (auto* c = dyn_cast<ConstantInt>(lhs)) {
    bound = c;
    if (rhs != iv.phi)
      return -1;
  } else if (auto* c = dyn_cast<ConstantInt>(rhs)) {
    bound = c;
    if (lhs != iv.phi)
      return -1;
  } else {
    return -1;
  }

  // 确定条件为真时循环继续还是退出
  BasicBlock* trueDest = br->getSuccessor(0);
  BasicBlock* falseDest = br->getSuccessor(1);

  bool continueWhenTrue = L->contains(trueDest);
  outExitBlock = continueWhenTrue ? falseDest : trueDest;

  // 若循环在条件为假时继续，则取反谓词
  // 现在 pred 是循环**继续**执行的条件
  auto pred = continueWhenTrue ? icmp->getPredicate()
                               : icmp->getInversePredicate();

  int64_t init = iv.initial->getSExtValue();
  int64_t limit = bound->getSExtValue();
  int64_t effStep = iv.step->getSExtValue() * iv.direction;

  // iv 在第 k 次迭代时为 init + k*effStep (k = 0, 1, ...)
  // 循环在条件首次为假时退出
  // tripCount = 满足条件的最大的 k + 1

  if (effStep > 0) {
    switch (pred) {
      case CmpInst::ICMP_SLT:
      case CmpInst::ICMP_ULT:
        // init + k*effStep < limit → k < (limit-init)/effStep
        // tripCount = ceil((limit - init) / effStep)
        return std::max(int64_t(0),
                        (limit - init + effStep - 1) / effStep);
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_ULE:
        // init + k*effStep <= limit → k <= (limit-init)/effStep
        // tripCount = floor((limit - init) / effStep) + 1
        if (limit < init)
          return 0;
        return (limit - init) / effStep + 1;
      default:
        return -1;
    }
  } else if (effStep < 0) {
    int64_t absStep = -effStep;
    switch (pred) {
      case CmpInst::ICMP_SGT:
      case CmpInst::ICMP_UGT:
        // init + k*effStep > limit (effStep 为负)
        // tripCount = ceil((init - limit) / |effStep|)
        return std::max(int64_t(0),
                        (init - limit + absStep - 1) / absStep);
      case CmpInst::ICMP_SGE:
      case CmpInst::ICMP_UGE:
        if (init < limit)
          return 0;
        return (init - limit) / absStep + 1;
      default:
        return -1;
    }
  }

  return -1;
}

/// 收集循环中直接包含的基本块（排除子循环的基本块）
static SmallVector<BasicBlock*, 16>
getDirectLoopBlocks(Loop* L, LoopInfo& LI)
{
  SmallVector<BasicBlock*, 16> blocks;
  for (BasicBlock* BB : L->blocks()) {
    if (LI.getLoopFor(BB) == L)
      blocks.push_back(BB);
  }
  return blocks;
}

/// 克隆一个基本块的所有指令到新块中，并填充 VMap
static BasicBlock*
cloneBlockManual(BasicBlock* BB, ValueToValueMapTy& VMap, const Twine& suffix,
                 Function* F)
{
  BasicBlock* newBB =
    BasicBlock::Create(BB->getContext(), BB->getName() + suffix, F);

  for (Instruction& I : *BB) {
    Instruction* newI = I.clone();
    if (I.hasName())
      newI->setName(I.getName() + suffix);
    newBB->getInstList().push_back(newI);
    VMap[&I] = newI;
  }

  return newBB;
}

/// 为 VMap 构建反向映射（克隆值 → 原始值），便于从克隆指令反查原始指令
static DenseMap<Value*, Value*>
buildReverseMap(const ValueToValueMapTy& vmap)
{
  DenseMap<Value*, Value*> rev;
  for (auto& [orig, clone] : vmap)
    rev[clone] = const_cast<Value*>(orig);
  return rev;
}

/// 对克隆的基本块中的指令操作数进行重映射
/// 将指向原始循环内指令的操作数替换为 VMap 中对应的克隆值
static void
remapInstructions(BasicBlock* BB, ValueToValueMapTy& VMap)
{
  for (Instruction& I : *BB) {
    if (auto* phi = dyn_cast<PHINode>(&I)) {
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        Value* incomingVal = phi->getIncomingValue(i);
        auto it = VMap.find(incomingVal);
        if (it != VMap.end())
          phi->setIncomingValue(i, it->second);
        // 基本块的重映射在 fixupSuccessors 中处理
      }
    } else {
      for (unsigned i = 0; i < I.getNumOperands(); ++i) {
        Value* op = I.getOperand(i);
        auto it = VMap.find(op);
        if (it != VMap.end())
          I.setOperand(i, it->second);
      }
    }
  }
}

/// 修复所有迭代副本中基本块的后继关系：
/// - 循环内部边 → 同一迭代副本的对应块
/// - 回边（目标为 header）→ 下一迭代副本的 header（末次迭代指向退出块）
/// - 退出边 → 保持指向原始退出块
/// 同时修复非 header 块中 PHI 节点的基本块引用
static void
fixupSuccessors(
  const SmallVector<BasicBlock*, 16>& loopBlocks,
  BasicBlock* header, BasicBlock* exitBlock,
  DenseMap<BasicBlock*, SmallVector<BasicBlock*, 8>>& clonedBlocks,
  int64_t tripCount)
{
  SmallPtrSet<BasicBlock*, 16> loopBlockSet(loopBlocks.begin(),
                                            loopBlocks.end());

  for (int64_t iter = 0; iter < tripCount; ++iter) {
    for (BasicBlock* BB : loopBlocks) {
      BasicBlock* clone = clonedBlocks[BB][iter];
      Instruction* term = clone->getTerminator();

      for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
        BasicBlock* succ = term->getSuccessor(i);

        if (!loopBlockSet.count(succ))
          continue; // 退出边，保持不变

        if (succ == header) {
          // 回边：末次迭代指向退出块，否则指向下一迭代的 header
          term->setSuccessor(i, (iter < tripCount - 1)
                                  ? clonedBlocks[header][iter + 1]
                                  : exitBlock);
        } else {
          // 循环内部边：指向同一迭代的对应块
          term->setSuccessor(i, clonedBlocks[succ][iter]);
        }
      }
    }
  }

  // 修复非 header 块中 PHI 节点的基本块引用
  for (int64_t iter = 0; iter < tripCount; ++iter) {
    for (BasicBlock* BB : loopBlocks) {
      if (BB == header)
        continue;
      BasicBlock* clone = clonedBlocks[BB][iter];
      for (PHINode& phi : clone->phis()) {
        for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
          BasicBlock* incBB = phi.getIncomingBlock(i);
          if (loopBlockSet.count(incBB))
            phi.setIncomingBlock(i, clonedBlocks[incBB][iter]);
        }
      }
    }
  }
}

/// 修复所有退出块中的 PHI 节点
/// 循环展开后，退出块可能被多个克隆块引用，其 PHI 节点需要更新
/// 将来自原始循环块的 PHI 条目替换为来自对应克隆块的条目
/// @param iterVMaps 每次迭代的 VMap，用于查找原始值在克隆中的对应
static void
fixExitBlockPhis(
  Loop* L,
  const SmallVector<BasicBlock*, 16>& loopBlocks,
  DenseMap<BasicBlock*, SmallVector<BasicBlock*, 8>>& clonedBlocks,
  DenseMap<int64_t, ValueToValueMapTy>& iterVMaps,
  int64_t tripCount)
{
  SmallPtrSet<BasicBlock*, 16> loopBlockSet(loopBlocks.begin(),
                                            loopBlocks.end());

  // 收集所有退出块
  SmallVector<BasicBlock*, 8> exitBlocks;
  L->getExitBlocks(exitBlocks);

  for (BasicBlock* exitBlock : exitBlocks) {
    for (PHINode& phi : exitBlock->phis()) {
      // 收集来自循环块的 PHI 条目
      struct OldEntry
      {
        unsigned idx;
        Value* val;
        BasicBlock* bb;
      };
      SmallVector<OldEntry, 8> oldEntries;

      for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
        BasicBlock* incBB = phi.getIncomingBlock(i);
        if (loopBlockSet.count(incBB))
          oldEntries.push_back({ i, phi.getIncomingValue(i), incBB });
      }

      if (oldEntries.empty())
        continue;

      // 对每个旧条目：为每个有边指向此退出块的克隆块添加 PHI 条目
      for (auto& entry : oldEntries) {
        for (int64_t iter = 0; iter < tripCount; ++iter) {
          BasicBlock* clonedBB = clonedBlocks[entry.bb][iter];

          // 检查此克隆块是否确实有边指向此退出块
          Instruction* term = clonedBB->getTerminator();
          bool reachesExit = false;
          for (unsigned s = 0; s < term->getNumSuccessors(); ++s) {
            if (term->getSuccessor(s) == exitBlock) {
              reachesExit = true;
              break;
            }
          }
          if (!reachesExit)
            continue;

          // 使用 VMap 查找克隆值（而非名称匹配）
          Value* clonedVal = nullptr;
          auto& vmap = iterVMaps[iter];
          auto it = vmap.find(entry.val);
          if (it != vmap.end())
            clonedVal = it->second;
          else
            clonedVal = entry.val; // 常量或循环外值，直接使用原值

          phi.addIncoming(clonedVal, clonedBB);
        }
      }

      // 逆序删除旧条目以避免索引失效
      SmallVector<unsigned, 8> toRemove;
      for (auto& entry : oldEntries)
        toRemove.push_back(entry.idx);
      std::sort(toRemove.begin(), toRemove.end(), std::greater<unsigned>());
      for (unsigned idx : toRemove)
        phi.removeIncomingValue(idx, false);
    }
  }
}

/// 替换克隆的 header 中所有 PHI 节点
/// 迭代 0：使用来自 preheader 的初始值
/// 迭代 j > 0：使用上一迭代副本中 latch 侧产生的值（通过反向 VMap 查找）
static void
replaceHeaderPhis(
  BasicBlock* header, BasicBlock* preheader, BasicBlock* latch,
  DenseMap<BasicBlock*, SmallVector<BasicBlock*, 8>>& clonedBlocks,
  DenseMap<int64_t, ValueToValueMapTy>& iterVMaps,
  DenseMap<int64_t, DenseMap<Value*, Value*>>& iterRevMaps,
  InductionVariable& iv, int64_t tripCount)
{
  for (int64_t iter = 0; iter < tripCount; ++iter) {
    BasicBlock* headerClone = clonedBlocks[header][iter];

    // 收集此 header 副本中的 PHI 节点
    SmallVector<PHINode*, 8> phis;
    for (PHINode& phi : headerClone->phis())
      phis.push_back(&phi);

    for (PHINode* phi : phis) {
      Value* replacement = nullptr;

      if (iter == 0) {
        // 第 0 次迭代：控制从 preheader 进入，PHI 取 preheader 侧的值
        // 克隆的 PHI 仍保留原始 incoming blocks，preheader 侧值可直接获取
        replacement = phi->getIncomingValueForBlock(preheader);
        // replacement 可能是 ConstantInt（IV 初始值）或循环外定义的指令
      } else {
        // 第 j > 0 次迭代：控制从上一迭代的 latch 进入
        // 通过反向映射找到此克隆 PHI 对应的原始 PHI
        auto& revMap = iterRevMaps[iter];
        auto* origPhi =
          dyn_cast_or_null<PHINode>(revMap.lookup(phi));
        if (origPhi) {
          Value* origLatchVal =
            origPhi->getIncomingValueForBlock(latch);
          // 在上一迭代的 VMap 中查找 latch 侧值的克隆
          auto& prevVMap = iterVMaps[iter - 1];
          auto it = prevVMap.find(origLatchVal);
          if (it != prevVMap.end())
            replacement = it->second;
          else
            replacement = origLatchVal; // 常量或循环外值，直接使用
        }
      }

      // 若无法确定替换值，尝试用 IV 公式直接计算（适用于归纳变量）
      if (!replacement && phi->getType()->isIntegerTy()) {
        int64_t init = iv.initial->getSExtValue();
        int64_t step = iv.step->getSExtValue() * iv.direction;
        int64_t val = init + iter * step;
        replacement = ConstantInt::get(phi->getType(), val);
      }

      if (replacement) {
        phi->replaceAllUsesWith(replacement);
        phi->eraseFromParent();
      }
    }
  }
}

/// 安全地删除循环原始基本块
/// 在删除前断开与其他块（如退出块 PHI 节点）的引用
static void
eraseOriginalLoopBlocks(const SmallVector<BasicBlock*, 16>& loopBlocks)
{
  // 将原始循环块中指令的所有使用者替换为 undef
  // 此时退出块的 PHI 应已由 fixExitBlockPhis 处理完毕
  for (BasicBlock* BB : loopBlocks) {
    for (auto it = BB->begin(); it != BB->end();) {
      Instruction* I = &*it++;
      if (!I->use_empty())
        I->replaceAllUsesWith(UndefValue::get(I->getType()));
    }
  }

  for (BasicBlock* BB : loopBlocks) {
    BB->eraseFromParent();
  }
}

/// 对单个循环执行完全展开
/// 返回展开的迭代次数（0 表示未展开）
static int
tryFullyUnroll(Loop* L, LoopInfo& LI, DominatorTree& DT)
{
  BasicBlock* preheader = L->getLoopPreheader();
  BasicBlock* header = L->getHeader();
  BasicBlock* latch = L->getLoopLatch();

  if (!preheader || !header || !latch)
    return 0;

  // 1. 识别归纳变量
  InductionVariable iv;
  if (!identifyInductionVariable(L, iv))
    return 0;

  // 2. 计算迭代次数
  BasicBlock* exitBlock = nullptr;
  int64_t tripCount = computeTripCount(L, iv, exitBlock);
  if (tripCount <= 0 ||
      tripCount > static_cast<int64_t>(MAX_FULL_UNROLL_COUNT))
    return 0;
  if (!exitBlock)
    return 0;

  Function* F = header->getParent();

  // 3. 收集循环体中的基本块
  auto loopBlocks = getDirectLoopBlocks(L, LI);

  // 4. 克隆循环体 tripCount 份
  // clonedBlocks[原始块] = {克隆_0, 克隆_1, ..., 克隆_{tripCount-1}}
  DenseMap<BasicBlock*, SmallVector<BasicBlock*, 8>> clonedBlocks;
  DenseMap<int64_t, ValueToValueMapTy> iterVMaps;
  DenseMap<int64_t, DenseMap<Value*, Value*>> iterRevMaps;

  for (int64_t iter = 0; iter < tripCount; ++iter) {
    ValueToValueMapTy vmap;

    for (BasicBlock* BB : loopBlocks) {
      BasicBlock* clone =
        cloneBlockManual(BB, vmap, ".unroll" + Twine(iter), F);
      clonedBlocks[BB].push_back(clone);
    }

    // 重映射所有操作数
    for (BasicBlock* BB : loopBlocks) {
      BasicBlock* clone = clonedBlocks[BB][iter];
      remapInstructions(clone, vmap);
    }

    iterVMaps[iter] = std::move(vmap);
    iterRevMaps[iter] = buildReverseMap(iterVMaps[iter]);
  }

  // 5. 修复后继关系
  fixupSuccessors(loopBlocks, header, exitBlock, clonedBlocks, tripCount);

  // 6. 修复退出块的 PHI 节点（必须在删除原始块之前完成）
  fixExitBlockPhis(L, loopBlocks, clonedBlocks, iterVMaps, tripCount);

  // 7. 替换 header 中的 PHI 节点
  replaceHeaderPhis(header, preheader, latch, clonedBlocks, iterVMaps,
                    iterRevMaps, iv, tripCount);

  // 8. 修复 preheader：指向第 0 个迭代副本的 header
  Instruction* preheaderTerm = preheader->getTerminator();
  for (unsigned i = 0; i < preheaderTerm->getNumSuccessors(); ++i) {
    if (preheaderTerm->getSuccessor(i) == header) {
      preheaderTerm->setSuccessor(i, clonedBlocks[header][0]);
      break;
    }
  }

  // 9. 删除原始循环基本块
  eraseOriginalLoopBlocks(loopBlocks);

  return static_cast<int>(tripCount);
}

/// 对单个循环及其子循环递归执行展开（后序遍历，内层优先）
static int
unrollLoopRecursive(Loop* L, LoopInfo& LI, DominatorTree& DT)
{
  int totalUnrolled = 0;

  // 先处理子循环
  SmallVector<Loop*, 8> subLoops;
  for (Loop* sub : *L)
    subLoops.push_back(sub);
  for (Loop* sub : subLoops)
    totalUnrolled += unrollLoopRecursive(sub, LI, DT);

  // 再处理当前循环
  totalUnrolled += tryFullyUnroll(L, LI, DT);

  return totalUnrolled;
}

} // anonymous namespace

PreservedAnalyses
LoopUnroll::run(Module& mod, ModuleAnalysisManager& mam)
{
  int totalUnrolled = 0;

  FunctionAnalysisManager fam;
  PassBuilder pb;
  pb.registerFunctionAnalyses(fam);

  for (Function& F : mod) {
    if (F.isDeclaration())
      continue;

    auto& LI = fam.getResult<LoopAnalysis>(F);
    auto& DT = fam.getResult<DominatorTreeAnalysis>(F);

    SmallVector<Loop*, 8> topLevelLoops;
    for (Loop* L : LI)
      topLevelLoops.push_back(L);

    for (Loop* L : topLevelLoops)
      totalUnrolled += unrollLoopRecursive(L, LI, DT);
  }

  mOut << "LoopUnroll running...\n"
       << "Fully unrolled " << totalUnrolled << " loop iterations\n";

  if (totalUnrolled == 0)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
