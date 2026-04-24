#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/CodeGen/Register.h>
#include <llvm/MC/MCInst.h>

struct VInst final {
  enum class Kind {
    kMC,
    kCall,
    kRet,
    kBrUncond,
    kBrCond,
  };

  Kind kind = Kind::kMC;
  llvm::MCInst inst;
  std::string sym;
  llvm::SmallVector<llvm::Register, 8> args;
  bool has_ret = false;
  llvm::Register ret;
  llvm::Register cond;
  bool branch_on_zero = false;
};

class LinearScanAllocator final {
public:
  LinearScanAllocator();

  void allocate(const llvm::SmallVectorImpl<VInst>& insts);

  const llvm::DenseMap<llvm::Register, llvm::Register>& physMap() const;

private:
  struct Interval final {
    llvm::Register vreg;
    int start = -1;
    int end = -1;
    bool spilled = false;
    llvm::Register phys;
  };

  bool forceSpillAll_ = true;
  llvm::DenseMap<llvm::Register, Interval> intervals_;
  llvm::DenseMap<llvm::Register, llvm::Register> physMap_;

  void touchInterval(llvm::Register vreg, int idx);
  void addUsesDefs(const VInst& vi, int idx);
  void buildIntervals(const llvm::SmallVectorImpl<VInst>& insts);
  void runLinearScan();
};
