#include "Obfuscation/TimeExpiry.h"
#include "Obfuscation/Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Alignment.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

using namespace llvm;

// Marker global name — prevents double-injection if the pass runs twice on the
// same module.
static const char *kMarker = "llvm.time_expiry.applied";

// Shuffle a list of assembly setup lines deterministically from a seed and
// append a randomized `svc #imm`. Randomizes instruction order + SVC immediate
// to defeat static signature matching (same technique as AntiDebugging).
static std::string shuffledSvcAsm(uint64_t Seed,
                                  const std::vector<std::string> &Setup) {
  std::vector<std::string> L = Setup;
  uint64_t S = Seed;
  for (size_t i = L.size() - 1; i > 0; i--) {
    S = S * 6364136223846793005ULL + 1442695040888963407ULL;
    size_t j = S % (i + 1);
    std::swap(L[i], L[j]);
  }
  std::string Asm;
  for (const std::string &Line : L)
    Asm += Line;
  uint32_t SvcImm = (uint32_t)((Seed >> 32) & 0xFFFF);
  Asm += "svc #" + std::to_string(SvcImm) + "\n";
  return Asm;
}

// Build a raw clock_gettime (syscall 113) helper that fills a 16-byte timespec
// buffer and returns tv_sec. Raw svc call — no libc, so it cannot be hooked.
static Function *createNowFn(Module &M, uint64_t Seed) {
  LLVMContext &Ctx = M.getContext();
  Type *I8Ty = Type::getInt8Ty(Ctx);
  Type *I64Ty = Type::getInt64Ty(Ctx);

  FunctionType *FTy = FunctionType::get(I64Ty, /*isVarArg=*/false);
  Function *F = Function::Create(
      FTy, GlobalValue::InternalLinkage,
      "obfu_now_" + std::to_string((unsigned)(Seed & 0xFFFF)), &M);
  F->addFnAttr(Attribute::NoUnwind);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(Entry);

  AllocaInst *TS = IRB.CreateAlloca(ArrayType::get(I8Ty, 16), nullptr, "ts");
  TS->setAlignment(Align(8));

  Value *BufI64 = IRB.CreatePtrToInt(TS, I64Ty);

  std::string Asm = shuffledSvcAsm(Seed,
                                   {"mov x0, #0\n",   // CLOCK_REALTIME
                                    "mov x1, %0\n",   // &timespec
                                    "mov x8, #113\n"} // __NR_clock_gettime
  );

  FunctionType *AsmFTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  InlineAsm *IA =
      InlineAsm::get(AsmFTy, Asm, "r,~{x0},~{x1},~{x8},~{memory}",
                     /*hasSideEffects=*/true, /*isAlignStack=*/false);
  IRB.CreateCall(IA, {BufI64});

  // Load tv_sec (first 8 bytes) directly from the opaque pointer.
  Value *Sec = IRB.CreateLoad(I64Ty, TS);
  IRB.CreateRet(Sec);
  return F;
}

// Build the crash path: optional printf message + SVC kill(-1, SIGKILL) + trap.
// Marked noreturn. Not annotated (contains inline asm + vararg printf), so it
// is never VM-protected — canVirtualize rejects it, which is intentional.
static Function *createFailFn(Module &M, uint64_t Seed, bool printMsg) {
  LLVMContext &Ctx = M.getContext();
  Type *I8Ty = Type::getInt8Ty(Ctx);
  Type *I32Ty = Type::getInt32Ty(Ctx);
  Type *I64Ty = Type::getInt64Ty(Ctx);
  PointerType *I8PtrTy = PointerType::get(I8Ty, 0);

  FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F = Function::Create(
      FTy, GlobalValue::InternalLinkage,
      "obfu_fail_" + std::to_string((unsigned)(Seed & 0xFFFF)), &M);
  F->addFnAttr(Attribute::NoReturn);
  F->addFnAttr(Attribute::NoUnwind);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> IRB(Entry);

  if (printMsg) {
    Constant *Str =
        IRB.CreateGlobalStringPtr("[protected] license expired", "expiry_msg");
    FunctionCallee Printf = M.getOrInsertFunction(
        "printf", FunctionType::get(I32Ty, {I8PtrTy}, /*isVarArg=*/true));
    IRB.CreateCall(Printf, {Str});
  }

  std::string Asm = shuffledSvcAsm(Seed,
                                   {"mov x0, %0\n",   // pid = -1
                                    "mov x1, %1\n",   // sig = SIGKILL (9)
                                    "mov x8, #129\n"} // __NR_kill
  );

  FunctionType *AsmFTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  InlineAsm *IA = InlineAsm::get(AsmFTy, Asm, "r,r,~{x0},~{x1},~{x8},~{memory}",
                                 /*hasSideEffects=*/true,
                                 /*isAlignStack=*/false);
  IRB.CreateCall(IA, {IRB.getInt64((uint64_t)-1), IRB.getInt64(9)});

  IRB.CreateIntrinsic(Intrinsic::trap, {}, {});
  IRB.CreateUnreachable();
  return F;
}

// Build the expiry guard: fetch the current time, compare against the baked-in
// deadline (with a plausibility lower bound), and crash on expiry. Contains no
// inline asm / vararg calls, so VMP can virtualize it.
static Function *createGuardFn(Module &M, uint64_t Seed, Function *Now,
                               Function *Fail, uint64_t BuildEpoch,
                               uint64_t Deadline) {
  LLVMContext &Ctx = M.getContext();
  Type *I64Ty = Type::getInt64Ty(Ctx);

  FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *F = Function::Create(
      FTy, GlobalValue::InternalLinkage,
      "obfu_guard_" + std::to_string((unsigned)(Seed & 0xFFFF)), &M);
  F->addFnAttr(Attribute::NoUnwind);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *FailBB = BasicBlock::Create(Ctx, "fail", F);
  BasicBlock *OkBB = BasicBlock::Create(Ctx, "ok", F);

  IRBuilder<> IRB(Entry);
  Value *NowVal = IRB.CreateCall(Now);
  // Plausibility guard: a clock before the build epoch means the time source
  // was hooked/spoofed — treat as expired.
  Value *Die1 = IRB.CreateICmpULT(NowVal, IRB.getInt64(BuildEpoch));
  Value *Die2 = IRB.CreateICmpUGT(NowVal, IRB.getInt64(Deadline));
  Value *Die = IRB.CreateOr(Die1, Die2);
  IRB.CreateCondBr(Die, FailBB, OkBB);

  IRB.SetInsertPoint(FailBB);
  IRB.CreateCall(Fail);
  IRB.CreateRetVoid(); // unreachable at runtime (Fail is noreturn)

  IRB.SetInsertPoint(OkBB);
  IRB.CreateRetVoid();

  return F;
}

// Attach an entry to @llvm.global.annotations so the existing obfuscation
// passes (which read annotations via getFunctionAnnotation()) auto-protect F.
static void annotateFunction(Module &M, Function *F) {
  LLVMContext &Ctx = M.getContext();
  Type *I8Ty = Type::getInt8Ty(Ctx);
  Type *I32Ty = Type::getInt32Ty(Ctx);
  PointerType *I8PtrTy = PointerType::get(I8Ty, 0);

  Constant *AnnoStr = ConstantDataArray::getString(
      Ctx, "fla bcf sub split mba linearmba aliasaccess junkcode constenc vmp");
  GlobalVariable *AnnoGV = new GlobalVariable(
      M, AnnoStr->getType(), /*isConstant=*/true, GlobalValue::PrivateLinkage,
      AnnoStr, ".anno");
  Constant *FileStr = ConstantDataArray::getString(Ctx, "expiry");
  GlobalVariable *FileGV = new GlobalVariable(
      M, FileStr->getType(), /*isConstant=*/true, GlobalValue::PrivateLinkage,
      FileStr, ".anno.file");

  StructType *STy = StructType::get(I8PtrTy, I8PtrTy, I8PtrTy, I32Ty);
  Constant *Entry =
      ConstantStruct::get(STy, {F, AnnoGV, FileGV, ConstantInt::get(I32Ty, 0)});

  if (GlobalVariable *GA = M.getGlobalVariable("llvm.global.annotations")) {
    ConstantArray *CA = cast<ConstantArray>(GA->getInitializer());
    SmallVector<Constant *, 8> Elts;
    for (unsigned i = 0; i < CA->getNumOperands(); i++)
      Elts.push_back(cast<Constant>(CA->getOperand(i)));
    Elts.push_back(Entry);
    GA->setInitializer(
        ConstantArray::get(ArrayType::get(STy, Elts.size()), Elts));
  } else {
    Constant *Arr = ConstantArray::get(ArrayType::get(STy, 1), {Entry});
    new GlobalVariable(M, Arr->getType(), /*isConstant=*/false,
                       GlobalValue::AppendingLinkage, Arr,
                       "llvm.global.annotations");
  }
}

// Register the guard as a global constructor (priority 65535), appending to any
// existing @llvm.global_ctors so user constructors are preserved.
static void registerConstructor(Module &M, Function *Ctor) {
  LLVMContext &Ctx = M.getContext();
  Type *I8Ty = Type::getInt8Ty(Ctx);
  Type *I32Ty = Type::getInt32Ty(Ctx);
  PointerType *I8PtrTy = PointerType::get(I8Ty, 0);

  StructType *CTy = StructType::get(I32Ty, I8PtrTy, I8PtrTy);
  Constant *Entry = ConstantStruct::get(
      CTy, {ConstantInt::get(I32Ty, 65535), Ctor,
            ConstantPointerNull::get(I8PtrTy)});

  if (GlobalVariable *GC = M.getGlobalVariable("llvm.global_ctors")) {
    ConstantArray *CA = cast<ConstantArray>(GC->getInitializer());
    SmallVector<Constant *, 8> Elts;
    for (unsigned i = 0; i < CA->getNumOperands(); i++)
      Elts.push_back(cast<Constant>(CA->getOperand(i)));
    Elts.push_back(Entry);
    GC->setInitializer(
        ConstantArray::get(ArrayType::get(CTy, Elts.size()), Elts));
  } else {
    Constant *Arr = ConstantArray::get(ArrayType::get(CTy, 1), {Entry});
    new GlobalVariable(M, Arr->getType(), /*isConstant=*/false,
                       GlobalValue::AppendingLinkage, Arr, "llvm.global_ctors");
  }
}

PreservedAnalyses TimeExpiry::run(Module &M, ModuleAnalysisManager &AM) {
  if (minutes == 0)
    return PreservedAnalyses::all();

  Triple T(M.getTargetTriple());
  if (!T.isAArch64())
    return PreservedAnalyses::all();

  if (M.getNamedGlobal(kMarker))
    return PreservedAnalyses::all();

  uint64_t BuildEpoch = (uint64_t)std::time(nullptr);
  uint64_t Deadline = BuildEpoch + (uint64_t)minutes * 60ULL;

  uint64_t Seed = BuildEpoch ^ 0x9E3779B97F4A7C15ULL;

  Function *Now = createNowFn(M, Seed);
  Function *Fail = createFailFn(M, Seed, printMsg);
  Function *Guard = createGuardFn(M, Seed, Now, Fail, BuildEpoch, Deadline);

  annotateFunction(M, Guard);
  registerConstructor(M, Guard);

  new GlobalVariable(M, Type::getInt8Ty(M.getContext()), /*isConstant=*/true,
                     GlobalValue::PrivateLinkage,
                     ConstantInt::get(Type::getInt8Ty(M.getContext()), 1),
                     kMarker);

  return PreservedAnalyses::none();
}
