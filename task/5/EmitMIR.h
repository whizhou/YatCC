#pragma once

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

void emitModule(const llvm::Module& mod, llvm::raw_ostream& out);
