#include "AllocReg.h"

#include <algorithm>

#include <llvm/Support/ErrorHandling.h>

#define GET_REGINFO_ENUM
#include "RISCVGenRegisterInfo.inc"
#undef GET_REGINFO_ENUM

#define GET_INSTRINFO_ENUM
#include "RISCVGenInstrInfo.inc"
#undef GET_INSTRINFO_ENUM

LinearScanAllocator::LinearScanAllocator() = default;

void LinearScanAllocator::allocate(const llvm::SmallVectorImpl<VInst>& insts)
{
  if (insts.empty())
    return;
  buildIntervals(insts);
  runLinearScan();
}

const llvm::DenseMap<llvm::Register, llvm::Register>& LinearScanAllocator::physMap() const
{
  return physMap_;
}

void LinearScanAllocator::touchInterval(llvm::Register vreg, int idx)
{
  if (!vreg.isVirtual())
    return;
  Interval& it = intervals_[vreg];
  it.vreg = vreg;
  if (it.start < 0)
    it.start = idx;
  it.end = idx;
}

void LinearScanAllocator::addUsesDefs(const VInst& vi, int idx)
{
  switch (vi.kind) {
    case VInst::Kind::kCall:
      for (llvm::Register v : vi.args)
        touchInterval(v, idx);
      if (vi.has_ret)
        touchInterval(vi.ret, idx);
      return;
    case VInst::Kind::kRet:
      if (vi.has_ret)
        touchInterval(vi.ret, idx);
      return;
    case VInst::Kind::kBrCond:
      touchInterval(vi.cond, idx);
      return;
    case VInst::Kind::kBrUncond:
      return;
    case VInst::Kind::kMC:
      break;
  }

  unsigned opc = vi.inst.getOpcode();
  auto& op = vi.inst;
  auto regAt = [&](unsigned i) {
    return llvm::Register(op.getOperand(i).getReg());
  };

  switch (opc) {
    case llvm::RISCV::ADDI:
    case llvm::RISCV::XORI:
    case llvm::RISCV::LD:
    case llvm::RISCV::LW:
    case llvm::RISCV::SLTIU:
    case llvm::RISCV::SLLI:
    case llvm::RISCV::SRLI:
    case llvm::RISCV::SRAI:
      touchInterval(regAt(1), idx);
      touchInterval(regAt(0), idx);
      return;
    case llvm::RISCV::SD:
    case llvm::RISCV::SW:
      touchInterval(regAt(0), idx);
      touchInterval(regAt(1), idx);
      return;
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
    case llvm::RISCV::SRA:
      touchInterval(regAt(1), idx);
      touchInterval(regAt(2), idx);
      touchInterval(regAt(0), idx);
      return;
    case llvm::RISCV::PseudoLA:
      touchInterval(regAt(0), idx);
      return;
    default:
      llvm::report_fatal_error("unsupported opcode in linear scan");
  }
}

void LinearScanAllocator::buildIntervals(const llvm::SmallVectorImpl<VInst>& insts)
{
  intervals_.clear();
  physMap_.clear();
  for (int i = 0; i < static_cast<int>(insts.size()); ++i) {
    addUsesDefs(insts[i], i);
  }
}

// Physical reg map (RV64, caller-saved focus):
//
//   x0  -> zero: hardwired zero
//   x1  -> ra  : return address
//   x2  -> sp  : stack pointer
//   x3  -> gp  : global pointer
//   x4  -> tp  : thread pointer
//   x5  -> t0  : temp (caller-saved)
//   x6  -> t1  : temp (caller-saved)
//   x7  -> t2  : temp (caller-saved)
//   x8  -> s0  : frame pointer (callee-saved)
//   x9  -> s1  : callee-saved
//   x10 -> a0  : arg0 / return value
//   x11 -> a1  : arg1
//   x12 -> a2  : arg2
//   x13 -> a3  : arg3
//   x14 -> a4  : arg4
//   x15 -> a5  : arg5
//   x16 -> a6  : arg6
//   x17 -> a7  : arg7
//   x18 -> s2  : callee-saved
//   x19 -> s3  : callee-saved
//   x20 -> s4  : callee-saved
//   x21 -> s5  : callee-saved
//   x22 -> s6  : callee-saved
//   x23 -> s7  : callee-saved
//   x24 -> s8  : callee-saved
//   x25 -> s9  : callee-saved
//   x26 -> s10 : callee-saved
//   x27 -> s11 : callee-saved
//   x28 -> t3  : temp (caller-saved)
//   x29 -> t4  : temp (caller-saved)
//   x30 -> t5  : temp (caller-saved)
//   x31 -> t6  : temp (caller-saved)
void LinearScanAllocator::runLinearScan()
{
  // All temps are caller-saved; we spill before calls.
  if (forceSpillAll_) {
    physMap_.clear();
    for (auto& kv : intervals_) {
      kv.second.spilled = true;
      kv.second.phys = llvm::Register();
    }
    return;
  }
  std::vector<Interval*> sorted;
  sorted.reserve(intervals_.size());
  for (auto& kv : intervals_)
    sorted.push_back(&kv.second);

  std::sort(sorted.begin(), sorted.end(), [](const Interval* a, const Interval* b) {
    return a->start < b->start;
  });

  std::vector<Interval*> active;
  std::vector<llvm::Register> freeRegs = {
      llvm::Register(llvm::RISCV::X5),
      llvm::Register(llvm::RISCV::X6),
      llvm::Register(llvm::RISCV::X7),
      llvm::Register(llvm::RISCV::X28),
      llvm::Register(llvm::RISCV::X29),
      llvm::Register(llvm::RISCV::X30),
      llvm::Register(llvm::RISCV::X31),
  };

  auto expire = [&](int start) {
    active.erase(std::remove_if(active.begin(), active.end(), [&](Interval* it) {
      if (it->end < start) {
        freeRegs.push_back(it->phys);
        return true;
      }
      return false;
    }), active.end());
  };

  auto insertActive = [&](Interval* cur) {
    active.push_back(cur);
    std::sort(active.begin(), active.end(), [](const Interval* a, const Interval* b) {
      return a->end < b->end;
    });
  };

  for (Interval* cur : sorted) {
    expire(cur->start);
    if (!freeRegs.empty()) {
      cur->phys = freeRegs.back();
      freeRegs.pop_back();
      insertActive(cur);
      continue;
    }

    Interval* spill = active.back();
    if (spill->end > cur->end) {
      cur->phys = spill->phys;
      spill->spilled = true;
      spill->phys = llvm::Register();
      active.pop_back();
      insertActive(cur);
    } else {
      cur->spilled = true;
    }
  }

  for (auto& kv : intervals_) {
    Interval& it = kv.second;
    if (!it.spilled && it.phys.isValid())
      physMap_[it.vreg] = it.phys;
  }
}
