#pragma once
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

// TimeExpiry — embeds the build-time epoch (Unix seconds) and injects a global
// constructor that terminates the process once a configurable number of
// minutes has elapsed since compilation.
//
// The elapsed time is fetched via a raw AArch64 SVC syscall (clock_gettime
// #113) — no libc time()/gettimeofday() that can be hooked. On expiry the
// guard prints a message (toggleable) and terminates the process via SVC kill
// (#129) + trap — never abort().
//
// The guard function is registered in llvm.global.annotations with the
// obfuscation keywords ("fla bcf sub split mba linearmba aliasaccess junkcode
// constenc vmp") so the existing passes automatically obfuscate and VM-protect
// it even when only -expiry is passed.
//
// Usage:
//   global:  -mllvm -expiry=1440      (minutes; 1440 = 1 day; 0 = disabled)
//            -mllvm -expiry-print=0   (suppress the expiry message in release)
struct TimeExpiry : PassInfoMixin<TimeExpiry> {
  unsigned minutes;
  bool printMsg;
  TimeExpiry(unsigned minutes = 0, bool printMsg = true)
      : minutes(minutes), printMsg(printMsg) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};
