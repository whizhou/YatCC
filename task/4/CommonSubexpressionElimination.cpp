#include "CommonSubexpressionElimination.hpp"

#include <llvm/IR/Instructions.h>
#include <unordered_map>
#include <vector>

using namespace llvm;

/// 判断指令是否可以作为公共子表达式消除的候选
/// 排除终止指令、有副作用的指令、PHI 节点和 volatile 指令
static bool
isCSECandidate(Instruction* inst)
{
  // 终止指令不可替换
  if (inst->isTerminator())
    return false;

  // PHI 节点需要特殊处理，跳过
  if (isa<PHINode>(inst))
    return false;

  // volatile 指令不可优化
  if (inst->isVolatile())
    return false;

  // store、fence 等有副作用的指令不可替换
  if (inst->mayWriteToMemory())
    return false;

  // 只处理纯计算指令：算术、比较、类型转换、GEP、select
  switch (inst->getOpcode()) {
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::ICmp:
    case Instruction::FCmp:
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
    case Instruction::GetElementPtr:
    case Instruction::Select:
      return inst->hasNUsesOrMore(1);
    default:
      return false;
  }
}

/// 为指令计算哈希值，用于快速判断两条指令是否可能相同
/// 基于操作码和操作数指针的组合
static size_t
hashInstruction(Instruction* inst)
{
  size_t h = std::hash<unsigned>{}(inst->getOpcode());
  h ^= std::hash<unsigned>{}(inst->getNumOperands()) + 0x9e3779b9 + (h << 6) +
       (h >> 2);
  for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
    size_t opHash =
      std::hash<void*>{}(static_cast<void*>(inst->getOperand(i)));
    h ^= opHash + 0x9e3779b9 + (h << 6) + (h >> 2);
  }
  return h;
}

/// 公共子表达式消除的核心算法
/// 对每个基本块，正向遍历指令，在向前的窗口范围内搜索相同的指令
/// 若找到，则用已有的（更早的）指令替换当前指令的所有使用
static int
eliminateCommonSubexpressions(Module& mod, unsigned windowSize)
{
  int eliminated = 0;

  // 待删除的指令列表
  std::vector<Instruction*> toErase;

  for (auto& func : mod) {
    for (auto& bb : func) {
      // 哈希表：hash -> 窗口内可能相同的指令列表
      std::unordered_map<size_t, std::vector<Instruction*>> exprMap;

      // 用于维护窗口：记录指令及其哈希值，便于在滑出窗口时移除
      std::vector<std::pair<Instruction*, size_t>> window;

      // 正向遍历基本块中的指令（从前向后）
      // 窗口中的候选指令总是在当前指令之前，
      // 因此候选指令支配当前指令及其所有使用，不会违反支配关系
      for (auto& instRef : bb) {
        Instruction* inst = &instRef;

        if (!isCSECandidate(inst))
          continue;

        size_t h = hashInstruction(inst);
        bool eliminatedThis = false;

        // 在窗口中搜索相同的指令
        auto mapIt = exprMap.find(h);
        if (mapIt != exprMap.end()) {
          for (Instruction* candidate : mapIt->second) {
            // 使用 LLVM 内置方法精确判断两条指令是否相同
            // isIdenticalTo 检查操作码、类型、操作数和标志位
            if (inst->isIdenticalTo(candidate)) {
              // 找到公共子表达式，用已有的指令替换
              // candidate 在窗口中（即当前指令之前），因此 candidate
              // 支配 inst 及其所有使用
              candidate->copyMetadata(*inst);

              // 替换所有使用者
              inst->replaceAllUsesWith(candidate);

              // 标记为待删除
              toErase.push_back(inst);
              ++eliminated;
              eliminatedThis = true;
              break;
            }
          }
        }

        // 只有未被消除的指令才加入窗口作为候选
        // 已被消除的指令加入窗口会导致后续指令错误地匹配到它
        if (!eliminatedThis) {
          window.emplace_back(inst, h);
          exprMap[h].push_back(inst);

          // 维护窗口大小：移除超出窗口范围的指令
          while (window.size() > windowSize) {
            auto& [oldInst, oldHash] = window.front();
            auto& vec = exprMap[oldHash];
            // 从哈希表中移除该指令
            for (auto vecIt = vec.begin(); vecIt != vec.end(); ++vecIt) {
              if (*vecIt == oldInst) {
                vec.erase(vecIt);
                break;
              }
            }
            if (vec.empty())
              exprMap.erase(oldHash);
            window.erase(window.begin());
          }
        }
      }
    }
  }

  // 删除被消除的指令（逆序删除，避免迭代器失效）
  for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
    Instruction* inst = *it;
    if (inst->getParent() != nullptr && inst->use_empty())
      inst->eraseFromParent();
  }

  return eliminated;
}

/// 递归清理因 CSE 而变为死代码的指令
/// 类似于 DCE 的 worklist 算法
static void
cleanupDeadInstructions(Module& mod)
{
  std::vector<Instruction*> worklist;

  for (auto& func : mod) {
    for (auto& bb : func) {
      for (auto& inst : bb) {
        if (inst.use_empty() && !inst.isTerminator() && !inst.mayWriteToMemory())
          worklist.push_back(&inst);
      }
    }
  }

  while (!worklist.empty()) {
    Instruction* inst = worklist.back();
    worklist.pop_back();

    if (inst->getParent() == nullptr)
      continue;
    if (!inst->use_empty())
      continue;

    std::vector<Value*> operands;
    for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
      if (Value* op = inst->getOperand(i))
        operands.push_back(op);
    }

    inst->eraseFromParent();

    for (Value* op : operands) {
      if (auto* opInst = dyn_cast<Instruction>(op)) {
        if (opInst->use_empty() && !opInst->isTerminator() &&
            !opInst->mayWriteToMemory())
          worklist.push_back(opInst);
      }
    }
  }
}

PreservedAnalyses
CommonSubexpressionElimination::run(Module& mod, ModuleAnalysisManager& mam)
{
  int eliminated = eliminateCommonSubexpressions(mod, mWindowSize);

  if (eliminated > 0)
    cleanupDeadInstructions(mod);

  mOut << "CommonSubexpressionElimination running...\n"
       << "Window size: " << mWindowSize << "\n"
       << "Eliminated " << eliminated << " common subexpressions\n";

  if (eliminated == 0)
    return PreservedAnalyses::all();

  // CSE 只替换指令使用者，不改变 CFG
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
