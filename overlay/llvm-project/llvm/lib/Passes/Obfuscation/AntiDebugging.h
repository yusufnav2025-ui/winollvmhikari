#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// AntiDebugging — upgraded from the stub version.
// Injects ptrace(PT_DENY_ATTACH) detection on Android AArch64 via inline
// assembly at the function entry block, plus the original volatile guard
// noise that interferes with dynamic analysis tools.
//
// Usage:
//   global:      -mllvm -antidbg
//   per-function: __attribute__((annotate("antidbg")))
struct AntiDebugging : PassInfoMixin<AntiDebugging> {
  bool enabled;
  AntiDebugging(bool enabled = false) : enabled(enabled) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static bool isRequired() { return true; }
private:
  void process(Function &F);
};
