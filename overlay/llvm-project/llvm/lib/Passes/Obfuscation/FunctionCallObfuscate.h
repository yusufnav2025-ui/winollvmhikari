#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// FunctionCallObfuscate — replaces direct calls to external functions with
// runtime dlopen/dlsym resolution so call targets are not statically visible.
//
// Usage:
//   global:      -mllvm -fco
//   per-function: __attribute__((annotate("fco")))
//
// Only targets Android (AArch64 / ARM). Skips intrinsics, varargs,
// C++ ABI helpers, and stack-protector functions.
struct FunctionCallObfuscate : PassInfoMixin<FunctionCallObfuscate> {
  bool enabled;
  FunctionCallObfuscate(bool enabled = false) : enabled(enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
