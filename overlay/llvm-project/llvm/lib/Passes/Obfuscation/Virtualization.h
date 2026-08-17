#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// VMP (Virtualization) — translates entire functions into a custom bytecode
// format and replaces the function body with a software interpreter loop.
//
// The original logic is never present as native instructions — a reverse
// engineer sees only a VM dispatcher. No decompiler can reconstruct the
// original function from the binary.
//
// Supported IR constructs: alloca, load, store, binary ops, icmp,
// branch, ret, cast, select, gep (simple forms), call.
// PHI nodes are demoted to stack automatically (fixStack) before translation.
// Unsupported: atomics, varargs, invoke, indirect calls, vector/float types.
// Unsupported functions are skipped cleanly.
//
// Usage:
//   global:      -mllvm -vmp
//   per-function: __attribute__((annotate("vmp")))
struct Virtualization : PassInfoMixin<Virtualization> {
  bool enabled;
  Virtualization(bool enabled = false) : enabled(enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
