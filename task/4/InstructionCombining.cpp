#include "InstructionCombining.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <vector>

using namespace llvm;

/// 尝试对二元运算指令进行指令合并
/// 返回新值（Instruction* 或 Constant*）或 nullptr
///
/// 支持的模式：
/// 1. 恒等消除：add(x,0)->x, sub(x,0)->x, mul(x,1)->x, div(x,1)->x
/// 2. 零化：mul(x,0)->0, and(x,0)->0
/// 3. 常量链合并：add(add(x,C1),C2)->add(x,C1+C2) 等
/// 4. 自身消除：sub(x,x)->0
static Value*
tryCombine(BinaryOperator* binOp)
{
  Value* lhs = binOp->getOperand(0);
  Value* rhs = binOp->getOperand(1);
  auto* constRhs = dyn_cast<ConstantInt>(rhs);
  auto* constLhs = dyn_cast<ConstantInt>(lhs);
  Type* ty = binOp->getType();

  switch (binOp->getOpcode()) {
    case Instruction::Add: {
      // add(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr; // 直接用 lhs 替换，不创建新指令
      if (constLhs && constLhs->isZero())
        return nullptr;

      // add(add(x, C1), C2) -> add(x, C1+C2)
      // 或 add(C2, add(x, C1)) -> add(x, C1+C2)
      if (auto* innerAdd = dyn_cast<BinaryOperator>(lhs)) {
        if (innerAdd->getOpcode() == Instruction::Add && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(1))) {
            // add(add(x, C1), C2) -> add(x, C1+C2)
            int64_t newConst = innerConst->getSExtValue() + constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(0), newC, "",
                                              binOp);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(0))) {
            // add(add(C1, x), C2) -> add(x, C1+C2)
            int64_t newConst = innerConst->getSExtValue() + constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(1), newC, "",
                                              binOp);
          }
        }
      }
      // add(C1, add(x, C2)) -> add(x, C1+C2)
      if (auto* innerAdd = dyn_cast<BinaryOperator>(rhs)) {
        if (innerAdd->getOpcode() == Instruction::Add && constLhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(1))) {
            int64_t newConst = constLhs->getSExtValue() + innerConst->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(0), newC, "",
                                              binOp);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(0))) {
            int64_t newConst = constLhs->getSExtValue() + innerConst->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(1), newC, "",
                                              binOp);
          }
        }
      }
      break;
    }

    case Instruction::Sub: {
      // sub(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;

      // sub(x, x) -> 0
      if (lhs == rhs)
        return ConstantInt::get(ty, 0);

      // sub(sub(x, C1), C2) -> sub(x, C1+C2)
      if (auto* innerSub = dyn_cast<BinaryOperator>(lhs)) {
        if (innerSub->getOpcode() == Instruction::Sub && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerSub->getOperand(1))) {
            int64_t newConst = innerConst->getSExtValue() + constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateSub(innerSub->getOperand(0), newC, "",
                                              binOp);
          }
        }
      }

      // sub(add(x, C1), C2) -> add(x, C1-C2)
      if (auto* innerAdd = dyn_cast<BinaryOperator>(lhs)) {
        if (innerAdd->getOpcode() == Instruction::Add && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(1))) {
            int64_t newConst = innerConst->getSExtValue() - constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(0), newC, "",
                                              binOp);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(0))) {
            int64_t newConst = innerConst->getSExtValue() - constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(1), newC, "",
                                              binOp);
          }
        }
      }

      // add(sub(x, C1), C2) -> sub(x, C1-C2) 或 add(x, C2-C1)
      // 简化为 add(x, C2-C1)
      if (auto* innerSub = dyn_cast<BinaryOperator>(rhs)) {
        if (innerSub->getOpcode() == Instruction::Sub && constLhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerSub->getOperand(1))) {
            int64_t newConst = constLhs->getSExtValue() - innerConst->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerSub->getOperand(0), newC, "",
                                              binOp);
          }
        }
      }
      break;
    }

    case Instruction::Mul: {
      // mul(x, 1) -> x
      if (constRhs && constRhs->isOne())
        return nullptr;
      if (constLhs && constLhs->isOne())
        return nullptr;

      // mul(x, 0) -> 0
      if (constRhs && constRhs->isZero())
        return ConstantInt::get(ty, 0);
      if (constLhs && constLhs->isZero())
        return ConstantInt::get(ty, 0);

      // mul(mul(x, C1), C2) -> mul(x, C1*C2)
      if (auto* innerMul = dyn_cast<BinaryOperator>(lhs)) {
        if (innerMul->getOpcode() == Instruction::Mul && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerMul->getOperand(1))) {
            int64_t newConst = innerConst->getSExtValue() * constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateMul(innerMul->getOperand(0), newC, "",
                                              binOp);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerMul->getOperand(0))) {
            int64_t newConst = innerConst->getSExtValue() * constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateMul(innerMul->getOperand(1), newC, "",
                                              binOp);
          }
        }
      }
      break;
    }

    case Instruction::SDiv:
    case Instruction::UDiv: {
      // div(x, 1) -> x
      if (constRhs && constRhs->isOne())
        return nullptr;
      break;
    }

    case Instruction::And: {
      // and(x, 0) -> 0
      if (constRhs && constRhs->isZero())
        return ConstantInt::get(ty, 0);
      if (constLhs && constLhs->isZero())
        return ConstantInt::get(ty, 0);
      break;
    }

    case Instruction::Or: {
      // or(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;
      if (constLhs && constLhs->isZero())
        return nullptr;
      break;
    }

    case Instruction::Xor: {
      // xor(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;
      if (constLhs && constLhs->isZero())
        return nullptr;
      // xor(x, x) -> 0
      if (lhs == rhs)
        return ConstantInt::get(ty, 0);
      break;
    }

    default:
      break;
  }

  return nullptr;
}

static int
combineInstructions(Module& mod)
{
  int combined = 0;
  std::vector<Instruction*> toErase;

  for (auto& func : mod) {
    for (auto& bb : func) {
      // 收集基本块中的所有二元运算指令（避免迭代器失效）
      std::vector<BinaryOperator*> worklist;
      for (auto& inst : bb) {
        if (auto* binOp = dyn_cast<BinaryOperator>(&inst))
          worklist.push_back(binOp);
      }

      for (BinaryOperator* binOp : worklist) {
        // 可能已被之前的合并操作替换
        if (binOp->use_empty() && !binOp->getParent())
          continue;

        // 恒等消除：直接用操作数替换
        Value* lhs = binOp->getOperand(0);
        Value* rhs = binOp->getOperand(1);
        auto* constRhs = dyn_cast<ConstantInt>(rhs);
        auto* constLhs = dyn_cast<ConstantInt>(lhs);

        bool isIdentity = false;
        switch (binOp->getOpcode()) {
          case Instruction::Add:
            isIdentity = (constRhs && constRhs->isZero()) ||
                         (constLhs && constLhs->isZero());
            break;
          case Instruction::Sub:
            isIdentity = constRhs && constRhs->isZero();
            break;
          case Instruction::Mul:
            isIdentity = (constRhs && constRhs->isOne()) ||
                         (constLhs && constLhs->isOne());
            break;
          case Instruction::SDiv:
          case Instruction::UDiv:
            isIdentity = constRhs && constRhs->isOne();
            break;
          case Instruction::Or:
            isIdentity = (constRhs && constRhs->isZero()) ||
                         (constLhs && constLhs->isZero());
            break;
          case Instruction::Xor:
            isIdentity = (constRhs && constRhs->isZero()) ||
                         (constLhs && constLhs->isZero());
            break;
          default:
            break;
        }

        if (isIdentity) {
          Value* replacement =
            (constRhs && constRhs->isZero()) || (constRhs && constRhs->isOne())
              ? lhs
              : rhs;
          binOp->replaceAllUsesWith(replacement);
          toErase.push_back(binOp);
          ++combined;
          continue;
        }

        // 尝试指令合并
        Value* newVal = tryCombine(binOp);
        if (newVal) {
          if (auto* newInst = dyn_cast<Instruction>(newVal)) {
            // 将新指令插入到原指令之后
            newInst->insertAfter(binOp);
            // 复制 nsw/nuw 标志
            if (auto* newBinOp = dyn_cast<BinaryOperator>(newInst)) {
              newBinOp->setHasNoUnsignedWrap(binOp->hasNoUnsignedWrap());
              newBinOp->setHasNoSignedWrap(binOp->hasNoSignedWrap());
            }
            // 新指令可能还可以继续合并，加入 worklist
            if (auto* newBinOp = dyn_cast<BinaryOperator>(newInst))
              worklist.push_back(newBinOp);
          }
          binOp->replaceAllUsesWith(newVal);
          toErase.push_back(binOp);
          ++combined;
        }
      }
    }
  }

  // 删除被合并的指令
  for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
    Instruction* inst = *it;
    if (inst->getParent() != nullptr)
      inst->eraseFromParent();
  }

  return combined;
}

/// 清理因指令合并而变为死代码的指令
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
InstructionCombining::run(Module& mod, ModuleAnalysisManager& mam)
{
  int combined = combineInstructions(mod);

  if (combined > 0)
    cleanupDeadInstructions(mod);

  mOut << "InstructionCombining running...\n"
       << "Combined " << combined << " instructions\n";

  if (combined == 0)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
