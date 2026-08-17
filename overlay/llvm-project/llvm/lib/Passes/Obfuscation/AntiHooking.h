#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// AntiHooking — inline-hook detection (AArch64) + anti-rebind (fishhook).
// Ported from Hikari, converted to the new pass manager.
//
// Usage:
//   global:      -mllvm -antihook
//   per-function: __attribute__((annotate("antihook")))
struct AntiHooking : PassInfoMixin<AntiHooking> {
  bool enabled;
  AntiHooking(bool enabled = false) : enabled(enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  void handleInlineHookAArch64(Function *F);
  static bool isRequired() { return true; }
};
