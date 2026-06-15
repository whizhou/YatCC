#include "StrengthReduction.hpp"

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

/// 对 mul x, C 进行强度削弱，使用 shift + add/sub 组合
/// 将 C 分解为连续的 1-bit 区间，减少操作数
static Value*
strengthReduceMul(ConstantInt* c, Value* x, Type* ty)
{
  uint64_t val = c->getZExtValue();

  // 2 的幂：直接左移
  if (c->getValue().isPowerOf2()) {
    unsigned shift = c->getValue().countTrailingZeros();
    auto* shiftAmt = ConstantInt::get(ty, shift);
    return BinaryOperator::CreateShl(
      x, shiftAmt, "", (Instruction*)nullptr);
  }

  // 尝试分解为 shift + add/sub 组合
  // 例如：x * 7 = x * (8 - 1) = (x << 3) - x
  //       x * 15 = x * (16 - 1) = (x << 4) - x
  //       x * 6  = x * (4 + 2)  = (x << 2) + (x << 1)
  // 策略：找最长的连续 1-bit 区间，用 (x << (high+1)) - (x << low) 表示

  unsigned width = ty->getScalarSizeInBits();
  // 从低位开始找最长的连续 1-bit 区间
  unsigned bestLow = 0, bestLen = 0;
  unsigned curLow = 0, curLen = 0;
  for (unsigned i = 0; i < width; ++i) {
    if ((val >> i) & 1) {
      if (curLen == 0)
        curLow = i;
      ++curLen;
      if (curLen > bestLen) {
        bestLen = curLen;
        bestLow = curLow;
      }
    } else {
      curLen = 0;
    }
  }

  // 最长连续 1-bit 区间长度 >= 2 时，使用减法优化
  if (bestLen >= 2) {
    // val 包含一个从 bestLow 到 bestLow+bestLen-1 的连续 1 区间
    // 剩余部分为 val_without_run
    uint64_t runMask = ((1ULL << bestLen) - 1) << bestLow;
    uint64_t remainder = val & ~runMask;

    // 构造 x * val = x * (runMask + remainder)
    //             = (x << (bestLow + bestLen)) - (x << bestLow) + x * remainder
    Value* result = nullptr;

    // (x << (bestLow + bestLen)) - (x << bestLow)
    auto* highShiftAmt = ConstantInt::get(ty, bestLow + bestLen);
    auto* lowShiftAmt = ConstantInt::get(ty, bestLow);
    Instruction* highShift =
      BinaryOperator::CreateShl(x, highShiftAmt, "", (Instruction*)nullptr);
    Instruction* lowShift =
      BinaryOperator::CreateShl(x, lowShiftAmt, "", (Instruction*)nullptr);
    Instruction* sub =
      BinaryOperator::CreateSub(highShift, lowShift, "", (Instruction*)nullptr);
    sub->insertBefore(lowShift);
    lowShift->insertBefore(sub);
    result = sub;

    // 如果有剩余部分，递归处理
    if (remainder != 0) {
      auto* remConst = cast<ConstantInt>(ConstantInt::get(ty, remainder));
      Value* remMul = strengthReduceMul(remConst, x, ty);
      if (remMul) {
        if (auto* remInst = dyn_cast<Instruction>(remMul)) {
          remInst->insertBefore(sub);
        }
        result =
        BinaryOperator::CreateAdd(result, remMul, "", (Instruction*)nullptr);
      }
    }
    return result;
  }

  // 否则，逐 bit 分解为 add + shift 链
  // val = sum of (1 << i) for each set bit i
  Value* result = nullptr;
  for (unsigned i = 0; i < width; ++i) {
    if (!((val >> i) & 1))
      continue;

    Value* shifted;
    if (i == 0) {
      shifted = x;
    } else {
      auto* shiftAmt = ConstantInt::get(ty, i);
      shifted =
        BinaryOperator::CreateShl(x, shiftAmt, "", (Instruction*)nullptr);
      if (auto* shiftInst = dyn_cast<Instruction>(shifted)) {
        if (result && isa<Instruction>(result))
          shiftInst->insertAfter(cast<Instruction>(result));
      }
    }

    if (!result) {
      result = shifted;
    } else {
      auto* add =
        BinaryOperator::CreateAdd(result, shifted, "", (Instruction*)nullptr);
      if (auto* shiftedInst = dyn_cast<Instruction>(shifted))
        add->insertAfter(shiftedInst);
      else if (auto* resultInst = dyn_cast<Instruction>(result))
        add->insertAfter(resultInst);
      result = add;
    }
  }

  return result;
}

/// 对二元运算指令尝试强度削弱
/// 返回 true 表示指令已被替换
static bool
tryStrengthReduce(BinaryOperator* binOp, std::vector<Instruction*>& toErase)
{
  Value* lhs = binOp->getOperand(0);
  Value* rhs = binOp->getOperand(1);
  Type* ty = binOp->getType();

  switch (binOp->getOpcode()) {

    // ============================================================
    // 乘法强度削弱：mul x, C -> shift + add/sub
    // ============================================================
    case Instruction::Mul: {
      auto* constRhs = dyn_cast<ConstantInt>(rhs);
      auto* constLhs = dyn_cast<ConstantInt>(lhs);

      if (constRhs && !constRhs->isZero() && !constRhs->isOne()) {
        Value* x = lhs;
        Value* newVal = strengthReduceMul(constRhs, x, ty);
        if (newVal && newVal != binOp) {
          if (auto* newInst = dyn_cast<Instruction>(newVal))
            newInst->insertBefore(binOp);
          binOp->replaceAllUsesWith(newVal);
          toErase.push_back(binOp);
          return true;
        }
      }

      if (constLhs && !constLhs->isZero() && !constLhs->isOne()) {
        Value* x = rhs;
        Value* newVal = strengthReduceMul(constLhs, x, ty);
        if (newVal && newVal != binOp) {
          if (auto* newInst = dyn_cast<Instruction>(newVal))
            newInst->insertBefore(binOp);
          binOp->replaceAllUsesWith(newVal);
          toErase.push_back(binOp);
          return true;
        }
      }
      break;
    }

    // ============================================================
    // 无符号除法强度削弱：udiv x, 2^n -> lshr x, n
    // ============================================================
    case Instruction::UDiv: {
      auto* constRhs = dyn_cast<ConstantInt>(rhs);
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        auto* shiftAmt = ConstantInt::get(ty, shift);
        auto* newInst = BinaryOperator::CreateLShr(lhs, shiftAmt, "", binOp);
        binOp->replaceAllUsesWith(newInst);
        toErase.push_back(binOp);
        return true;
      }
      break;
    }

    // ============================================================
    // 有符号除法强度削弱：sdiv x, 2^n -> 带偏置的 ashr
    // sdiv x, d = ashr (x + ((x >> (w-1)) >> (w - log2(d))), log2(d))
    // 其中 (x >> (w-1)) 提取符号位，再右移得到偏置
    // ============================================================
    case Instruction::SDiv: {
      auto* constRhs = dyn_cast<ConstantInt>(rhs);
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        unsigned width = ty->getScalarSizeInBits();

        if (shift == 0) {
          // 除以 1，恒等消除
          binOp->replaceAllUsesWith(lhs);
          toErase.push_back(binOp);
          return true;
        }

        // 计算偏置：(x >> (w-1)) >> (w - shift)
        // 对于正数，偏置为 0；对于负数，偏置为 2^shift - 1
        auto* wMinus1 = ConstantInt::get(ty, width - 1);
        auto* wMinusShift = ConstantInt::get(ty, width - shift);
        auto* shiftAmt = ConstantInt::get(ty, shift);

        // %bias = ashr x, (w-1)  — 提取符号位（全 0 或全 1）
        auto* signBit =
          BinaryOperator::CreateAShr(lhs, wMinus1, "", binOp);
        // %adj_bias = lshr %bias, (w - shift)
        auto* adjBias =
          BinaryOperator::CreateLShr(signBit, wMinusShift, "", binOp);
        // %adjusted = add x, %adj_bias
        auto* adjusted =
          BinaryOperator::CreateAdd(lhs, adjBias, "", binOp);
        // %result = ashr %adjusted, shift
        auto* result =
          BinaryOperator::CreateAShr(adjusted, shiftAmt, "", binOp);

        binOp->replaceAllUsesWith(result);
        toErase.push_back(binOp);
        return true;
      }
      break;
    }

    // ============================================================
    // 无符号取模强度削弱：urem x, 2^n -> and x, (2^n - 1)
    // ============================================================
    case Instruction::URem: {
      auto* constRhs = dyn_cast<ConstantInt>(rhs);
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        auto* mask = ConstantInt::get(ty, (1ULL << shift) - 1);
        auto* newInst = BinaryOperator::CreateAnd(lhs, mask, "", binOp);
        binOp->replaceAllUsesWith(newInst);
        toErase.push_back(binOp);
        return true;
      }
      break;
    }

    // ============================================================
    // 有符号取模强度削弱：srem x, 2^n -> 带符号修正的 and
    // srem x, d 的正确结果：
    //   abs(x) & (d-1)，如果 x >= 0
    //   - (abs(x) & (d-1))，如果 x < 0 且余数非零
    // ============================================================
    case Instruction::SRem: {
      auto* constRhs = dyn_cast<ConstantInt>(rhs);
      if (constRhs && isPowerOf2(constRhs)) {
        unsigned shift = getPowerOf2Index(constRhs);
        unsigned width = ty->getScalarSizeInBits();

        if (shift == 0) {
          // 取模 1 恒为 0
          auto* zero = ConstantInt::get(ty, 0);
          binOp->replaceAllUsesWith(zero);
          toErase.push_back(binOp);
          return true;
        }

        auto* mask = ConstantInt::get(ty, (1ULL << shift) - 1);
        auto* zero = ConstantInt::get(ty, 0);
        auto* wMinus1 = ConstantInt::get(ty, width - 1);

        // %sign = ashr x, (w-1)  — 全 0 或全 1
        auto* sign = BinaryOperator::CreateAShr(lhs, wMinus1, "", binOp);
        // %abs_x = (x ^ sign) - sign  — 取绝对值
        auto* xored = BinaryOperator::CreateXor(lhs, sign, "", binOp);
        auto* absX = BinaryOperator::CreateSub(xored, sign, "", binOp);
        // %abs_rem = and %abs_x, mask
        auto* absRem = BinaryOperator::CreateAnd(absX, mask, "", binOp);
        // %neg_rem = sub 0, %abs_rem  — 取负
        auto* negRem = BinaryOperator::CreateSub(zero, absRem, "", binOp);
        // %is_neg = icmp slt x, 0
        auto* isNeg = new ICmpInst(binOp, ICmpInst::ICMP_SLT, lhs, zero);
        // %result = select %is_neg, %neg_rem, %abs_rem
        auto* result = SelectInst::Create(isNeg, negRem, absRem, "", binOp);

        binOp->replaceAllUsesWith(result);
        toErase.push_back(binOp);
        return true;
      }
      break;
    }

    default:
      break;
  }

  return false;
}

static int
reduceStrength(Module& mod)
{
  int reduced = 0;
  std::vector<Instruction*> toErase;

  for (auto& func : mod) {
    for (auto& bb : func) {
      std::vector<BinaryOperator*> worklist;
      for (auto& inst : bb) {
        if (auto* binOp = dyn_cast<BinaryOperator>(&inst))
          worklist.push_back(binOp);
      }

      for (BinaryOperator* binOp : worklist) {
        if (binOp->getParent() == nullptr)
          continue;

        if (tryStrengthReduce(binOp, toErase))
          ++reduced;
      }
    }
  }

  for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
    Instruction* inst = *it;
    if (inst->getParent() != nullptr)
      inst->eraseFromParent();
  }

  return reduced;
}

PreservedAnalyses
StrengthReduction::run(Module& mod, ModuleAnalysisManager& mam)
{
  int reduced = reduceStrength(mod);

  mOut << "StrengthReduction running...\n"
       << "Reduced " << reduced << " instructions\n";

  if (reduced == 0)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
