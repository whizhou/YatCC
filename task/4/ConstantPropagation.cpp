#include "ConstantPropagation.hpp"

using namespace llvm;

/// 检查全局变量是否被 store 指令修改
static bool
hasStoreToGlobal(GlobalVariable* GV)
{
  for (User* U : GV->users()) {
    if (isa<StoreInst>(U))
      return true;
    if (auto* GEP = dyn_cast<GetElementPtrInst>(U)) {
      for (User* GEPU : GEP->users()) {
        if (isa<StoreInst>(GEPU))
          return true;
      }
    }
  }
  return false;
}

/// 处理简单全局变量（非数组）的常量传播
static int
propagateSimpleGlobal(GlobalVariable* GV)
{
  if (!GV->hasInitializer())
    return 0;

  auto* CI = dyn_cast<ConstantInt>(GV->getInitializer());
  if (!CI)
    return 0;

  std::vector<LoadInst*> loads;
  for (User* U : GV->users()) {
    if (auto* LI = dyn_cast<LoadInst>(U))
      loads.push_back(LI);
  }

  if (loads.empty())
    return 0;

  int replaced = 0;
  for (LoadInst* LI : loads) {
    LI->replaceAllUsesWith(CI);
    LI->eraseFromParent();
    ++replaced;
  }

  return replaced;
}

/// 处理全局常量数组的常量传播
static int
propagateConstantArray(GlobalVariable* GV)
{
  if (!GV->hasInitializer())
    return 0;

  auto* CA = dyn_cast<ConstantArray>(GV->getInitializer());
  if (!CA)
    return 0;

  int replaced = 0;

  std::vector<std::pair<GetElementPtrInst*, std::vector<LoadInst*>>> gepLoads;
  for (User* U : GV->users()) {
    if (auto* GEP = dyn_cast<GetElementPtrInst>(U)) {
      std::vector<LoadInst*> loads;
      for (User* GEPU : GEP->users()) {
        if (auto* LI = dyn_cast<LoadInst>(GEPU))
          loads.push_back(LI);
      }
      if (!loads.empty())
        gepLoads.push_back({GEP, loads});
    }
  }

  for (auto& [GEP, loads] : gepLoads) {
    if (GEP->getNumIndices() != 2)
      continue;

    auto idxIt = GEP->idx_begin();
    ++idxIt;
    auto* idxCI = dyn_cast<ConstantInt>(*idxIt);
    if (!idxCI)
      continue;

    uint64_t elemIdx = idxCI->getZExtValue();
    if (elemIdx >= CA->getNumOperands())
      continue;

    auto* elemVal = dyn_cast<ConstantInt>(CA->getOperand(elemIdx));
    if (!elemVal)
      continue;

    for (LoadInst* LI : loads) {
      LI->replaceAllUsesWith(elemVal);
      LI->eraseFromParent();
      ++replaced;
    }

    if (GEP->use_empty())
      GEP->eraseFromParent();
  }

  return replaced;
}

/// 常量传播：将全局常量变量的 load 替换为常量值
static int
constantPropagation(Module& mod)
{
  int totalReplaced = 0;

  std::vector<GlobalVariable*> globals;
  for (GlobalVariable& GV : mod.globals()) {
    if (GV.hasInitializer() && !GV.isConstant())
      globals.push_back(&GV);
  }

  for (GlobalVariable* GV : globals) {
    if (GV->use_empty())
      continue;

    if (hasStoreToGlobal(GV))
      continue;

    totalReplaced += propagateSimpleGlobal(GV);
    totalReplaced += propagateConstantArray(GV);

    if (GV->use_empty() && !GV->hasExternalLinkage())
      GV->eraseFromParent();
  }

  return totalReplaced;
}

/// 常量折叠：将两个操作数均为常量的二元运算替换为常量结果
static int
constantFolding(Module& mod)
{
  int foldedTimes = 0;

  for (auto& func : mod) {
    for (auto& bb : func) {
      std::vector<Instruction*> instToErase;

      for (auto& inst : bb) {
        if (auto* binOp = dyn_cast<BinaryOperator>(&inst)) {
          Value* lhs = binOp->getOperand(0);
          Value* rhs = binOp->getOperand(1);
          auto* constLhs = dyn_cast<ConstantInt>(lhs);
          auto* constRhs = dyn_cast<ConstantInt>(rhs);

          if (!constLhs || !constRhs)
            continue;

          int64_t result;
          switch (binOp->getOpcode()) {
            case Instruction::Add:
              result = constLhs->getSExtValue() + constRhs->getSExtValue();
              break;
            case Instruction::Sub:
              result = constLhs->getSExtValue() - constRhs->getSExtValue();
              break;
            case Instruction::Mul:
              result = constLhs->getSExtValue() * constRhs->getSExtValue();
              break;
            case Instruction::SDiv:
              if (constRhs->getSExtValue() == 0)
                continue;
              result = constLhs->getSExtValue() / constRhs->getSExtValue();
              break;
            case Instruction::UDiv:
              if (constRhs->getZExtValue() == 0)
                continue;
              result = constLhs->getZExtValue() / constRhs->getZExtValue();
              break;
            case Instruction::SRem:
              if (constRhs->getSExtValue() == 0)
                continue;
              result = constLhs->getSExtValue() % constRhs->getSExtValue();
              break;
            case Instruction::URem:
              if (constRhs->getZExtValue() == 0)
                continue;
              result = constLhs->getZExtValue() % constRhs->getZExtValue();
              break;
            case Instruction::And:
              result = constLhs->getSExtValue() & constRhs->getSExtValue();
              break;
            case Instruction::Or:
              result = constLhs->getSExtValue() | constRhs->getSExtValue();
              break;
            case Instruction::Xor:
              result = constLhs->getSExtValue() ^ constRhs->getSExtValue();
              break;
            case Instruction::Shl:
              result = constLhs->getSExtValue() << constRhs->getSExtValue();
              break;
            case Instruction::LShr:
              result =
                (int64_t)(constLhs->getZExtValue() >> constRhs->getZExtValue());
              break;
            case Instruction::AShr:
              result = constLhs->getSExtValue() >> constRhs->getSExtValue();
              break;
            default:
              continue;
          }

          binOp->replaceAllUsesWith(
            ConstantInt::getSigned(binOp->getType(), result));
          instToErase.push_back(binOp);
          ++foldedTimes;
        }
        // 处理比较指令
        else if (auto* cmp = dyn_cast<ICmpInst>(&inst)) {
          Value* lhs = cmp->getOperand(0);
          Value* rhs = cmp->getOperand(1);
          auto* constLhs = dyn_cast<ConstantInt>(lhs);
          auto* constRhs = dyn_cast<ConstantInt>(rhs);

          if (!constLhs || !constRhs)
            continue;

          bool result;
          switch (cmp->getPredicate()) {
            case ICmpInst::ICMP_EQ:
              result = constLhs->getSExtValue() == constRhs->getSExtValue();
              break;
            case ICmpInst::ICMP_NE:
              result = constLhs->getSExtValue() != constRhs->getSExtValue();
              break;
            case ICmpInst::ICMP_SLT:
              result = constLhs->getSExtValue() < constRhs->getSExtValue();
              break;
            case ICmpInst::ICMP_SLE:
              result = constLhs->getSExtValue() <= constRhs->getSExtValue();
              break;
            case ICmpInst::ICMP_SGT:
              result = constLhs->getSExtValue() > constRhs->getSExtValue();
              break;
            case ICmpInst::ICMP_SGE:
              result = constLhs->getSExtValue() >= constRhs->getSExtValue();
              break;
            case ICmpInst::ICMP_ULT:
              result = constLhs->getZExtValue() < constRhs->getZExtValue();
              break;
            case ICmpInst::ICMP_ULE:
              result = constLhs->getZExtValue() <= constRhs->getZExtValue();
              break;
            case ICmpInst::ICMP_UGT:
              result = constLhs->getZExtValue() > constRhs->getZExtValue();
              break;
            case ICmpInst::ICMP_UGE:
              result = constLhs->getZExtValue() >= constRhs->getZExtValue();
              break;
            default:
              continue;
          }

          cmp->replaceAllUsesWith(ConstantInt::get(
            IntegerType::getInt1Ty(cmp->getContext()), result ? 1 : 0));
          instToErase.push_back(cmp);
          ++foldedTimes;
        }
      }

      for (auto* i : instToErase)
        i->eraseFromParent();
    }
  }

  return foldedTimes;
}

PreservedAnalyses
ConstantPropagation::run(Module& mod, ModuleAnalysisManager& mam)
{
  int totalPropagated = 0;
  int totalFolded = 0;

  // 循环执行常量传播和常量折叠，直到没有新的变换发生
  while (true) {
    int propagated = constantPropagation(mod);
    int folded = constantFolding(mod);
    totalPropagated += propagated;
    totalFolded += folded;

    // 本轮没有发生任何变换，退出循环
    if (propagated == 0 && folded == 0)
      break;
  }

  mOut << "ConstantPropagation running...\n"
       << "Propagated " << totalPropagated << " loads\n"
       << "Folded " << totalFolded << " instructions\n";

  if (totalPropagated == 0 && totalFolded == 0)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
