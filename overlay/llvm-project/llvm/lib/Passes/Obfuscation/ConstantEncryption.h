#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// ConstantEncryption — replaces each ConstantInt operand with a runtime
// XOR decryption sequence so raw integer values never appear in the binary.
//
// Usage:
//   global:      -mllvm -constenc
//   per-function: __attribute__((annotate("constenc")))
struct ConstantEncryption : PassInfoMixin<ConstantEncryption> {
  bool enabled;
  ConstantEncryption(bool enabled = false) : enabled(enabled) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
