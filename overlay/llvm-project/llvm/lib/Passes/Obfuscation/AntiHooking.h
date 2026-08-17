#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// AntiHooking — detects inline hooks and prevents symbol rebinding at runtime.
//
// Two independent protections (both on by default when the pass is enabled):
//
//   1. Inline hook detection (AArch64 only):
//      Inserts IR at function entry that reads the first 3 instructions of
//      the function's own machine code and checks for hook signatures:
//        - B  (unconditional branch)   — classic trampoline pattern
//        - BRK (breakpoint)             — debugger injection
//        - LDR + BR Xn                  — Substrate/fishhook stub
//      If detected → calls abort().
//
//   2. Anti-rebind (all targets):
//      Wraps every external callee pointer in a private const GlobalVariable
//      so fishhook-style symbol rebinding at runtime cannot redirect the call.
//
// Usage:
//   global:      -mllvm -antihook
//   per-function: __attribute__((annotate("antihook")))
struct AntiHooking : PassInfoMixin<AntiHooking> {
  bool enabled;
  AntiHooking(bool enabled = false) : enabled(enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
