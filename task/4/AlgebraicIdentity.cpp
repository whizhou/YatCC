#include "AlgebraicIdentity.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <vector>

using namespace llvm;

/// 判断一个值是否为 2 的幂（大于 0）
static bool
isPowerOf2(ConstantInt* c)
{
  if (c->isZero() || c->isNegative())
    return false;
  return c->getValue().isPowerOf2();
}

/// 获取 2 的幂的指数
static unsigned
getPowerOf2Index(ConstantInt* c)
{
  return c->getValue().countTrailingZeros();
}

/// 对二元运算指令尝试代数恒等式优化
/// 返回新值（Instruction* 或 Constant*）或 nullptr
/// nullptr 表示该指令为恒等操作，可直接用操作数替换
///
/// 支持的模式：
/// 1. 算术恒等式：add(x,0)->x, sub(x,0)->x, mul(x,1)->x, div(x,1)->x
/// 2. 零化：mul(x,0)->0, and(x,0)->0
/// 3. 幂等：and(x,x)->x, or(x,x)->x, sub(x,x)->0, xor(x,x)->0
/// 4. 负数/补码：mul(x,-1)->neg(x), sdiv(x,-1)->neg(x)
/// 5. 幂运算：mul(x,2^n)->shl(x,n), sdiv(x,2^n)->ashr(x,n), udiv(x,2^n)->lshr(x,n)
/// 6. 取模：urem(x,1)->0, srem(x,1)->0, urem(x,2^n)->and(x,2^n-1)
/// 7. 常量链合并：add(add(x,C1),C2)->add(x,C1+C2)
/// 8. 常量折叠：两个常量操作数直接计算
static Value*
tryAlgebraicIdentity(BinaryOperator* binOp)
{
  Value* lhs = binOp->getOperand(0);
  Value* rhs = binOp->getOperand(1);
  auto* constRhs = dyn_cast<ConstantInt>(rhs);
  auto* constLhs = dyn_cast<ConstantInt>(lhs);
  Type* ty = binOp->getType();

  switch (binOp->getOpcode()) {

    // ============================================================
    // 加法
    // ============================================================
    case Instruction::Add: {
      // add(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;
      if (constLhs && constLhs->isZero())
        return nullptr;

      // 常量折叠：add(C1, C2) -> C1+C2
      if (constLhs && constRhs) {
        return ConstantInt::getSigned(
          ty, constLhs->getSExtValue() + constRhs->getSExtValue());
      }

      // 常量链合并：add(add(x, C1), C2) -> add(x, C1+C2)
      if (auto* innerAdd = dyn_cast<BinaryOperator>(lhs)) {
        if (innerAdd->getOpcode() == Instruction::Add && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(1))) {
            int64_t newConst = innerConst->getSExtValue() + constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(0), newC, "",
                                              (Instruction*)nullptr);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(0))) {
            int64_t newConst = innerConst->getSExtValue() + constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(1), newC, "",
                                              (Instruction*)nullptr);
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
                                              (Instruction*)nullptr);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(0))) {
            int64_t newConst = constLhs->getSExtValue() + innerConst->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(1), newC, "",
                                              (Instruction*)nullptr);
          }
        }
      }
      break;
    }

    // ============================================================
    // 减法
    // ============================================================
    case Instruction::Sub: {
      // sub(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;

      // sub(x, x) -> 0
      if (lhs == rhs)
        return ConstantInt::get(ty, 0);

      // 注意：sub(0, x) 和 neg(x) 在 LLVM IR 中是同一条指令（CreateNeg 就是 sub(0, x)），
      // 不做转换以避免创建相同指令导致工作列表无限增长。

      // 常量折叠：sub(C1, C2) -> C1-C2
      if (constLhs && constRhs) {
        return ConstantInt::getSigned(
          ty, constLhs->getSExtValue() - constRhs->getSExtValue());
      }

      // 常量链合并：sub(sub(x, C1), C2) -> sub(x, C1+C2)
      if (auto* innerSub = dyn_cast<BinaryOperator>(lhs)) {
        if (innerSub->getOpcode() == Instruction::Sub && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerSub->getOperand(1))) {
            int64_t newConst = innerConst->getSExtValue() + constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateSub(innerSub->getOperand(0), newC, "",
                                              (Instruction*)nullptr);
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
                                              (Instruction*)nullptr);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerAdd->getOperand(0))) {
            int64_t newConst = innerConst->getSExtValue() - constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateAdd(innerAdd->getOperand(1), newC, "",
                                              (Instruction*)nullptr);
          }
        }
      }
      break;
    }

    // ============================================================
    // 乘法
    // ============================================================
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

      // 常量折叠：mul(C1, C2) -> C1*C2
      if (constLhs && constRhs) {
        return ConstantInt::getSigned(
          ty, constLhs->getSExtValue() * constRhs->getSExtValue());
      }

      // mul(x, -1) -> neg(x)
      if (constRhs && constRhs->isMinusOne())
        return BinaryOperator::CreateNeg(lhs, "", (Instruction*)nullptr);
      if (constLhs && constLhs->isMinusOne())
        return BinaryOperator::CreateNeg(rhs, "", (Instruction*)nullptr);

      // mul(x, 2^n) -> shl(x, n)
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        auto* shiftAmt = ConstantInt::get(ty, shift);
        return BinaryOperator::CreateShl(lhs, shiftAmt, "", (Instruction*)nullptr);
      }
      if (constLhs && isPowerOf2(constLhs)) {
        unsigned shift = getPowerOf2Index(constLhs);
        auto* shiftAmt = ConstantInt::get(ty, shift);
        return BinaryOperator::CreateShl(rhs, shiftAmt, "", (Instruction*)nullptr);
      }

      // 常量链合并：mul(mul(x, C1), C2) -> mul(x, C1*C2)
      if (auto* innerMul = dyn_cast<BinaryOperator>(lhs)) {
        if (innerMul->getOpcode() == Instruction::Mul && constRhs) {
          if (auto* innerConst = dyn_cast<ConstantInt>(innerMul->getOperand(1))) {
            int64_t newConst = innerConst->getSExtValue() * constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateMul(innerMul->getOperand(0), newC, "",
                                              (Instruction*)nullptr);
          }
          if (auto* innerConst = dyn_cast<ConstantInt>(innerMul->getOperand(0))) {
            int64_t newConst = innerConst->getSExtValue() * constRhs->getSExtValue();
            auto* newC = ConstantInt::getSigned(ty, newConst);
            return BinaryOperator::CreateMul(innerMul->getOperand(1), newC, "",
                                              (Instruction*)nullptr);
          }
        }
      }
      break;
    }

    // ============================================================
    // 有符号除法
    // ============================================================
    case Instruction::SDiv: {
      // div(x, 1) -> x
      if (constRhs && constRhs->isOne())
        return nullptr;

      // 常量折叠：sdiv(C1, C2) -> C1/C2
      if (constLhs && constRhs && !constRhs->isZero()) {
        return ConstantInt::getSigned(
          ty, constLhs->getSExtValue() / constRhs->getSExtValue());
      }

      // sdiv(x, -1) -> neg(x)
      if (constRhs && constRhs->isMinusOne())
        return BinaryOperator::CreateNeg(lhs, "", (Instruction*)nullptr);

      // sdiv(x, 2^n) -> 带偏置的 ashr
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        unsigned width = ty->getScalarSizeInBits();

        // 计算偏置：(x >> (w-1)) >> (w - shift)
        auto* wMinus1 = ConstantInt::get(ty, width - 1);
        auto* wMinusShift = ConstantInt::get(ty, width - shift);
        auto* shiftAmt = ConstantInt::get(ty, shift);

        // %sign = ashr x, (w-1)
        auto* signBit = BinaryOperator::CreateAShr(lhs, wMinus1, "", binOp);
        // %adj_bias = lshr %sign, (w - shift)
        auto* adjBias = BinaryOperator::CreateLShr(signBit, wMinusShift, "", binOp);
        // %adjusted = add x, %adj_bias
        auto* adjusted = BinaryOperator::CreateAdd(lhs, adjBias, "", binOp);
        // %result = ashr %adjusted, shift
        auto* result = BinaryOperator::CreateAShr(adjusted, shiftAmt, "", binOp);
        return result;
      }
      break;
    }

    // ============================================================
    // 无符号除法
    // ============================================================
    case Instruction::UDiv: {
      // div(x, 1) -> x
      if (constRhs && constRhs->isOne())
        return nullptr;

      // 常量折叠：udiv(C1, C2) -> C1/C2
      if (constLhs && constRhs && !constRhs->isZero()) {
        return ConstantInt::get(
          ty, constLhs->getZExtValue() / constRhs->getZExtValue());
      }

      // udiv(x, 2^n) -> lshr(x, n)
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        auto* shiftAmt = ConstantInt::get(ty, shift);
        return BinaryOperator::CreateLShr(lhs, shiftAmt, "", (Instruction*)nullptr);
      }
      break;
    }

    // ============================================================
    // 有符号取模
    // ============================================================
    case Instruction::SRem: {
      // srem(x, 1) -> 0
      if (constRhs && constRhs->isOne())
        return ConstantInt::get(ty, 0);

      // 常量折叠：srem(C1, C2) -> C1%C2
      if (constLhs && constRhs && !constRhs->isZero()) {
        return ConstantInt::getSigned(
          ty, constLhs->getSExtValue() % constRhs->getSExtValue());
      }
      break;
    }

    // ============================================================
    // 无符号取模
    // ============================================================
    case Instruction::URem: {
      // urem(x, 1) -> 0
      if (constRhs && constRhs->isOne())
        return ConstantInt::get(ty, 0);

      // 常量折叠：urem(C1, C2) -> C1%C2
      if (constLhs && constRhs && !constRhs->isZero()) {
        return ConstantInt::get(
          ty, constLhs->getZExtValue() % constRhs->getZExtValue());
      }

      // urem(x, 2^n) -> and(x, 2^n - 1)
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        auto* mask = ConstantInt::get(ty, (1ULL << shift) - 1);
        return BinaryOperator::CreateAnd(lhs, mask, "", (Instruction*)nullptr);
      }
      break;
    }

    // ============================================================
    // 按位与
    // ============================================================
    case Instruction::And: {
      // and(x, 0) -> 0
      if (constRhs && constRhs->isZero())
        return ConstantInt::get(ty, 0);
      if (constLhs && constLhs->isZero())
        return ConstantInt::get(ty, 0);

      // and(x, x) -> x
      if (lhs == rhs)
        return nullptr;

      // and(x, -1) -> x
      if (constRhs && constRhs->isMinusOne())
        return nullptr;
      if (constLhs && constLhs->isMinusOne())
        return nullptr;
      break;
    }

    // ============================================================
    // 按位或
    // ============================================================
    case Instruction::Or: {
      // or(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;
      if (constLhs && constLhs->isZero())
        return nullptr;

      // or(x, x) -> x
      if (lhs == rhs)
        return nullptr;

      // or(x, -1) -> -1
      if (constRhs && constRhs->isMinusOne())
        return ConstantInt::get(ty, -1);
      if (constLhs && constLhs->isMinusOne())
        return ConstantInt::get(ty, -1);
      break;
    }

    // ============================================================
    // 按位异或
    // ============================================================
    case Instruction::Xor: {
      // xor(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;
      if (constLhs && constLhs->isZero())
        return nullptr;

      // xor(x, x) -> 0
      if (lhs == rhs)
        return ConstantInt::get(ty, 0);

      // 注意：xor(x, -1) 和 not(x) 在 LLVM IR 中是同一条指令（CreateNot 就是 xor(x, -1)），
      // 不做转换以避免创建相同指令导致工作列表无限增长。
      break;
    }

    // ============================================================
    // 左移
    // ============================================================
    case Instruction::Shl: {
      // shl(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;

      // shl(0, x) -> 0
      if (constLhs && constLhs->isZero())
        return ConstantInt::get(ty, 0);

      // 常量折叠：shl(C1, C2) -> C1 << C2
      if (constLhs && constRhs) {
        uint64_t shift = constRhs->getZExtValue();
        if (shift < ty->getScalarSizeInBits())
          return ConstantInt::get(ty, constLhs->getZExtValue() << shift);
      }
      break;
    }

    // ============================================================
    // 逻辑右移
    // ============================================================
    case Instruction::LShr: {
      // lshr(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;

      // lshr(0, x) -> 0
      if (constLhs && constLhs->isZero())
        return ConstantInt::get(ty, 0);

      // 常量折叠：lshr(C1, C2) -> C1 >> C2
      if (constLhs && constRhs) {
        uint64_t shift = constRhs->getZExtValue();
        if (shift < ty->getScalarSizeInBits())
          return ConstantInt::get(ty, constLhs->getZExtValue() >> shift);
      }
      break;
    }

    // ============================================================
    // 算术右移
    // ============================================================
    case Instruction::AShr: {
      // ashr(x, 0) -> x
      if (constRhs && constRhs->isZero())
        return nullptr;

      // ashr(0, x) -> 0
      if (constLhs && constLhs->isZero())
        return ConstantInt::get(ty, 0);

      // 常量折叠：ashr(C1, C2) -> C1 >> C2 (算术)
      if (constLhs && constRhs) {
        uint64_t shift = constRhs->getZExtValue();
        if (shift < ty->getScalarSizeInBits())
          return ConstantInt::getSigned(ty, constLhs->getSExtValue() >> shift);
      }
      break;
    }

    default:
      break;
  }

  return nullptr;
}

/// 遍历模块，对每条二元运算指令尝试代数恒等式优化
static int
applyAlgebraicIdentities(Module& mod)
{
  int optimized = 0;
  std::vector<Instruction*> toErase;

  for (auto& func : mod) {
    for (auto& bb : func) {
      // 收集基本块中的所有二元运算指令（避免迭代器失效）
      std::vector<BinaryOperator*> worklist;
      for (auto& inst : bb) {
        if (auto* binOp = dyn_cast<BinaryOperator>(&inst))
          worklist.push_back(binOp);
      }

      for (size_t wi = 0; wi < worklist.size(); ++wi) {
        BinaryOperator* binOp = worklist[wi];
        // 可能已被之前的优化操作替换
        if (binOp->getParent() == nullptr)
          continue;
        if (binOp->use_empty() && !binOp->getParent())
          continue;

        // 先检查恒等消除（直接用操作数替换）
        Value* lhs = binOp->getOperand(0);
        Value* rhs = binOp->getOperand(1);
        auto* constRhs = dyn_cast<ConstantInt>(rhs);
        auto* constLhs = dyn_cast<ConstantInt>(lhs);

        bool isIdentity = false;
        Value* replacement = nullptr;

        switch (binOp->getOpcode()) {
          case Instruction::Add:
            if ((constRhs && constRhs->isZero()) ||
                (constLhs && constLhs->isZero())) {
              isIdentity = true;
              replacement = (constRhs && constRhs->isZero()) ? lhs : rhs;
            }
            break;
          case Instruction::Sub:
            if (constRhs && constRhs->isZero()) {
              isIdentity = true;
              replacement = lhs;
            }
            break;
          case Instruction::Mul:
            if ((constRhs && constRhs->isOne()) ||
                (constLhs && constLhs->isOne())) {
              isIdentity = true;
              replacement = (constRhs && constRhs->isOne()) ? lhs : rhs;
            }
            break;
          case Instruction::SDiv:
          case Instruction::UDiv:
            if (constRhs && constRhs->isOne()) {
              isIdentity = true;
              replacement = lhs;
            }
            break;
          case Instruction::And:
            if ((constRhs && constRhs->isMinusOne()) ||
                (constLhs && constLhs->isMinusOne())) {
              isIdentity = true;
              replacement = (constRhs && constRhs->isMinusOne()) ? lhs : rhs;
            }
            // and(x, x) -> x
            if (lhs == rhs) {
              isIdentity = true;
              replacement = lhs;
            }
            break;
          case Instruction::Or:
            if ((constRhs && constRhs->isZero()) ||
                (constLhs && constLhs->isZero())) {
              isIdentity = true;
              replacement = (constRhs && constRhs->isZero()) ? lhs : rhs;
            }
            // or(x, x) -> x
            if (lhs == rhs) {
              isIdentity = true;
              replacement = lhs;
            }
            break;
          case Instruction::Xor:
            if ((constRhs && constRhs->isZero()) ||
                (constLhs && constLhs->isZero())) {
              isIdentity = true;
              replacement = (constRhs && constRhs->isZero()) ? lhs : rhs;
            }
            break;
          case Instruction::Shl:
          case Instruction::LShr:
          case Instruction::AShr:
            if (constRhs && constRhs->isZero()) {
              isIdentity = true;
              replacement = lhs;
            }
            break;
          default:
            break;
        }

        if (isIdentity) {
          binOp->replaceAllUsesWith(replacement);
          toErase.push_back(binOp);
          ++optimized;
          continue;
        }

        // 尝试其他代数恒等式优化
        Value* newVal = tryAlgebraicIdentity(binOp);
        if (newVal) {
          if (auto* newInst = dyn_cast<Instruction>(newVal)) {
            // 将新指令插入到原指令之前（如果尚未插入）
            if (!newInst->getParent())
              newInst->insertBefore(binOp);
            // 复制 nsw/nuw 标志（仅 add/sub/mul 支持 OverflowingBinaryOperator）
            if (auto* newBinOp = dyn_cast<BinaryOperator>(newInst)) {
              unsigned srcOp = binOp->getOpcode();
              unsigned dstOp = newBinOp->getOpcode();
              auto supportsFlags = [](unsigned op) {
                return op == Instruction::Add || op == Instruction::Sub ||
                       op == Instruction::Mul;
              };
              if (supportsFlags(srcOp) && supportsFlags(dstOp)) {
                newBinOp->setHasNoUnsignedWrap(binOp->hasNoUnsignedWrap());
                newBinOp->setHasNoSignedWrap(binOp->hasNoSignedWrap());
              }
            }
            // 新指令可能还可以继续优化，加入 worklist
            if (auto* newBinOp = dyn_cast<BinaryOperator>(newInst))
              worklist.push_back(newBinOp);
          }
          binOp->replaceAllUsesWith(newVal);
          toErase.push_back(binOp);
          ++optimized;
        }
      }
    }
  }

  // 删除被优化的指令
  for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
    Instruction* inst = *it;
    if (inst->getParent() != nullptr)
      inst->eraseFromParent();
  }

  return optimized;
}

/// 清理因代数恒等式优化而变为死代码的指令
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
AlgebraicIdentity::run(Module& mod, ModuleAnalysisManager& mam)
{
  int optimized = applyAlgebraicIdentities(mod);

  if (optimized > 0)
    cleanupDeadInstructions(mod);

  mOut << "AlgebraicIdentity running...\n"
       << "Optimized " << optimized << " instructions\n";

  if (optimized == 0)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
