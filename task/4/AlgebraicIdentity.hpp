#pragma once

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/raw_ostream.h>

/// 代数恒等式优化 Pass
/// 通过数学规则消除无意义的数学运算，提前将结果算出
///
/// 支持的优化：
/// 1. 算术恒等式：add(x,0)->x, sub(x,0)->x, mul(x,1)->x, div(x,1)->x 等
/// 2. 零化规则：mul(x,0)->0, and(x,0)->0 等
/// 3. 幂等规则：and(x,x)->x, or(x,x)->x, sub(x,x)->0, xor(x,x)->0
/// 4. 负数/补码规则：mul(x,-1)->neg(x), sdiv(x,-1)->neg(x)
/// 5. 幂运算规则：mul(x,2)->shl(x,1), mul(x,4)->shl(x,2), sdiv(x,2)->ashr(x,1), udiv(x,2)->lshr(x,1)
/// 6. 取模规则：urem(x,1)->0, srem(x,1)->0, urem(x,2^n)->and(x,2^n-1)
/// 7. 常量链合并：add(add(x,C1),C2)->add(x,C1+C2) 等
/// 8. 常量折叠：两个常量操作数直接计算结果
class AlgebraicIdentity
  : public llvm::PassInfoMixin<AlgebraicIdentity>
{
public:
  explicit AlgebraicIdentity(llvm::raw_ostream& out)
    : mOut(out)
  {
  }

  llvm::PreservedAnalyses run(llvm::Module& mod,
                              llvm::ModuleAnalysisManager& mam);

private:
  llvm::raw_ostream& mOut;
};
