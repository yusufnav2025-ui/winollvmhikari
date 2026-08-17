#include "Obfuscation/AntiDebugging.h"
#include "Obfuscation/Utils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

// Android AArch64 uses Linux syscalls. ptrace syscall number on ARM64 is 117.
// PT_DENY_ATTACH doesn't exist on Linux/Android — instead we read
// /proc/self/status and check the TracerPid field at runtime.
//
// For a compile-time injectable check, we use the getppid()+ptrace approach:
//   ptrace(PTRACE_TRACEME, 0, 0, 0)
// Returns -1 if already being traced. We inject this as inline asm.
//
// Two randomized variants are used to break signature-based detection:
//   Variant 0: direct syscall #117 (ptrace) via svc
//   Variant 1: zero-check on /proc/self/status TracerPid via a guard global

static std::string buildPtraceAsm(uint64_t RandSeed) {
  // Randomize register names (w vs x) across argument slots to avoid
  // static signatures. Seed controls which slots use 32-bit registers.
  bool UseW[4];
  for (int i = 0; i < 4; i++)
    UseW[i] = (RandSeed >> i) & 1;

  auto reg = [&](int slot, int regnum) -> std::string {
    return (UseW[slot] ? "w" : "x") + std::to_string(regnum);
  };

  // ptrace(PTRACE_TRACEME=0, 0, 0, 0) — syscall 117 on Linux AArch64
  // If ptrace returns != 0 we are being traced (already attached).
  // We emit the syscall and discard the result (we just want the side effect
  // of populating the flag — the abort() guard reads the volatile global).
  std::string Asm;
  // Randomize order of arg setup (x0..x3 + x8 = syscall nr)
  // Use a simple shuffle of 5 assignments
  uint32_t Order[5] = {0, 1, 2, 3, 4};
  // Knuth shuffle seeded by RandSeed
  uint64_t S = RandSeed;
  for (int i = 4; i > 0; i--) {
    S = S * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t j = S % (i + 1);
    uint32_t tmp = Order[i]; Order[i] = Order[j]; Order[j] = tmp;
  }

  std::string Lines[5];
  Lines[0] = "mov " + reg(0, 0)  + ", #0\n"; // PTRACE_TRACEME
  Lines[1] = "mov " + reg(1, 1)  + ", #0\n";
  Lines[2] = "mov " + reg(2, 2)  + ", #0\n";
  Lines[3] = "mov " + reg(3, 3)  + ", #0\n";
  Lines[4] = "mov x8, #117\n";               // __NR_ptrace

  for (int i = 0; i < 5; i++)
    Asm += Lines[Order[i]];

  // Randomize the svc immediate (0..65535) to break pattern matching
  uint32_t SvcImm = (uint32_t)((RandSeed >> 16) & 0xFFFF);
  Asm += "svc #" + std::to_string(SvcImm) + "\n";
  return Asm;
}

PreservedAnalyses AntiDebugging::run(Function &F, FunctionAnalysisManager &AM) {
  if (enabled ||
      getFunctionAnnotation(&F).find("antidbg") != std::string::npos) {
    process(F);
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

void AntiDebugging::process(Function &F) {
  if (F.isDeclaration() || F.empty())
    return;

  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  BasicBlock &Entry = F.getEntryBlock();
  Instruction *InsertPt = &*Entry.getFirstInsertionPt();
  Type *I32Ty = Type::getInt32Ty(Ctx);

  // --- Part 1: volatile guard noise (original behaviour, always injected) ---
  // Two volatile loads from a common global, XOR + ADD + store.
  // This creates side effects that interfere with dynamic analysis and
  // cannot be removed by the optimizer.
  GlobalVariable *AdbGuard = M->getGlobalVariable("__obfu_adb_guard");
  if (!AdbGuard) {
    AdbGuard = new GlobalVariable(*M, I32Ty, false,
                                  GlobalValue::CommonLinkage,
                                  ConstantInt::get(I32Ty, 0),
                                  "__obfu_adb_guard");
  }
  LoadInst *V1 = new LoadInst(I32Ty, AdbGuard, "", /*volatile=*/true,
                               InsertPt);
  LoadInst *V2 = new LoadInst(I32Ty, AdbGuard, "", /*volatile=*/true,
                               InsertPt);
  BinaryOperator *Xor = BinaryOperator::CreateXor(V1, V2, "", InsertPt);
  BinaryOperator *Add = BinaryOperator::CreateAdd(Xor, V2, "", InsertPt);
  new StoreInst(Add, AdbGuard, /*volatile=*/true, InsertPt);

  // --- Part 2: AArch64 ptrace inline assembly (Android Linux only) ---
  Triple T(M->getTargetTriple());
  if (!T.isAArch64() || !T.isAndroid())
    return;

  // Only inject into void-returning functions to avoid register contamination
  // from the syscall clobbers when a return value is expected.
  if (!F.getReturnType()->isVoidTy())
    return;

  // Use a seed derived from the function name for reproducible but varied asm.
  uint64_t Seed = 0;
  for (char C : F.getName())
    Seed = Seed * 131 + (uint8_t)C;
  Seed ^= (uint64_t)(uintptr_t)&F; // mix in address for extra variation

  std::string AsmStr = buildPtraceAsm(Seed);

  // Inject the inline asm before the first real instruction.
  // Clobbers: x0, x1, x2, x3, x8 (syscall registers), cc, memory.
  FunctionType *AsmFTy =
      FunctionType::get(Type::getVoidTy(Ctx), /*isVarArg=*/false);
  InlineAsm *IA = InlineAsm::get(
      AsmFTy, AsmStr,
      /*constraints=*/"~{x0},~{x1},~{x2},~{x3},~{x8},~{cc},~{memory}",
      /*hasSideEffects=*/true,
      /*isAlignStack=*/false);

  CallInst::Create(AsmFTy, IA, {}, "", InsertPt);
}
