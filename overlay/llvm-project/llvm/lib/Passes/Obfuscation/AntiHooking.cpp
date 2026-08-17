#include "Obfuscation/AntiHooking.h"
#include "Obfuscation/Utils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include <vector>

using namespace llvm;

// AArch64 instruction signatures used to detect inline hooks.
// A hook inserts a branch/break at the function entry to redirect execution.
#define AARCH64_SIG_B   0x05u  // top 6 bits of an unconditional B  instruction
#define AARCH64_SIG_BRK 0x6A1u // top 11 bits of a BRK (breakpoint) instruction
#define AARCH64_SIG_BR  0x3587C0u // top 22 bits of a BR  Xn instruction

// Reads a 32-bit instruction word from base + byteOffset, returns as i32.
static Value *readInsnWord(IRBuilder<> &IRB, Value *BaseI64, int ByteOff,
                           LLVMContext &Ctx) {
  Type *I8PtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
  Type *I32Ty   = Type::getInt32Ty(Ctx);
  // base + offset → pointer → load 32-bit instruction word
  Value *Ptr = ByteOff == 0
                   ? IRB.CreateIntToPtr(BaseI64, I8PtrTy)
                   : IRB.CreateIntToPtr(
                         IRB.CreateAdd(BaseI64,
                                       ConstantInt::get(
                                           Type::getInt64Ty(Ctx), ByteOff)),
                         I8PtrTy);
  // Bitcast to i32* then load (instruction words are 4 bytes on AArch64)
  Value *I32Ptr = IRB.CreateBitCast(
      Ptr, PointerType::get(I32Ty, 0));
  return IRB.CreateLoad(I32Ty, I32Ptr);
}

// Builds the hook-detection prologue for an AArch64 function.
// Splits the entry block and inserts three detection blocks before the
// original code. Any detected hook calls abort() and then falls through
// (the branch to the original code is unreachable in practice).
static void insertAArch64HookCheck(Function &F) {
  LLVMContext &Ctx = F.getContext();
  Module *M = F.getParent();

  // Declare abort() if not already present.
  FunctionType *AbortTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  FunctionCallee AbortFn = M->getOrInsertFunction("abort", AbortTy);
  cast<Function>(AbortFn.getCallee())->addFnAttr(Attribute::NoReturn);

  Type *I32Ty = Type::getInt32Ty(Ctx);
  Type *I64Ty = Type::getInt64Ty(Ctx);

  // Split the entry block: everything from the first non-PHI/dbg instruction
  // onward moves into 'OrigBB'. We insert detection blocks before it.
  BasicBlock *OrigBB = &F.getEntryBlock();
  BasicBlock *CheckBB =
      OrigBB->splitBasicBlock(OrigBB->getFirstNonPHIOrDbg(), "ah.check");

  // Remove the unconditional branch the split inserted; we'll add our own.
  OrigBB->getTerminator()->eraseFromParent();

  // 'HookBB' — executed when a hook is detected: call abort().
  BasicBlock *HookBB = BasicBlock::Create(Ctx, "ah.hooked", &F, CheckBB);
  {
    IRBuilder<> B(HookBB);
    B.CreateCall(AbortFn);
    B.CreateBr(CheckBB); // unreachable, but satisfies IR well-formedness
  }

  // 'DetectBB' — check instruction 0 (B / BRK) and instruction 1+2 (BR stub).
  BasicBlock *DetectBB =
      BasicBlock::Create(Ctx, "ah.detect", &F, HookBB);

  IRBuilder<> DB(OrigBB); // insert at end of original entry (now empty of user code)

  // Get the function's own address as an integer.
  Value *FnPtr  = ConstantExpr::getPtrToInt(
      cast<Constant>(
          ConstantExpr::getBitCast(&F,
              PointerType::get(Type::getInt8Ty(Ctx), 0))),
      I64Ty);

  // Read instruction word 0.
  IRBuilder<> IRB(DetectBB);
  Value *Insn0 = readInsnWord(IRB, FnPtr, 0, Ctx);

  // Check for B: top 6 bits == AARCH64_SIG_B
  Value *Shift26 = IRB.CreateLShr(Insn0, ConstantInt::get(I32Ty, 26));
  Value *IsB     = IRB.CreateICmpEQ(Shift26,
                       ConstantInt::get(I32Ty, AARCH64_SIG_B));

  // Check for BRK: top 11 bits == AARCH64_SIG_BRK
  Value *Shift21  = IRB.CreateLShr(Insn0, ConstantInt::get(I32Ty, 21));
  Value *IsBrk    = IRB.CreateICmpEQ(Shift21,
                        ConstantInt::get(I32Ty, AARCH64_SIG_BRK));

  Value *IsHook0  = IRB.CreateOr(IsB, IsBrk);

  // 'Detect2BB' — check instructions 1 and 2 for LDR+BR Xn stub.
  BasicBlock *Detect2BB =
      BasicBlock::Create(Ctx, "ah.detect2", &F, HookBB);

  IRB.CreateCondBr(IsHook0, HookBB, Detect2BB);

  IRBuilder<> IRB2(Detect2BB);
  Value *Insn1   = readInsnWord(IRB2, FnPtr, 4, Ctx);
  Value *Insn2   = readInsnWord(IRB2, FnPtr, 8, Ctx);
  Value *Shr10_1 = IRB2.CreateLShr(Insn1, ConstantInt::get(I32Ty, 10));
  Value *Shr10_2 = IRB2.CreateLShr(Insn2, ConstantInt::get(I32Ty, 10));
  Value *IsBR1   = IRB2.CreateICmpEQ(Shr10_1,
                       ConstantInt::get(I32Ty, AARCH64_SIG_BR));
  Value *IsBR2   = IRB2.CreateICmpEQ(Shr10_2,
                       ConstantInt::get(I32Ty, AARCH64_SIG_BR));
  Value *IsHook1 = IRB2.CreateOr(IsBR1, IsBR2);
  IRB2.CreateCondBr(IsHook1, HookBB, CheckBB);

  // Wire the original entry block into the detection chain.
  DB.CreateBr(DetectBB);
}

// Wraps every external callee referenced in F in a private const GlobalVariable
// so fishhook-style runtime symbol rebinding cannot redirect the call.
static void insertAntiRebind(Function &F) {
  Module *M = F.getParent();
  LLVMContext &Ctx = F.getContext();

  std::vector<CallInst *> Calls;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (CallInst *CI = dyn_cast<CallInst>(&I))
        Calls.push_back(CI);

  for (CallInst *CI : Calls) {
    Function *Callee = CI->getCalledFunction();
    if (!Callee || !Callee->isDeclaration() || Callee->isIntrinsic())
      continue;

    // Create a private const global holding the function pointer.
    // The linker cannot rebind a private symbol, so fishhook cannot
    // intercept the call.
    std::string GVName = "__ah_rb_" + Callee->getName().str();
    GlobalVariable *GV = M->getGlobalVariable(GVName);
    if (!GV) {
      Type *FPtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
      Constant *FPtrConst = ConstantExpr::getBitCast(Callee, FPtrTy);
      GV = new GlobalVariable(*M, FPtrTy,
                              /*isConstant=*/true,
                              GlobalValue::PrivateLinkage,
                              FPtrConst, GVName);
      // Prevent the optimizer from stripping the global.
      appendToCompilerUsed(*M, {GV});
    }

    // Load the function pointer from the private global and call through it.
    IRBuilder<> IRB(CI);
    Value *FpLoad = IRB.CreateLoad(
        PointerType::get(Type::getInt8Ty(Ctx), 0), GV);
    CallInst *NewCall = IRB.CreateCall(
        CI->getFunctionType(), FpLoad,
        SmallVector<Value *, 8>(CI->arg_begin(), CI->arg_end()));
    NewCall->setCallingConv(CI->getCallingConv());
    if (!CI->getType()->isVoidTy())
      CI->replaceAllUsesWith(NewCall);
    CI->eraseFromParent();
  }
}

PreservedAnalyses AntiHooking::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;
  Triple T(M.getTargetTriple());
  bool IsAArch64 = T.isAArch64();

  for (Function &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!enabled &&
        getFunctionAnnotation(&F).find("antihook") == std::string::npos)
      continue;

    // Protection 1: AArch64 inline hook detection at function entry.
    if (IsAArch64)
      insertAArch64HookCheck(F);

    // Protection 2: wrap callees in private globals to defeat rebinding.
    insertAntiRebind(F);

    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
