#include "Obfuscation/FunctionCallObfuscate.h"
#include "Obfuscation/Utils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include <vector>

using namespace llvm;

// Returns the dlopen flags appropriate for the target triple.
// Android 64-bit: RTLD_NOW (0x2) | Android-specific RTLD_GLOBAL (0x100)
// Android 32-bit / fallback: RTLD_NOW (0x2)
static int32_t getDlopenFlags(const Module &M) {
  Triple T(M.getTargetTriple());
  if (T.isAArch64() || T.getArch() == Triple::x86_64)
    return 0x00002 | 0x100; // 64-bit Android
  return 0x00002;           // 32-bit Android / fallback
}

// Returns true if the callee is a safe target for dlopen/dlsym replacement.
static bool isSafeTarget(const Function *Callee) {
  if (!Callee)                       return false;
  if (!Callee->isDeclaration())      return false; // defined locally — keep direct
  if (Callee->isIntrinsic())         return false; // LLVM intrinsics
  if (Callee->isVarArg())            return false; // varargs can't be dlsym'd
  StringRef Name = Callee->getName();
  if (Name == "dlopen" || Name == "dlsym" || Name == "dlclose")
    return false; // avoid infinite recursion
  if (Name.startswith("llvm."))      return false; // safety belt
  if (Name.startswith("__cxa_"))     return false; // C++ exception ABI
  if (Name.startswith("__gcc_"))     return false; // GCC runtime helpers
  if (Name == "__stack_chk_fail" ||
      Name == "__stack_chk_guard")   return false; // stack protector
  return true;
}

PreservedAnalyses FunctionCallObfuscate::run(Module &M,
                                              ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();
  bool Changed     = false;

  // Use opaque pointer type (LLVM 17 default).
  Type *VoidPtrTy = PointerType::get(Type::getInt8Ty(Ctx), 0);
  Type *Int32Ty   = Type::getInt32Ty(Ctx);

  // Declare dlopen / dlsym once per module (getOrInsertFunction is idempotent).
  FunctionType *DlopenTy =
      FunctionType::get(VoidPtrTy, {VoidPtrTy, Int32Ty}, /*isVarArg=*/false);
  FunctionCallee DlopenFn = M.getOrInsertFunction("dlopen", DlopenTy);

  FunctionType *DlsymTy =
      FunctionType::get(VoidPtrTy, {VoidPtrTy, VoidPtrTy}, /*isVarArg=*/false);
  FunctionCallee DlsymFn = M.getOrInsertFunction("dlsym", DlsymTy);

  int32_t Flags   = getDlopenFlags(M);

  for (Function &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!enabled &&
        getFunctionAnnotation(&F).find("fco") == std::string::npos)
      continue;

    // Collect CallInsts to transform — must not modify while iterating.
    std::vector<CallInst *> ToObfuscate;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        CallInst *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;
        Function *Callee = CI->getCalledFunction();
        if (!isSafeTarget(Callee)) continue;
        ToObfuscate.push_back(CI);
      }
    }

    for (CallInst *CI : ToObfuscate) {
      Function   *Callee = CI->getCalledFunction();
      FunctionType *FTy  = Callee->getFunctionType();
      IRBuilder<> IRB(CI); // inserts before CI

      // 1. dlopen(NULL, flags) — open the current process image.
      Value *NullPtr = ConstantPointerNull::get(
          PointerType::get(Type::getInt8Ty(Ctx), 0));
      Value *FlagVal = ConstantInt::get(Int32Ty, Flags);
      Value *Handle  = IRB.CreateCall(DlopenFn, {NullPtr, FlagVal},
                                      "fco.handle");

      // 2. dlsym(handle, "func_name") — resolve the symbol at runtime.
      Value *SymName = IRB.CreateGlobalStringPtr(Callee->getName(),
                                                  "fco.sym");
      Value *FpVoid  = IRB.CreateCall(DlsymFn, {Handle, SymName}, "fco.fp");

      // 3. Call through the resolved opaque pointer using the original
      //    FunctionType — no bitcast needed with LLVM 17 opaque pointers.
      SmallVector<Value *, 8> Args(CI->arg_begin(), CI->arg_end());
      CallInst *NewCall = IRB.CreateCall(FTy, FpVoid, Args);

      // Preserve calling convention and attributes of the original call.
      NewCall->setCallingConv(CI->getCallingConv());

      if (!CI->getType()->isVoidTy())
        CI->replaceAllUsesWith(NewCall);

      CI->eraseFromParent();
      Changed = true;
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
