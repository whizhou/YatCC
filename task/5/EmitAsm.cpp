#include "EmitAsm.h"

#include <llvm/Support/ErrorHandling.h>

#define GET_REGINFO_ENUM
#include "RISCVGenRegisterInfo.inc"
#undef GET_REGINFO_ENUM

#define GET_INSTRINFO_ENUM
#include "RISCVGenInstrInfo.inc"
#undef GET_INSTRINFO_ENUM

RvAsmEmitter::RvAsmEmitter(llvm::raw_ostream& out)
    : out_(out)
{
}

void RvAsmEmitter::emitDirective(llvm::StringRef s)
{
  out_ << s << "\n";
}

void RvAsmEmitter::emitLabel(llvm::StringRef name)
{
  out_ << name << ":\n";
}

void RvAsmEmitter::emitLine(llvm::StringRef s)
{
  out_ << "\t" << s << "\n";
}

RvInstEmitter::RvInstEmitter(RvAsmEmitter& as,
                             const llvm::DenseMap<llvm::Register, int32_t>& spill,
                             int32_t localBytes,
                             int32_t saveRaOff,
                             int32_t saveS0Off)
    : as_(as)
    , spill_(spill)
    , localBytes_(localBytes)
    , saveRaOff_(saveRaOff)
    , saveS0Off_(saveS0Off)
{
}

void RvInstEmitter::emit(const llvm::SmallVectorImpl<VInst>& insts,
                         const llvm::DenseMap<llvm::Register, llvm::Register>& physMap)
{
  physMap_ = physMap;
  emitInsts(insts);
}

llvm::StringRef RvInstEmitter::regName(llvm::Register reg)
{
  switch (reg.id()) {
    case llvm::RISCV::X0: return "zero";
    case llvm::RISCV::X1: return "ra";
    case llvm::RISCV::X2: return "sp";
    case llvm::RISCV::X5: return "t0";
    case llvm::RISCV::X6: return "t1";
    case llvm::RISCV::X7: return "t2";
    case llvm::RISCV::X28: return "t3";
    case llvm::RISCV::X29: return "t4";
    case llvm::RISCV::X30: return "t5";
    case llvm::RISCV::X31: return "t6";
    case llvm::RISCV::X8: return "s0";
    case llvm::RISCV::X10: return "a0";
    case llvm::RISCV::X11: return "a1";
    case llvm::RISCV::X12: return "a2";
    case llvm::RISCV::X13: return "a3";
    case llvm::RISCV::X14: return "a4";
    case llvm::RISCV::X15: return "a5";
    case llvm::RISCV::X16: return "a6";
    case llvm::RISCV::X17: return "a7";
    default: break;
  }
  return "x";
}

int32_t RvInstEmitter::spillSlot(llvm::Register vreg) const
{
  auto it = spill_.find(vreg);
  if (it == spill_.end())
    llvm::report_fatal_error("missing spill slot for vreg");
  return it->second;
}

llvm::Register RvInstEmitter::physOrInvalid(llvm::Register vreg) const
{
  if (!vreg.isVirtual())
    return vreg;
  auto it = physMap_.find(vreg);
  if (it == physMap_.end())
    return llvm::Register();
  return it->second;
}

llvm::Register RvInstEmitter::useReg(llvm::Register vreg, llvm::Register scratch)
{
  if (!vreg.isVirtual())
    return vreg;
  llvm::Register phys = physOrInvalid(vreg);
  if (phys.isValid())
    return phys;
  as_.emitLine("ld " + regName(scratch).str() + ", " +
               std::to_string(spillSlot(vreg)) + "(sp)");
  return scratch;
}

llvm::Register RvInstEmitter::defReg(llvm::Register vreg, llvm::Register scratch)
{
  if (!vreg.isVirtual())
    return vreg;
  llvm::Register phys = physOrInvalid(vreg);
  if (phys.isValid())
    return phys;
  return scratch;
}

void RvInstEmitter::storeIfSpilled(llvm::Register vreg, llvm::Register reg)
{
  if (!vreg.isVirtual())
    return;
  if (physOrInvalid(vreg).isValid())
    return;
  as_.emitLine("sd " + regName(reg).str() + ", " +
               std::to_string(spillSlot(vreg)) + "(sp)");
}

void RvInstEmitter::emitEpilogue()
{
  as_.emitLine("ld ra, " + std::to_string(saveRaOff_) + "(sp)");
  as_.emitLine("ld s0, " + std::to_string(saveS0Off_) + "(sp)");
  as_.emitLine("addi sp, sp, " + std::to_string(localBytes_));
  as_.emitLine("ret");
}

void RvInstEmitter::emitBranchUncond(llvm::StringRef target)
{
  as_.emitLine("j " + target.str());
}

void RvInstEmitter::emitBranchCond(llvm::Register cond, llvm::StringRef target, bool branch_on_zero)
{
  llvm::Register src = useReg(cond, llvm::Register(llvm::RISCV::X29));
  const char* opName = branch_on_zero ? "beqz" : "bnez";
  as_.emitLine(std::string(opName) + " " + regName(src).str() + ", " + target.str());
}

void RvInstEmitter::emitInsts(const llvm::SmallVectorImpl<VInst>& insts)
{
  for (const VInst& vi : insts) {
    emitInst(vi);
  }
}

void RvInstEmitter::emitMCInst(const VInst& vi)
{
  unsigned opc = vi.inst.getOpcode();
  auto& op = vi.inst;
  auto regAt = [&](unsigned i) {
    return llvm::Register(op.getOperand(i).getReg());
  };
  auto immAt = [&](unsigned i) {
    return op.getOperand(i).getImm();
  };

  switch (opc) {
    case llvm::RISCV::PseudoLA: {
      llvm::Register dst = defReg(regAt(0), llvm::Register(llvm::RISCV::X29));
      as_.emitLine("la " + regName(dst).str() + ", " + vi.sym);
      storeIfSpilled(regAt(0), dst);
      break;
    }
    case llvm::RISCV::ADDI:
    case llvm::RISCV::XORI:
    case llvm::RISCV::SLTIU: {
      llvm::Register src = useReg(regAt(1), llvm::Register(llvm::RISCV::X29));
      llvm::Register dst = defReg(regAt(0), llvm::Register(llvm::RISCV::X29));
      const char* opName = nullptr;
      if (opc == llvm::RISCV::ADDI)
        opName = "addi";
      else if (opc == llvm::RISCV::XORI)
        opName = "xori";
      else
        opName = "sltiu";
      as_.emitLine(std::string(opName) + " " + regName(dst).str() + ", " +
                   regName(src).str() + ", " + std::to_string(immAt(2)));
      storeIfSpilled(regAt(0), dst);
      break;
    }
    case llvm::RISCV::SLLI:
    case llvm::RISCV::SRLI:
    case llvm::RISCV::SRAI: {
      llvm::Register src = useReg(regAt(1), llvm::Register(llvm::RISCV::X29));
      llvm::Register dst = defReg(regAt(0), llvm::Register(llvm::RISCV::X29));
      const char* opName = (opc == llvm::RISCV::SLLI)
                               ? "slli"
                               : (opc == llvm::RISCV::SRLI) ? "srli" : "srai";
      as_.emitLine(std::string(opName) + " " + regName(dst).str() + ", " +
                   regName(src).str() + ", " + std::to_string(immAt(2)));
      storeIfSpilled(regAt(0), dst);
      break;
    }
    case llvm::RISCV::ADD:
    case llvm::RISCV::SUB:
    case llvm::RISCV::MUL:
    case llvm::RISCV::DIV:
    case llvm::RISCV::REM:
    case llvm::RISCV::XOR:
    case llvm::RISCV::SLT:
    case llvm::RISCV::SLTU:
    case llvm::RISCV::AND:
    case llvm::RISCV::OR:
    case llvm::RISCV::SLL:
    case llvm::RISCV::SRL:
    case llvm::RISCV::SRA: {
      llvm::Register lhs = useReg(regAt(1), llvm::Register(llvm::RISCV::X29));
      llvm::Register rhs = useReg(regAt(2), llvm::Register(llvm::RISCV::X30));
      llvm::Register dst = defReg(regAt(0), llvm::Register(llvm::RISCV::X29));
      std::string opName;
      switch (opc) {
        case llvm::RISCV::ADD: opName = "add"; break;
        case llvm::RISCV::SUB: opName = "sub"; break;
        case llvm::RISCV::MUL: opName = "mul"; break;
        case llvm::RISCV::DIV: opName = "div"; break;
        case llvm::RISCV::REM: opName = "rem"; break;
        case llvm::RISCV::XOR: opName = "xor"; break;
        case llvm::RISCV::SLT: opName = "slt"; break;
        case llvm::RISCV::SLTU: opName = "sltu"; break;
        case llvm::RISCV::AND: opName = "and"; break;
        case llvm::RISCV::OR: opName = "or"; break;
        case llvm::RISCV::SLL: opName = "sll"; break;
        case llvm::RISCV::SRL: opName = "srl"; break;
        case llvm::RISCV::SRA: opName = "sra"; break;
        default: break;
      }
      as_.emitLine(opName + " " + regName(dst).str() + ", " +
                   regName(lhs).str() + ", " + regName(rhs).str());
      storeIfSpilled(regAt(0), dst);
      break;
    }
    case llvm::RISCV::LD:
    case llvm::RISCV::LW: {
      llvm::Register base = useReg(regAt(1), llvm::Register(llvm::RISCV::X30));
      llvm::Register dst = defReg(regAt(0), llvm::Register(llvm::RISCV::X29));
      const char* opName = (opc == llvm::RISCV::LW) ? "lw" : "ld";
      as_.emitLine(std::string(opName) + " " + regName(dst).str() + ", " +
                   std::to_string(immAt(2)) + "(" + regName(base).str() + ")");
      storeIfSpilled(regAt(0), dst);
      break;
    }
    case llvm::RISCV::SD:
    case llvm::RISCV::SW: {
      llvm::Register val = useReg(regAt(0), llvm::Register(llvm::RISCV::X29));
      llvm::Register base = useReg(regAt(1), llvm::Register(llvm::RISCV::X30));
      const char* opName = (opc == llvm::RISCV::SW) ? "sw" : "sd";
      as_.emitLine(std::string(opName) + " " + regName(val).str() + ", " +
                   std::to_string(immAt(2)) + "(" + regName(base).str() + ")");
      break;
    }
    default:
      llvm::report_fatal_error("unsupported opcode in emitter");
  }
}

void RvInstEmitter::emitInst(const VInst& vi)
{
  switch (vi.kind) {
    case VInst::Kind::kMC:
      emitMCInst(vi);
      return;
    case VInst::Kind::kCall: {
      spillAllPhysBeforeCall();
      for (unsigned i = 0; i < vi.args.size() && i < 8; ++i) {
        llvm::Register src = useReg(vi.args[i], llvm::Register(llvm::RISCV::X29));
        as_.emitLine("mv a" + std::to_string(i) + ", " + regName(src).str());
      }
      as_.emitLine("call " + vi.sym);
      if (vi.has_ret) {
        llvm::Register dst = physOrInvalid(vi.ret);
        if (dst.isValid()) {
          if (dst.id() != llvm::RISCV::X10)
            as_.emitLine("mv " + regName(dst).str() + ", a0");
        } else {
          as_.emitLine("sd a0, " + std::to_string(spillSlot(vi.ret)) + "(sp)");
        }
      }
      return;
    }
    case VInst::Kind::kRet: {
      if (vi.has_ret) {
        llvm::Register src = physOrInvalid(vi.ret);
        if (src.isValid()) {
          if (src.id() != llvm::RISCV::X10)
            as_.emitLine("mv a0, " + regName(src).str());
        } else {
          as_.emitLine("ld a0, " + std::to_string(spillSlot(vi.ret)) + "(sp)");
        }
      }
      emitEpilogue();
      return;
    }
    case VInst::Kind::kBrUncond:
      spillAllPhysBeforeCall();
      emitBranchUncond(vi.sym);
      return;
    case VInst::Kind::kBrCond:
      spillAllPhysBeforeCall();
      emitBranchCond(vi.cond, vi.sym, vi.branch_on_zero);
      return;
  }
}

void RvInstEmitter::spillAllPhysBeforeCall()
{
  if (physMap_.empty())
    return;
  for (auto& kv : physMap_) {
    llvm::Register vreg = kv.first;
    llvm::Register preg = kv.second;
    as_.emitLine("sd " + regName(preg).str() + ", " +
                 std::to_string(spillSlot(vreg)) + "(sp)");
  }
  physMap_.clear();
}
