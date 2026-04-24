#pragma once

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/Register.h>
#include <llvm/MC/MCInst.h>
#include <llvm/Support/raw_ostream.h>

#include "AllocReg.h"

class RvAsmEmitter final {
public:
  explicit RvAsmEmitter(llvm::raw_ostream& out);

  void emitDirective(llvm::StringRef s);
  void emitLabel(llvm::StringRef name);
  void emitLine(llvm::StringRef s);

private:
  llvm::raw_ostream& out_;
};

class RvInstEmitter final {
public:
  RvInstEmitter(RvAsmEmitter& as,
                const llvm::DenseMap<llvm::Register, int32_t>& spill,
                int32_t localBytes,
                int32_t saveRaOff,
                int32_t saveS0Off);

  void emit(const llvm::SmallVectorImpl<VInst>& insts,
            const llvm::DenseMap<llvm::Register, llvm::Register>& physMap);

private:
  RvAsmEmitter& as_;
  const llvm::DenseMap<llvm::Register, int32_t>& spill_;
  int32_t localBytes_ = 0;
  int32_t saveRaOff_ = 0;
  int32_t saveS0Off_ = 0;
  llvm::DenseMap<llvm::Register, llvm::Register> physMap_;

  static llvm::StringRef regName(llvm::Register reg);
  int32_t spillSlot(llvm::Register vreg) const;
  llvm::Register physOrInvalid(llvm::Register vreg) const;
  llvm::Register useReg(llvm::Register vreg, llvm::Register scratch);
  llvm::Register defReg(llvm::Register vreg, llvm::Register scratch);
  void storeIfSpilled(llvm::Register vreg, llvm::Register reg);
  void emitEpilogue();
  void emitBranchUncond(llvm::StringRef target);
  void emitBranchCond(llvm::Register cond, llvm::StringRef target, bool branch_on_zero);
  void emitInsts(const llvm::SmallVectorImpl<VInst>& insts);
  void emitMCInst(const VInst& vi);
  void emitInst(const VInst& vi);
  void spillAllPhysBeforeCall();
};
