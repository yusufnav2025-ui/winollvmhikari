// AntiHooking pass — inline-hook detection (AArch64) + anti-rebind.
// Ported from Hikari's AntiHooking.cpp, converted to the new pass manager
// and stripped of the precompiled-`.bc` handler mechanism (the handler is
// now inlined as abort()/AHCallBack()).
#include "Obfuscation/AntiHooking.h"
#include "Obfuscation/Utils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include <vector>

using namespace llvm;

// Arm A64 instruction-set signatures used to detect a trampolined prologue.
#define AARCH64_SIGNATURE_B   0b0000101                  // B (uncond. branch)
#define AARCH64_SIGNATURE_BR  0b11010110000111110000000 // BR/ret
#define AARCH64_SIGNATURE_BRK 0b11010100001             // BRK

// Replace the original prologue with a check that the first instructions have
// not been overwritten with a branch (inline hook). On detection, jump to a
// handler (AHCallBack() if present, else abort()).
void AntiHooking::handleInlineHookAArch64(Function *F) {
  LLVMContext &Ctx = F->getContext();
  Module *M = F->getParent();

  BasicBlock *A = &F->getEntryBlock();
  BasicBlock *C = A->splitBasicBlock(A->getFirstNonPHIOrDbgOrLifetime());
  BasicBlock *B = BasicBlock::Create(Ctx, "HookDetectedHandler", F);
  BasicBlock *Detect = BasicBlock::Create(Ctx, "", F);
  BasicBlock *Detect2 = BasicBlock::Create(Ctx, "", F);

  A->getTerminator()->eraseFromParent();
  BranchInst::Create(Detect, A);

  Type *I32Ty = Type::getInt32Ty(Ctx);
  Type *I64Ty = Type::getInt64Ty(Ctx);

  // Detect: read the first instruction word of F.
  IRBuilder<> IRBD(Detect);
  Value *Load = IRBD.CreateLoad(I32Ty, F);
  Value *LS2 = IRBD.CreateLShr(Load, IRBD.getInt32(26));
  Value *ICmpB = IRBD.CreateICmpEQ(LS2, IRBD.getInt32(AARCH64_SIGNATURE_B));
  Value *LS3 = IRBD.CreateLShr(Load, IRBD.getInt32(21));
  Value *ICmpBRK = IRBD.CreateICmpEQ(LS3, IRBD.getInt32(AARCH64_SIGNATURE_BRK));
  IRBD.CreateCondBr(IRBD.CreateOr(ICmpB, ICmpBRK), B, Detect2);

  // Detect2: check instruction words at F+4 and F+8 for BR (branch to reg).
  IRBuilder<> IRBD2(Detect2);
  Value *PTI = IRBD2.CreatePtrToInt(F, I64Ty);
  Value *Add4 = IRBD2.CreateAdd(PTI, IRBD2.getInt64(4));
  Value *Load2 = IRBD2.CreateLoad(I32Ty, IRBD2.CreateIntToPtr(Add4, PointerType::get(Ctx, 0)));
  Value *LS4 = IRBD2.CreateLShr(Load2, IRBD2.getInt32(10));
  Value *ICmpBR1 = IRBD2.CreateICmpEQ(LS4, IRBD2.getInt32(AARCH64_SIGNATURE_BR));

  Value *Add8 = IRBD2.CreateAdd(PTI, IRBD2.getInt64(8));
  Value *Load3 = IRBD2.CreateLoad(I32Ty, IRBD2.CreateIntToPtr(Add8, PointerType::get(Ctx, 0)));
  Value *LS5 = IRBD2.CreateLShr(Load3, IRBD2.getInt32(10));
  Value *ICmpBR2 = IRBD2.CreateICmpEQ(LS5, IRBD2.getInt32(AARCH64_SIGNATURE_BR));

  IRBD2.CreateCondBr(IRBD2.CreateOr(ICmpBR1, ICmpBR2), B, C);

  // B: call the hook handler, then continue into the real body C.
  IRBuilder<> IRBB(B);
  Function *AHCallBack = M->getFunction("AHCallBack");
  if (AHCallBack) {
    IRBB.CreateCall(AHCallBack);
  } else {
    Function *abortFn = cast<Function>(
        M->getOrInsertFunction("abort", FunctionType::get(IRBB.getVoidTy(), false))
            .getCallee());
    abortFn->addFnAttr(Attribute::NoReturn);
    IRBB.CreateCall(abortFn);
  }
  IRBB.CreateBr(C);
}

PreservedAnalyses AntiHooking::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;
  bool IsAArch64 = Triple(M.getTargetTriple()).isAArch64();

  for (Function &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!enabled &&
        getFunctionAnnotation(&F).find("antihook") == std::string::npos)
      continue;

    if (IsAArch64) {
      handleInlineHookAArch64(&F);
      Changed = true;
    }

    // Anti-rebind: route calls to external declarations through a private
    // constant global holding the function pointer, so fishhook can't rebind.
    std::vector<CallBase *> ToRedirect;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          Function *Called = dyn_cast<Function>(CB->getCalledOperand()->stripPointerCasts());
          if (!Called || !Called->isDeclaration() ||
              !Called->hasExternalLinkage() || Called->isIntrinsic())
            continue;
          StringRef Name = Called->getName();
          if (Name.starts_with("clang.") || Name.starts_with("llvm.") ||
              Name == "dlopen" || Name == "dlsym" || Name == "dlclose")
            continue;
          ToRedirect.push_back(CB);
        }
      }
    }

    for (CallBase *CB : ToRedirect) {
      Function *Called = cast<Function>(CB->getCalledOperand()->stripPointerCasts());
      std::string SymbolName = (Twine("AntiRebindSymbol_") + Called->getName()).str();
      GlobalVariable *GV = M.getGlobalVariable(StringRef(SymbolName));
      if (!GV) {
        GV = new GlobalVariable(M, Called->getType(), false,
                                GlobalValue::PrivateLinkage, Called,
                                SymbolName);
        GV->setConstant(true);
        appendToCompilerUsed(M, {GV});
      }
      LoadInst *Load = new LoadInst(GV->getValueType(), GV, Called->getName(), CB);
      CB->setCalledOperand(Load);
      Changed = true;
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
