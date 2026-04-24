#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include "EmitMIR.h"

static constexpr const char* kTriple = "riscv64-unknown-linux-gnu";

int main(int argc, char** argv)
{
  llvm::InitLLVM init_llvm(argc, argv);

  if (argc != 3) {
    llvm::errs() << "Usage: " << argv[0] << " <input.ll> <output.s>\n";
    return 1;
  }

  llvm::LLVMContext ir_ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> mod = llvm::parseIRFile(argv[1], err, ir_ctx);
  if (!mod) {
    llvm::errs() << "Error: unable to parse input IR: " << argv[1] << "\n";
    err.print(argv[0], llvm::errs());
    return 2;
  }

  mod->setTargetTriple(kTriple);
  if (llvm::verifyModule(*mod, &llvm::errs())) {
    llvm::errs() << "Error: input module verification failed.\n";
    return 3;
  }

  std::error_code ec;
  llvm::raw_fd_ostream out(argv[2], ec, llvm::sys::fs::OF_Text);
  if (ec) {
    llvm::errs() << "Error: unable to open output file: " << argv[2]
                 << " (" << ec.message() << ")\n";
    return 10;
  }

  emitModule(*mod, out);
  out.flush();
  return 0;
}
