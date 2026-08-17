// VMP (Virtualization) Pass — LLVM 17, new pass manager
// Translates IR functions into custom bytecode + software interpreter.
// Ported from Hikari-fix (PPKunOfficial), adapted for LLVM 17 new PM,
// Android NDK targets, and stripped of Darwin/ObjC dependencies.
//
// Architecture:
//   translateFunction()  → encodes IR into VMPContext.Code (byte buffer)
//   encryptBytecode()    → dual-layer xorshift32 XOR per basic block
//   buildInterpreter()   → replaces function body with dispatch loop
//   __vmp_ld / __vmp_st  → module-level dynamic-width load/store helpers

#include "Obfuscation/Virtualization.h"
#include "Obfuscation/Utils.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include <cassert>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace llvm;

// ─────────────────────────────────────────────────────────────────────────────
// Bytecode opcode constants (uint8_t)
// ─────────────────────────────────────────────────────────────────────────────
enum VmOp : uint8_t {
  OP_NOP         = 0,
  OP_ALLOCA      = 1,
  OP_LOAD        = 2,
  OP_STORE       = 3,
  OP_BINOP       = 4,
  OP_ICMP        = 5,
  OP_BR          = 6,
  OP_RET         = 7,
  OP_GEP         = 8,
  OP_CALL        = 9,
  OP_CAST        = 10,
  OP_SELECT      = 11,
  OP_UNREACHABLE = 12,
};

enum VmBinOp : uint8_t {
  BIN_ADD=0, BIN_SUB=1, BIN_MUL=2, BIN_UDIV=3, BIN_SDIV=4,
  BIN_UREM=5, BIN_SREM=6, BIN_AND=7,  BIN_OR=8,  BIN_XOR=9,
  BIN_SHL=10, BIN_LSHR=11, BIN_ASHR=12
};
enum VmCmp : uint8_t {
  CMP_EQ=0,CMP_NE=1,CMP_UGT=2,CMP_UGE=3,CMP_ULT=4,
  CMP_ULE=5,CMP_SGT=6,CMP_SGE=7,CMP_SLT=8,CMP_SLE=9
};
enum VmCast : uint8_t {
  CAST_TRUNC=0, CAST_ZEXT=1, CAST_SEXT=2,
  CAST_BITCAST=3, CAST_PTRTOINT=4, CAST_INTTOPTR=5
};

static constexpr unsigned kPtrSize = 8; // all targets are 64-bit Android

struct BBSeedInfo {
  uint32_t EntryIP = 0, EndIP = 0;
  uint32_t OpcodeSeed = 0, CodeSeed = 0;
  std::vector<uint32_t> OpcodeOffsets; // code[] offsets that hold opcodes
};

struct CallArgInfo {
  bool     IsConst  = false;
  uint64_t ConstVal = 0;
  uint32_t Off      = 0;   // data-area slot
  uint32_t Size     = 8;   // bytes
  Type    *ArgTy    = nullptr;
};
struct CallSiteInfo {
  uint32_t             Id           = 0;
  Function            *DirectCallee = nullptr;
  uint32_t             RetOff       = 0;
  uint32_t             RetSize      = 0;
  bool                 HasRet       = false;
  FunctionType        *FTy          = nullptr;
  std::vector<CallArgInfo> Args;
};

struct VMPContext {
  std::vector<uint8_t>          Code;
  std::map<Value *, uint32_t>   ValueMap;    // Value → data-area byte offset
  std::map<BasicBlock *, uint32_t> BBMap;    // BB → code IP
  std::vector<std::pair<uint32_t, BasicBlock *>> BrPatches;
  std::vector<CallSiteInfo>     Calls;
  std::vector<std::pair<GlobalValue *, uint32_t>> Globals; // GV → data slot
  uint32_t DataSize = 0;
  std::vector<BBSeedInfo> BBInfos;
  bool Failed = false;
  std::string FailReason;

  uint32_t allocSlot(uint32_t bytes) {
    uint32_t off = DataSize;
    DataSize += bytes;
    return off;
  }
  uint32_t getGlobalSlot(GlobalValue *GV) {
    for (auto &G : Globals)
      if (G.first == GV) return G.second;
    uint32_t slot = allocSlot(kPtrSize);
    Globals.push_back({GV, slot});
    return slot;
  }
  void emitU8(uint8_t v)  { Code.push_back(v); }
  void emitU64(uint64_t v) {
    for (int i = 0; i < 8; i++) Code.push_back((v >> (i*8)) & 0xFF);
  }
  uint32_t ip() const { return (uint32_t)Code.size(); }
};

static uint32_t typeSize(Type *T) {
  if (T->isVoidTy())    return 0;
  if (T->isPointerTy()) return kPtrSize;
  if (T->isIntegerTy()) return (T->getIntegerBitWidth() + 7) / 8;
  return 0; // unsupported
}

static bool isTypeOK(Type *T) {
  if (T->isVoidTy() || T->isPointerTy()) return true;
  if (T->isIntegerTy()) {
    unsigned W = T->getIntegerBitWidth();
    return W==1||W==8||W==16||W==32||W==64;
  }
  return false;
}

static uint8_t mapBin(unsigned Op) {
  switch(Op) {
  case Instruction::Add:  return BIN_ADD;  case Instruction::Sub: return BIN_SUB;
  case Instruction::Mul:  return BIN_MUL;  case Instruction::UDiv:return BIN_UDIV;
  case Instruction::SDiv: return BIN_SDIV; case Instruction::URem:return BIN_UREM;
  case Instruction::SRem: return BIN_SREM; case Instruction::And: return BIN_AND;
  case Instruction::Or:   return BIN_OR;   case Instruction::Xor: return BIN_XOR;
  case Instruction::Shl:  return BIN_SHL;  case Instruction::LShr:return BIN_LSHR;
  case Instruction::AShr: return BIN_ASHR;
  default: return 0xFF;
  }
}
static uint8_t mapCmp(CmpInst::Predicate P) {
  switch(P) {
  case CmpInst::ICMP_EQ:  return CMP_EQ;  case CmpInst::ICMP_NE:  return CMP_NE;
  case CmpInst::ICMP_UGT: return CMP_UGT; case CmpInst::ICMP_UGE: return CMP_UGE;
  case CmpInst::ICMP_ULT: return CMP_ULT; case CmpInst::ICMP_ULE: return CMP_ULE;
  case CmpInst::ICMP_SGT: return CMP_SGT; case CmpInst::ICMP_SGE: return CMP_SGE;
  case CmpInst::ICMP_SLT: return CMP_SLT; case CmpInst::ICMP_SLE: return CMP_SLE;
  default: return 0xFF;
  }
}
static uint8_t mapCast(unsigned Op) {
  switch(Op) {
  case Instruction::Trunc:    return CAST_TRUNC;
  case Instruction::ZExt:     return CAST_ZEXT;
  case Instruction::SExt:     return CAST_SEXT;
  case Instruction::BitCast:  return CAST_BITCAST;
  case Instruction::PtrToInt: return CAST_PTRTOINT;
  case Instruction::IntToPtr: return CAST_INTTOPTR;
  default: return 0xFF;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Operand encoding:
//   [size:u8][type:u8=0(var)/1(const)][8 bytes = slot-offset or const-value]
// ─────────────────────────────────────────────────────────────────────────────
static void emitVar(VMPContext &C, uint32_t slot, uint32_t sz) {
  C.emitU8((uint8_t)sz);
  C.emitU8(0); // variable
  C.emitU64(slot);
}
static void emitConst(VMPContext &C, uint64_t val, uint32_t sz) {
  C.emitU8((uint8_t)sz);
  C.emitU8(1); // constant
  C.emitU64(val);
}

static void emitOperand(VMPContext &C, Value *V) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
    uint32_t sz = typeSize(CI->getType());
    uint64_t val = sz <= 8 ? CI->getZExtValue() : 0;
    emitConst(C, val, sz ? sz : 1);
    return;
  }
  if (isa<ConstantPointerNull>(V)) {
    emitConst(C, 0, kPtrSize);
    return;
  }
  if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    emitVar(C, C.getGlobalSlot(GV), kPtrSize);
    return;
  }
  auto it = C.ValueMap.find(V);
  if (it != C.ValueMap.end()) {
    uint32_t sz = typeSize(V->getType()); if (!sz) sz = 1;
    emitVar(C, it->second, sz);
    return;
  }
  uint32_t sz = typeSize(V->getType()); if (!sz) sz = 1;
  emitConst(C, 0, sz);
}

static uint32_t xorshift32(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// ─────────────────────────────────────────────────────────────────────────────
// Translate one instruction into bytecode
// ─────────────────────────────────────────────────────────────────────────────
static void translateInst(VMPContext &C, Instruction &I) {
  uint32_t opcodeOff = C.ip();

  if (isa<PHINode>(I)) {
    C.Failed = true; C.FailReason = "phi node (fixStack should eliminate)"; return;
  }
  if (isa<IntrinsicInst>(I)) {
    if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      Intrinsic::ID id = II->getIntrinsicID();
      if (id == Intrinsic::lifetime_start || id == Intrinsic::lifetime_end ||
          id == Intrinsic::dbg_declare    || id == Intrinsic::dbg_value)
        return;
    }
    C.Failed = true; C.FailReason = "intrinsic"; return;
  }

  if (auto *AI = dyn_cast<AllocaInst>(&I)) {
    if (!isTypeOK(AI->getAllocatedType())) {
      C.Failed = true; C.FailReason = "alloca unsupported type"; return;
    }
    uint32_t sz = typeSize(AI->getAllocatedType());
    if (sz == 0) sz = kPtrSize;
    uint32_t slot = C.allocSlot(sz);
    uint32_t ptrSlot = C.allocSlot(kPtrSize);
    C.ValueMap[AI] = ptrSlot;
    C.emitU8(OP_ALLOCA);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU64(slot);
    C.emitU64(sz);
    C.emitU64(ptrSlot);
    return;
  }
  if (auto *LI = dyn_cast<LoadInst>(&I)) {
    if (!isTypeOK(LI->getType())) {
      C.Failed = true; C.FailReason = "load unsupported type"; return;
    }
    uint32_t sz = typeSize(LI->getType()); if (!sz) sz = kPtrSize;
    uint32_t dst = C.allocSlot(sz);
    C.ValueMap[LI] = dst;
    C.emitU8(OP_LOAD);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU8((uint8_t)sz);
    C.emitU64(dst);
    emitOperand(C, LI->getPointerOperand());
    return;
  }
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    C.emitU8(OP_STORE);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    emitOperand(C, SI->getValueOperand());
    emitOperand(C, SI->getPointerOperand());
    return;
  }
  if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
    if (!isTypeOK(BO->getType())) {
      C.Failed = true; C.FailReason = "binop unsupported type"; return;
    }
    uint8_t sub = mapBin(BO->getOpcode());
    if (sub == 0xFF) { C.Failed=true; C.FailReason="binop"; return; }
    uint32_t sz = typeSize(BO->getType()); if (!sz) sz=8;
    uint32_t dst = C.allocSlot(sz);
    C.ValueMap[BO] = dst;
    C.emitU8(OP_BINOP);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU8(sub);
    C.emitU8((uint8_t)sz);
    C.emitU64(dst);
    emitOperand(C, BO->getOperand(0));
    emitOperand(C, BO->getOperand(1));
    return;
  }
  if (auto *CI = dyn_cast<ICmpInst>(&I)) {
    uint8_t pred = mapCmp(CI->getPredicate());
    if (pred == 0xFF) { C.Failed=true; C.FailReason="icmp pred"; return; }
    uint32_t dst = C.allocSlot(1);
    C.ValueMap[CI] = dst;
    C.emitU8(OP_ICMP);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU8(pred);
    C.emitU64(dst);
    emitOperand(C, CI->getOperand(0));
    emitOperand(C, CI->getOperand(1));
    return;
  }
  if (auto *BI = dyn_cast<BranchInst>(&I)) {
    C.emitU8(OP_BR);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    if (BI->isUnconditional()) {
      C.emitU8(0);
      uint32_t patchOff = C.ip();
      C.emitU64(0);
      C.BrPatches.push_back({patchOff, BI->getSuccessor(0)});
    } else {
      C.emitU8(1);
      emitOperand(C, BI->getCondition());
      uint32_t p0 = C.ip(); C.emitU64(0);
      uint32_t p1 = C.ip(); C.emitU64(0);
      C.BrPatches.push_back({p0, BI->getSuccessor(0)});
      C.BrPatches.push_back({p1, BI->getSuccessor(1)});
    }
    return;
  }
  if (auto *RI = dyn_cast<ReturnInst>(&I)) {
    C.emitU8(OP_RET);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    if (RI->getNumOperands() == 0) {
      C.emitU8(0);
    } else {
      C.emitU8(1);
      emitOperand(C, RI->getReturnValue());
    }
    return;
  }
  if (auto *Cast = dyn_cast<CastInst>(&I)) {
    if (!isTypeOK(Cast->getType()) || !isTypeOK(Cast->getSrcTy())) {
      C.Failed=true; C.FailReason="cast type"; return;
    }
    uint8_t sub = mapCast(Cast->getOpcode());
    if (sub==0xFF) { C.Failed=true; C.FailReason="cast op"; return; }
    uint32_t dstSz = typeSize(Cast->getType()); if (!dstSz) dstSz=kPtrSize;
    uint32_t dst = C.allocSlot(dstSz);
    C.ValueMap[Cast] = dst;
    C.emitU8(OP_CAST);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU8(sub);
    C.emitU8((uint8_t)dstSz);
    C.emitU64(dst);
    emitOperand(C, Cast->getOperand(0));
    return;
  }
  if (auto *SI = dyn_cast<SelectInst>(&I)) {
    if (!isTypeOK(SI->getType())) { C.Failed=true; C.FailReason="select type"; return; }
    uint32_t sz = typeSize(SI->getType()); if (!sz) sz=kPtrSize;
    uint32_t dst = C.allocSlot(sz);
    C.ValueMap[SI] = dst;
    C.emitU8(OP_SELECT);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU8((uint8_t)sz);
    C.emitU64(dst);
    emitOperand(C, SI->getCondition());
    emitOperand(C, SI->getTrueValue());
    emitOperand(C, SI->getFalseValue());
    return;
  }
  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    uint32_t dst = C.allocSlot(kPtrSize);
    C.ValueMap[GEP] = dst;
    C.emitU8(OP_GEP);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU64(dst);
    emitOperand(C, GEP->getPointerOperand());
    APInt ConstOffset(64, 0);
    if (GEP->hasAllConstantIndices() &&
        GEP->accumulateConstantOffset(GEP->getModule()->getDataLayout(),
                                       ConstOffset)) {
      C.emitU64(1); // scale = 1 (offset already in bytes)
      emitConst(C, ConstOffset.getZExtValue(), kPtrSize);
    } else if (GEP->getNumIndices() == 1) {
      uint32_t scale = typeSize(GEP->getSourceElementType());
      if (!scale) { C.Failed = true; C.FailReason = "GEP elem size"; return; }
      C.emitU64(scale);
      emitOperand(C, *GEP->idx_begin());
    } else if (GEP->getNumIndices() == 2) {
      // Common "[0, i]" array form.
      ConstantInt *C0 = dyn_cast<ConstantInt>(GEP->getOperand(1));
      if (!C0 || !C0->isZero()) { C.Failed = true; C.FailReason = "complex GEP"; return; }
      Type *ArrTy = GEP->getSourceElementType();
      if (!ArrTy->isArrayTy()) { C.Failed = true; C.FailReason = "complex GEP"; return; }
      uint32_t scale = typeSize(ArrTy->getArrayElementType());
      if (!scale) { C.Failed = true; C.FailReason = "GEP elem size"; return; }
      C.emitU64(scale);
      emitOperand(C, GEP->getOperand(2));
    } else {
      C.Failed=true; C.FailReason="complex GEP"; return;
    }
    return;
  }
  if (auto *CB = dyn_cast<CallBase>(&I)) {
    if (CB->isIndirectCall()) { C.Failed=true; C.FailReason="indirect call"; return; }
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isVarArg()) { C.Failed=true; C.FailReason="vararg call"; return; }

    CallSiteInfo CSI;
    CSI.Id          = (uint32_t)C.Calls.size();
    CSI.DirectCallee= Callee;
    CSI.FTy         = CB->getFunctionType();
    CSI.HasRet      = !CB->getType()->isVoidTy();
    if (CSI.HasRet) {
      uint32_t sz = typeSize(CB->getType()); if (!sz) sz=kPtrSize;
      CSI.RetOff  = C.allocSlot(sz);
      CSI.RetSize = sz;
      C.ValueMap[CB] = CSI.RetOff;
    }
    for (Use &U : CB->args()) {
      CallArgInfo AI;
      Value *A = U.get();
      AI.ArgTy = A->getType();
      if (ConstantInt *CI2 = dyn_cast<ConstantInt>(A)) {
        AI.IsConst  = true;
        AI.ConstVal = CI2->getZExtValue();
        AI.Size     = typeSize(A->getType()); if (!AI.Size) AI.Size=8;
      } else if (isa<ConstantPointerNull>(A)) {
        AI.IsConst  = true;
        AI.ConstVal = 0;
        AI.Size     = kPtrSize;
      } else if (GlobalValue *GV = dyn_cast<GlobalValue>(A)) {
        AI.IsConst = false;
        AI.Off     = C.getGlobalSlot(GV);
        AI.Size    = kPtrSize;
      } else {
        AI.IsConst = false;
        AI.Size    = typeSize(A->getType()); if (!AI.Size) AI.Size=kPtrSize;
        auto it = C.ValueMap.find(A);
        AI.Off = it != C.ValueMap.end() ? it->second : C.allocSlot(AI.Size);
      }
      CSI.Args.push_back(AI);
    }
    C.Calls.push_back(CSI);

    C.emitU8(OP_CALL);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU64(CSI.Id);
    return;
  }
  if (isa<UnreachableInst>(I)) {
    C.emitU8(OP_UNREACHABLE);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    return;
  }
  C.Failed = true;
  C.FailReason = std::string("unsupported: ") + I.getOpcodeName();
}

// ─────────────────────────────────────────────────────────────────────────────
// Pre-scan: reject obvious hard failures before touching the function.
// ─────────────────────────────────────────────────────────────────────────────
static bool canVirtualize(Function &F) {
  if (F.isDeclaration() || F.empty())
    return false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<InvokeInst>(I) || isa<CallBrInst>(I) || isa<IndirectBrInst>(I))
        return false;
      if (I.isAtomic())
        return false;
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->isIndirectCall())
          return false;
        if (Function *C = CB->getCalledFunction())
          if (C->isVarArg())
            return false;
      }
      if (I.getType()->isFPOrFPVectorTy() || I.getType()->isVectorTy())
        return false;
      if (isa<FCmpInst>(I) || isa<FPMathOperator>(I))
        return false;
      if (!isTypeOK(I.getType()) && !I.getType()->isVoidTy())
        return false;
      if (isa<FPToSIInst>(I) || isa<FPToUIInst>(I) || isa<SIToFPInst>(I) ||
          isa<UIToFPInst>(I))
        return false;
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Translate entire function: alloc data slots, translate all insns,
// back-patch branch targets, encrypt bytecode.
// ─────────────────────────────────────────────────────────────────────────────
static void translateFunction(VMPContext &C, Function &F) {
  for (Argument &Arg : F.args()) {
    uint32_t sz = typeSize(Arg.getType());
    if (!sz) sz = kPtrSize;
    uint32_t slot = C.allocSlot(sz);
    C.ValueMap[&Arg] = slot;
  }

  std::vector<BasicBlock *> BBs;
  for (BasicBlock &BB : F)
    BBs.push_back(&BB);

  for (BasicBlock *BB : BBs) {
    C.BBMap[BB] = C.ip();
    C.BBInfos.push_back(BBSeedInfo());
    C.BBInfos.back().EntryIP = C.ip();

    for (Instruction &I : *BB) {
      if (!I.isTerminator())
        translateInst(C, I);
      if (C.Failed) return;
    }
    Instruction *Term = BB->getTerminator();
    if (Term) translateInst(C, *Term);
    if (C.Failed) return;

    C.BBInfos.back().EndIP = C.ip();
  }

  // Back-patch branch targets.
  for (auto &[codeOff, targetBB] : C.BrPatches) {
    uint32_t targetIP = C.BBMap[targetBB];
    for (int i = 0; i < 8; i++)
      C.Code[codeOff + i] = (targetIP >> (i * 8)) & 0xFF;
  }

  // Dual-layer encryption per BB.
  for (size_t i = 0; i < C.BBInfos.size(); i++) {
    BBSeedInfo &BBI = C.BBInfos[i];
    BBI.OpcodeSeed = 0x12345678 ^ ((uint32_t)i * 0xDEADBEEF);
    BBI.CodeSeed   = 0x87654321 ^ ((uint32_t)i * 0xCAFEBABE);

    uint32_t os = BBI.OpcodeSeed;
    for (uint32_t off : BBI.OpcodeOffsets) {
      xorshift32(os);
      C.Code[off] ^= (os & 0xFF);
    }

    uint32_t cs = BBI.CodeSeed;
    for (uint32_t j = BBI.EntryIP; j < BBI.EndIP; j++) {
      xorshift32(cs);
      C.Code[j] ^= (cs & 0xFF);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Module-level dynamic-width load/store helpers.
//   i64  @__vmp_ld(i8* p, i8 size)
//   void @__vmp_st(i8* p, i8 size, i64 val)
// ─────────────────────────────────────────────────────────────────────────────
static void getOrCreateVmHelpers(Module &M, Function *&LdFn, Function *&StFn) {
  LdFn = M.getFunction("__vmp_ld");
  StFn = M.getFunction("__vmp_st");
  if (LdFn && StFn)
    return;

  LLVMContext &Ctx = M.getContext();
  Type *I8    = Type::getInt8Ty(Ctx);
  Type *I16   = Type::getInt16Ty(Ctx);
  Type *I32   = Type::getInt32Ty(Ctx);
  Type *I64   = Type::getInt64Ty(Ctx);
  Type *Void  = Type::getVoidTy(Ctx);
  Type *I8Ptr = PointerType::get(I8, 0);
  Align A1(1);

  if (!LdFn) {
    FunctionType *FT = FunctionType::get(I64, {I8Ptr, I8}, false);
    LdFn = Function::Create(FT, GlobalValue::PrivateLinkage, "__vmp_ld", M);
    BasicBlock *E  = BasicBlock::Create(Ctx, "entry", LdFn);
    BasicBlock *C1 = BasicBlock::Create(Ctx, "sz1", LdFn);
    BasicBlock *C2 = BasicBlock::Create(Ctx, "sz2", LdFn);
    BasicBlock *C4 = BasicBlock::Create(Ctx, "sz4", LdFn);
    BasicBlock *C8 = BasicBlock::Create(Ctx, "sz8", LdFn);
    BasicBlock *CD = BasicBlock::Create(Ctx, "def", LdFn);
    BasicBlock *R  = BasicBlock::Create(Ctx, "ret", LdFn);
    IRBuilder<> IRB(E);
    SwitchInst *SW = IRB.CreateSwitch(LdFn->getArg(1), CD, 4);
    SW->addCase(IRB.getInt8(1), C1);
    SW->addCase(IRB.getInt8(2), C2);
    SW->addCase(IRB.getInt8(4), C4);
    SW->addCase(IRB.getInt8(8), C8);

    IRB.SetInsertPoint(C1);
    Value *v1 = IRB.CreateZExt(IRB.CreateAlignedLoad(I8, LdFn->getArg(0), A1), I64);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(C2);
    Value *v2 = IRB.CreateZExt(IRB.CreateAlignedLoad(I16, LdFn->getArg(0), A1), I64);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(C4);
    Value *v4 = IRB.CreateZExt(IRB.CreateAlignedLoad(I32, LdFn->getArg(0), A1), I64);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(C8);
    Value *v8 = IRB.CreateAlignedLoad(I64, LdFn->getArg(0), A1);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(CD);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(R);
    PHINode *PHI = IRB.CreatePHI(I64, 5);
    PHI->addIncoming(v1, C1);
    PHI->addIncoming(v2, C2);
    PHI->addIncoming(v4, C4);
    PHI->addIncoming(v8, C8);
    PHI->addIncoming(ConstantInt::get(I64, 0), CD);
    IRB.CreateRet(PHI);
  }

  if (!StFn) {
    FunctionType *FT = FunctionType::get(Void, {I8Ptr, I8, I64}, false);
    StFn = Function::Create(FT, GlobalValue::PrivateLinkage, "__vmp_st", M);
    BasicBlock *E  = BasicBlock::Create(Ctx, "entry", StFn);
    BasicBlock *C1 = BasicBlock::Create(Ctx, "sz1", StFn);
    BasicBlock *C2 = BasicBlock::Create(Ctx, "sz2", StFn);
    BasicBlock *C4 = BasicBlock::Create(Ctx, "sz4", StFn);
    BasicBlock *C8 = BasicBlock::Create(Ctx, "sz8", StFn);
    BasicBlock *CD = BasicBlock::Create(Ctx, "def", StFn);
    BasicBlock *R  = BasicBlock::Create(Ctx, "ret", StFn);
    IRBuilder<> IRB(E);
    SwitchInst *SW = IRB.CreateSwitch(StFn->getArg(1), CD, 4);
    SW->addCase(IRB.getInt8(1), C1);
    SW->addCase(IRB.getInt8(2), C2);
    SW->addCase(IRB.getInt8(4), C4);
    SW->addCase(IRB.getInt8(8), C8);

    IRB.SetInsertPoint(C1);
    IRB.CreateAlignedStore(IRB.CreateTrunc(StFn->getArg(2), I8), StFn->getArg(0), A1);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(C2);
    IRB.CreateAlignedStore(IRB.CreateTrunc(StFn->getArg(2), I16), StFn->getArg(0), A1);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(C4);
    IRB.CreateAlignedStore(IRB.CreateTrunc(StFn->getArg(2), I32), StFn->getArg(0), A1);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(C8);
    IRB.CreateAlignedStore(StFn->getArg(2), StFn->getArg(0), A1);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(CD);
    IRB.CreateBr(R);
    IRB.SetInsertPoint(R);
    IRB.CreateRetVoid();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Replace F's body with the software interpreter.
// ─────────────────────────────────────────────────────────────────────────────
static void buildInterpreter(VMPContext &C, Function &F) {
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();

  Type *I8    = Type::getInt8Ty(Ctx);
  Type *I32   = Type::getInt32Ty(Ctx);
  Type *I64   = Type::getInt64Ty(Ctx);
  Type *I1    = Type::getInt1Ty(Ctx);
  Type *Void  = Type::getVoidTy(Ctx);
  Type *I8Ptr = PointerType::get(I8, 0);

  Function *LdFn, *StFn;
  getOrCreateVmHelpers(*M, LdFn, StFn);

  // Emit encrypted bytecode + seed tables as globals.
  std::string Base = F.getName().str();
  uint32_t CodeSize = (uint32_t)C.Code.size();
  ArrayType *CodeArrTy = ArrayType::get(I8, CodeSize);
  GlobalVariable *codeGV = new GlobalVariable(
      *M, CodeArrTy, true, GlobalValue::PrivateLinkage,
      ConstantDataArray::get(Ctx, C.Code), "__vmp_code_" + Base);

  uint32_t N = (uint32_t)C.BBInfos.size();
  ArrayType *SeedArrTy = ArrayType::get(I32, N);
  std::vector<Constant *> EntryVals, CSeedVals, OSeedVals;
  for (BBSeedInfo &BBI : C.BBInfos) {
    EntryVals.push_back(ConstantInt::get(I32, BBI.EntryIP));
    CSeedVals.push_back(ConstantInt::get(I32, BBI.CodeSeed));
    OSeedVals.push_back(ConstantInt::get(I32, BBI.OpcodeSeed));
  }
  GlobalVariable *entryGV = new GlobalVariable(
      *M, SeedArrTy, true, GlobalValue::PrivateLinkage,
      ConstantArray::get(SeedArrTy, EntryVals), "__vmp_entry_" + Base);
  GlobalVariable *cseedGV = new GlobalVariable(
      *M, SeedArrTy, true, GlobalValue::PrivateLinkage,
      ConstantArray::get(SeedArrTy, CSeedVals), "__vmp_cseed_" + Base);
  GlobalVariable *oseedGV = new GlobalVariable(
      *M, SeedArrTy, true, GlobalValue::PrivateLinkage,
      ConstantArray::get(SeedArrTy, OSeedVals), "__vmp_oseed_" + Base);

  // Erase the original body.
  while (!F.empty()) {
    BasicBlock &BB = F.front();
    BB.eraseFromParent();
  }

  BasicBlock *EntryBB = BasicBlock::Create(Ctx, "entry", &F);
  IRBuilder<> IRB(EntryBB);

  // Data area.
  uint32_t DataBytes = std::max(C.DataSize, 1u);
  Value *Data = IRB.CreateAlloca(ArrayType::get(I8, DataBytes), nullptr, "vmp.data");
  Data = IRB.CreateBitCast(Data, I8Ptr);

  AllocaInst *ipA = IRB.CreateAlloca(I64, nullptr, "vmp.ip");
  AllocaInst *csA = IRB.CreateAlloca(I32, nullptr, "vmp.cs");
  AllocaInst *osA = IRB.CreateAlloca(I32, nullptr, "vmp.os");
  IRB.CreateStore(ConstantInt::get(I64, 0), ipA);

  // Store arguments into data area.
  for (Argument &Arg : F.args()) {
    uint32_t slot = C.ValueMap[&Arg];
    uint32_t sz = typeSize(Arg.getType()); if (!sz) sz = kPtrSize;
    Value *v64 = Arg.getType()->isPointerTy()
                     ? IRB.CreatePtrToInt(&Arg, I64)
                     : IRB.CreateZExt(&Arg, I64);
    Value *p = IRB.CreateGEP(I8, Data, IRB.getInt64(slot));
    IRB.CreateCall(StFn, {p, IRB.getInt8(sz), v64});
  }

  // Store global addresses into data area.
  for (auto &[GV, slot] : C.Globals) {
    Value *v64 = IRB.CreatePtrToInt(GV, I64);
    Value *p = IRB.CreateGEP(I8, Data, IRB.getInt64(slot));
    IRB.CreateCall(StFn, {p, IRB.getInt8(kPtrSize), v64});
  }

  BasicBlock *DispatchBB = BasicBlock::Create(Ctx, "dispatch", &F);
  IRB.CreateBr(DispatchBB);
  IRB.SetInsertPoint(DispatchBB);

  // --- lambdas (share this IRBuilder; SetInsertPoint drives where they emit) ---
  auto loadIP  = [&]() { return IRB.CreateLoad(I64, ipA); };
  auto storeIP = [&](Value *v) { return IRB.CreateStore(v, ipA); };
  auto xorshift = [&](Value *s) {
    s = IRB.CreateXor(s, IRB.CreateShl(s, 13));
    s = IRB.CreateXor(s, IRB.CreateLShr(s, 17));
    s = IRB.CreateXor(s, IRB.CreateShl(s, 5));
    return s;
  };
  // Read + decrypt one code byte (optionally opcode-layer), advancing ip/cs/os.
  auto readByte = [&](bool isOp) {
    Value *ip = loadIP();
    Value *cs = IRB.CreateLoad(I32, csA);
    cs = xorshift(cs);
    IRB.CreateStore(cs, csA);
    Value *key = IRB.CreateAnd(cs, ConstantInt::get(I32, 0xFF));
    if (isOp) {
      Value *os = IRB.CreateLoad(I32, osA);
      os = xorshift(os);
      IRB.CreateStore(os, osA);
      key = IRB.CreateXor(key, IRB.CreateAnd(os, ConstantInt::get(I32, 0xFF)));
    }
    Value *gep = IRB.CreateGEP(CodeArrTy, codeGV, {IRB.getInt64(0), ip});
    Value *byte = IRB.CreateLoad(I8, gep);
    Value *dec = IRB.CreateXor(byte, IRB.CreateTrunc(key, I8));
    storeIP(IRB.CreateAdd(ip, IRB.getInt64(1)));
    return dec; // i8
  };
  auto readU64 = [&]() {
    Value *r = ConstantInt::get(I64, 0);
    for (int i = 0; i < 8; i++) {
      Value *b = readByte(false);
      r = IRB.CreateOr(r, IRB.CreateShl(IRB.CreateZExt(b, I64), IRB.getInt64(i * 8)));
    }
    return r; // i64
  };
  // Read [size][type][8-byte] operand → {i64 value, i8 size}.
  auto readOperand = [&]() {
    Value *sz8 = readByte(false);
    Value *ty8 = readByte(false);
    Value *payload = readU64();
    Value *p = IRB.CreateGEP(I8, Data, payload);
    Value *varVal = IRB.CreateCall(LdFn, {p, sz8});
    Value *isConst = IRB.CreateICmpNE(ty8, IRB.getInt8(0));
    Value *val = IRB.CreateSelect(isConst, payload, varVal);
    return std::make_pair(val, sz8);
  };
  auto toPtr = [&](Value *v) { return IRB.CreateIntToPtr(v, I8Ptr); };
  auto stSlot = [&](Value *slot, Value *sz, Value *v) {
    Value *p = IRB.CreateGEP(I8, Data, slot);
    IRB.CreateCall(StFn, {p, sz, v});
  };

  // --- dispatch: locate BB from ip, reset cs/os, read opcode ---
  Value *ip = loadIP();
  Value *bbidx = ConstantInt::get(I64, 0);
  for (uint32_t i = 1; i < N; i++) {
    Value *e = IRB.getInt64(C.BBInfos[i].EntryIP);
    bbidx = IRB.CreateSelect(IRB.CreateICmpUGE(ip, e), IRB.getInt64(i), bbidx);
  }
  IRB.CreateStore(IRB.CreateLoad(I32, IRB.CreateGEP(SeedArrTy, cseedGV, {IRB.getInt64(0), bbidx})), csA);
  IRB.CreateStore(IRB.CreateLoad(I32, IRB.CreateGEP(SeedArrTy, oseedGV, {IRB.getInt64(0), bbidx})), osA);

  Value *op = IRB.CreateZExt(readByte(true), I32);

  BasicBlock *bbDef    = BasicBlock::Create(Ctx, "vmp.def", &F);
  BasicBlock *bbNop    = BasicBlock::Create(Ctx, "vmp.nop", &F);
  BasicBlock *bbAlloca = BasicBlock::Create(Ctx, "vmp.alloca", &F);
  BasicBlock *bbLoad   = BasicBlock::Create(Ctx, "vmp.load", &F);
  BasicBlock *bbStore  = BasicBlock::Create(Ctx, "vmp.store", &F);
  BasicBlock *bbBinop  = BasicBlock::Create(Ctx, "vmp.binop", &F);
  BasicBlock *bbIcmp   = BasicBlock::Create(Ctx, "vmp.icmp", &F);
  BasicBlock *bbBr     = BasicBlock::Create(Ctx, "vmp.br", &F);
  BasicBlock *bbRet    = BasicBlock::Create(Ctx, "vmp.ret", &F);
  BasicBlock *bbGep    = BasicBlock::Create(Ctx, "vmp.gep", &F);
  BasicBlock *bbCall   = BasicBlock::Create(Ctx, "vmp.call", &F);
  BasicBlock *bbCast   = BasicBlock::Create(Ctx, "vmp.cast", &F);
  BasicBlock *bbSelect = BasicBlock::Create(Ctx, "vmp.select", &F);
  BasicBlock *bbUnreach= BasicBlock::Create(Ctx, "vmp.unreach", &F);

  SwitchInst *SW = IRB.CreateSwitch(op, bbDef, 13);
  SW->addCase(IRB.getInt32(OP_NOP), bbNop);
  SW->addCase(IRB.getInt32(OP_ALLOCA), bbAlloca);
  SW->addCase(IRB.getInt32(OP_LOAD), bbLoad);
  SW->addCase(IRB.getInt32(OP_STORE), bbStore);
  SW->addCase(IRB.getInt32(OP_BINOP), bbBinop);
  SW->addCase(IRB.getInt32(OP_ICMP), bbIcmp);
  SW->addCase(IRB.getInt32(OP_BR), bbBr);
  SW->addCase(IRB.getInt32(OP_RET), bbRet);
  SW->addCase(IRB.getInt32(OP_GEP), bbGep);
  SW->addCase(IRB.getInt32(OP_CALL), bbCall);
  SW->addCase(IRB.getInt32(OP_CAST), bbCast);
  SW->addCase(IRB.getInt32(OP_SELECT), bbSelect);
  SW->addCase(IRB.getInt32(OP_UNREACHABLE), bbUnreach);

  // default → abort (should be unreachable)
  IRB.SetInsertPoint(bbDef);
  Function *abortFn = cast<Function>(M->getOrInsertFunction(
      "abort", FunctionType::get(Void, false)).getCallee());
  abortFn->addFnAttr(Attribute::NoReturn);
  IRB.CreateCall(abortFn);
  IRB.CreateUnreachable();

  // OP_NOP
  IRB.SetInsertPoint(bbNop);
  IRB.CreateBr(DispatchBB);

  // OP_ALLOCA
  IRB.SetInsertPoint(bbAlloca);
  {
    Value *slot = readU64();
    Value *sz = readU64();
    Value *ptrSlot = readU64();
    Value *ptr = IRB.CreateGEP(I8, Data, slot);
    stSlot(ptrSlot, IRB.getInt8(kPtrSize), IRB.CreatePtrToInt(ptr, I64));
    IRB.CreateBr(DispatchBB);
  }

  // OP_LOAD
  IRB.SetInsertPoint(bbLoad);
  {
    Value *sz = readByte(false);
    Value *dst = readU64();
    auto [ptrval, _] = readOperand();
    Value *v = IRB.CreateCall(LdFn, {toPtr(ptrval), sz});
    stSlot(dst, sz, v);
    IRB.CreateBr(DispatchBB);
  }

  // OP_STORE
  IRB.SetInsertPoint(bbStore);
  {
    auto [val, sz] = readOperand();
    auto [ptrval, _] = readOperand();
    IRB.CreateCall(StFn, {toPtr(ptrval), sz, val});
    IRB.CreateBr(DispatchBB);
  }

  // OP_BINOP — select mask/shiftmask, then switch on sub-op.
  IRB.SetInsertPoint(bbBinop);
  {
    Value *sub = readByte(false);
    Value *sz = readByte(false);
    Value *dst = readU64();
    auto [a, _] = readOperand();
    auto [b, __] = readOperand();

    Value *mask = IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(1)), IRB.getInt64(0xFF),
        IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(2)), IRB.getInt64(0xFFFF),
        IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(4)), IRB.getInt64(0xFFFFFFFF),
        IRB.getInt64(-1))));
    Value *shmask = IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(1)), IRB.getInt64(7),
        IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(2)), IRB.getInt64(15),
        IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(4)), IRB.getInt64(31),
        IRB.getInt64(63))));
    Value *shiftAmt = IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(1)), IRB.getInt64(56),
        IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(2)), IRB.getInt64(48),
        IRB.CreateSelect(IRB.CreateICmpEQ(sz, IRB.getInt8(4)), IRB.getInt64(32),
        IRB.getInt64(0))));
    // Sign-extend operands for signed ops (operands are stored zero-extended).
    Value *a_sext = IRB.CreateAShr(IRB.CreateShl(a, shiftAmt), shiftAmt);
    Value *b_sext = IRB.CreateAShr(IRB.CreateShl(b, shiftAmt), shiftAmt);

    BasicBlock *done = BasicBlock::Create(Ctx, "vmp.binop.done", &F);
    PHINode *result = PHINode::Create(I64, 13, "vmp.binop.res", done);

    const char *names[13] = {"add","sub","mul","udiv","sdiv","urem","srem","and","or","xor","shl","lshr","ashr"};
    for (int k = 0; k < 13; k++) {
      BasicBlock *cb = BasicBlock::Create(Ctx, std::string("vmp.binop.") + names[k], &F);
      IRB.SetInsertPoint(cb);
      Value *r = nullptr;
      switch (k) {
        case BIN_ADD:  r = IRB.CreateAdd(a, b); break;
        case BIN_SUB:  r = IRB.CreateSub(a, b); break;
        case BIN_MUL:  r = IRB.CreateMul(a, b); break;
        case BIN_UDIV: r = IRB.CreateUDiv(a, b); break;
        case BIN_SDIV: r = IRB.CreateSDiv(a_sext, b_sext); break;
        case BIN_UREM: r = IRB.CreateURem(a, b); break;
        case BIN_SREM: r = IRB.CreateSRem(a_sext, b_sext); break;
        case BIN_AND:  r = IRB.CreateAnd(a, b); break;
        case BIN_OR:   r = IRB.CreateOr(a, b); break;
        case BIN_XOR:  r = IRB.CreateXor(a, b); break;
        case BIN_SHL:  r = IRB.CreateShl(a, IRB.CreateAnd(b, shmask)); break;
        case BIN_LSHR: r = IRB.CreateLShr(a, IRB.CreateAnd(b, shmask)); break;
        case BIN_ASHR: r = IRB.CreateAShr(a_sext, IRB.CreateAnd(b, shmask)); break;
      }
      result->addIncoming(r, cb);
      IRB.CreateBr(done);
    }

    IRB.SetInsertPoint(bbBinop);
    SwitchInst *BS = IRB.CreateSwitch(IRB.CreateZExt(sub, I32), bbDef, 13);
    for (int k = 0; k < 13; k++) {
      // find block by name
      BS->addCase(IRB.getInt32(k), result->getIncomingBlock(k));
    }

    IRB.SetInsertPoint(done);
    Value *final = IRB.CreateAnd(result, mask);
    stSlot(dst, sz, final);
    IRB.CreateBr(DispatchBB);
  }

  // OP_ICMP
  IRB.SetInsertPoint(bbIcmp);
  {
    Value *pred = readByte(false);
    Value *dst = readU64();
    auto [a, szA] = readOperand();
    auto [b, _] = readOperand();

    // Sign-extend operands for signed predicates (stored zero-extended).
    Value *shiftAmt = IRB.CreateSelect(IRB.CreateICmpEQ(szA, IRB.getInt8(1)), IRB.getInt64(56),
        IRB.CreateSelect(IRB.CreateICmpEQ(szA, IRB.getInt8(2)), IRB.getInt64(48),
        IRB.CreateSelect(IRB.CreateICmpEQ(szA, IRB.getInt8(4)), IRB.getInt64(32),
        IRB.getInt64(0))));
    Value *a_sext = IRB.CreateAShr(IRB.CreateShl(a, shiftAmt), shiftAmt);
    Value *b_sext = IRB.CreateAShr(IRB.CreateShl(b, shiftAmt), shiftAmt);

    const char *names[10] = {"eq","ne","ugt","uge","ult","ule","sgt","sge","slt","sle"};
    BasicBlock *done = BasicBlock::Create(Ctx, "vmp.icmp.done", &F);
    PHINode *res = PHINode::Create(I64, 10, "vmp.icmp.res", done);
    for (int k = 0; k < 10; k++) {
      BasicBlock *cb = BasicBlock::Create(Ctx, std::string("vmp.icmp.") + names[k], &F);
      IRB.SetInsertPoint(cb);
      CmpInst::Predicate P;
      bool IsSigned = false;
      switch (k) {
        case CMP_EQ: P=CmpInst::ICMP_EQ; break; case CMP_NE: P=CmpInst::ICMP_NE; break;
        case CMP_UGT: P=CmpInst::ICMP_UGT; break; case CMP_UGE: P=CmpInst::ICMP_UGE; break;
        case CMP_ULT: P=CmpInst::ICMP_ULT; break; case CMP_ULE: P=CmpInst::ICMP_ULE; break;
        case CMP_SGT: P=CmpInst::ICMP_SGT; IsSigned=true; break;
        case CMP_SGE: P=CmpInst::ICMP_SGE; IsSigned=true; break;
        case CMP_SLT: P=CmpInst::ICMP_SLT; IsSigned=true; break;
        case CMP_SLE: P=CmpInst::ICMP_SLE; IsSigned=true; break;
      }
      Value *opA = IsSigned ? a_sext : a;
      Value *opB = IsSigned ? b_sext : b;
      Value *r = IRB.CreateZExt(IRB.CreateICmp(P, opA, opB), I64);
      res->addIncoming(r, cb);
      IRB.CreateBr(done);
    }
    IRB.SetInsertPoint(bbIcmp);
    SwitchInst *CS = IRB.CreateSwitch(IRB.CreateZExt(pred, I32), bbDef, 10);
    for (int k = 0; k < 10; k++)
      CS->addCase(IRB.getInt32(k), res->getIncomingBlock(k));

    IRB.SetInsertPoint(done);
    stSlot(dst, IRB.getInt8(1), res);
    IRB.CreateBr(DispatchBB);
  }

  // OP_BR
  IRB.SetInsertPoint(bbBr);
  {
    Value *kind = readByte(false);
    BasicBlock *bUnc = BasicBlock::Create(Ctx, "vmp.br.uncond", &F);
    BasicBlock *bCnd = BasicBlock::Create(Ctx, "vmp.br.cond", &F);
    SwitchInst *KS = IRB.CreateSwitch(IRB.CreateZExt(kind, I32), bUnc, 1);
    KS->addCase(IRB.getInt32(1), bCnd);

    IRB.SetInsertPoint(bUnc);
    Value *target = readU64();
    storeIP(target);
    IRB.CreateBr(DispatchBB);

    IRB.SetInsertPoint(bCnd);
    auto [condv, _] = readOperand();
    Value *t = readU64();
    Value *f = readU64();
    Value *cond = IRB.CreateTrunc(condv, I1);
    storeIP(IRB.CreateSelect(cond, t, f));
    IRB.CreateBr(DispatchBB);
  }

  // OP_RET
  IRB.SetInsertPoint(bbRet);
  {
    Value *hasRet = readByte(false);
    Type *RetTy = F.getReturnType();
    if (RetTy->isVoidTy()) {
      IRB.CreateRetVoid();
    } else {
      auto [val, _] = readOperand();
      Value *r = nullptr;
      if (RetTy->isPointerTy())
        r = IRB.CreateIntToPtr(val, RetTy);
      else if (RetTy->isIntegerTy())
        r = IRB.CreateTrunc(val, RetTy);
      else
        r = Constant::getNullValue(RetTy);
      IRB.CreateRet(r);
    }
  }

  // OP_GEP
  IRB.SetInsertPoint(bbGep);
  {
    Value *dst = readU64();
    auto [base, _] = readOperand();
    Value *scale = readU64();
    auto [off, __] = readOperand();
    Value *byteOff = IRB.CreateMul(off, scale);
    Value *p = IRB.CreateGEP(I8, toPtr(base), byteOff);
    stSlot(dst, IRB.getInt8(kPtrSize), IRB.CreatePtrToInt(p, I64));
    IRB.CreateBr(DispatchBB);
  }

  // OP_CALL — dispatch on call-site id to per-call blocks.
  IRB.SetInsertPoint(bbCall);
  {
    Value *callId = readU64();
    if (C.Calls.empty()) {
      IRB.CreateBr(DispatchBB);
    } else {
      BasicBlock *callDef = BasicBlock::Create(Ctx, "vmp.call.def", &F);
      SwitchInst *CS = IRB.CreateSwitch(callId, callDef, (unsigned)C.Calls.size());
      std::vector<BasicBlock *> callBBs;
      for (uint32_t i = 0; i < C.Calls.size(); i++) {
        BasicBlock *cb = BasicBlock::Create(Ctx, "vmp.call." + Twine(i), &F);
        CS->addCase(IRB.getInt64(i), cb);
        callBBs.push_back(cb);
      }
      IRB.SetInsertPoint(callDef);
      IRB.CreateBr(DispatchBB);

      for (uint32_t i = 0; i < C.Calls.size(); i++) {
        CallSiteInfo &CSI = C.Calls[i];
        IRB.SetInsertPoint(callBBs[i]);
        std::vector<Value *> Args;
        for (CallArgInfo &AI : CSI.Args) {
          Type *ATy = AI.ArgTy;
          if (AI.IsConst) {
            if (ATy->isPointerTy())
              Args.push_back(ConstantPointerNull::get(cast<PointerType>(ATy)));
            else
              Args.push_back(ConstantInt::get(ATy, AI.ConstVal));
          } else {
            Value *p = IRB.CreateGEP(I8, Data, IRB.getInt64(AI.Off));
            Args.push_back(IRB.CreateAlignedLoad(ATy, p, Align(1)));
          }
        }
        Value *ret = IRB.CreateCall(CSI.FTy, CSI.DirectCallee, Args);
        if (CSI.HasRet) {
          Value *v64 = ret->getType()->isPointerTy()
                           ? IRB.CreatePtrToInt(ret, I64)
                           : IRB.CreateZExt(ret, I64);
          stSlot(IRB.getInt64(CSI.RetOff), IRB.getInt8(CSI.RetSize), v64);
        }
        IRB.CreateBr(DispatchBB);
      }
    }
  }

  // OP_CAST
  IRB.SetInsertPoint(bbCast);
  {
    Value *sub = readByte(false);
    Value *dstSz = readByte(false);
    Value *dst = readU64();
    auto [src, srcSz] = readOperand();

    // Sign-extend src from its own width (for SExt).
    Value *shiftAmt = IRB.CreateSelect(IRB.CreateICmpEQ(srcSz, IRB.getInt8(1)), IRB.getInt64(56),
        IRB.CreateSelect(IRB.CreateICmpEQ(srcSz, IRB.getInt8(2)), IRB.getInt64(48),
        IRB.CreateSelect(IRB.CreateICmpEQ(srcSz, IRB.getInt8(4)), IRB.getInt64(32),
        IRB.getInt64(0))));
    Value *src_sext = IRB.CreateAShr(IRB.CreateShl(src, shiftAmt), shiftAmt);

    const char *names[6] = {"trunc","zext","sext","bitcast","ptrtoint","inttoptr"};
    BasicBlock *done = BasicBlock::Create(Ctx, "vmp.cast.done", &F);
    PHINode *res = PHINode::Create(I64, 6, "vmp.cast.res", done);
    for (int k = 0; k < 6; k++) {
      BasicBlock *cb = BasicBlock::Create(Ctx, std::string("vmp.cast.") + names[k], &F);
      IRB.SetInsertPoint(cb);
      Value *r = nullptr;
      switch (k) {
        case CAST_TRUNC:    r = src; break;          // truncate on store
        case CAST_ZEXT:     r = src; break;          // already zero-extended
        case CAST_SEXT:     r = src_sext; break;     // sign-extended from src width
        case CAST_BITCAST:  r = src; break;          // bit-pattern identity
        case CAST_PTRTOINT: r = src; break;
        case CAST_INTTOPTR: r = src; break;
      }
      res->addIncoming(r, cb);
      IRB.CreateBr(done);
    }
    IRB.SetInsertPoint(bbCast);
    SwitchInst *CS = IRB.CreateSwitch(IRB.CreateZExt(sub, I32), bbDef, 6);
    for (int k = 0; k < 6; k++)
      CS->addCase(IRB.getInt32(k), res->getIncomingBlock(k));

    IRB.SetInsertPoint(done);
    stSlot(dst, dstSz, res);
    IRB.CreateBr(DispatchBB);
  }

  // OP_SELECT
  IRB.SetInsertPoint(bbSelect);
  {
    Value *sz = readByte(false);
    Value *dst = readU64();
    auto [condv, _] = readOperand();
    auto [t, __] = readOperand();
    auto [f, ___] = readOperand();
    Value *cond = IRB.CreateTrunc(condv, I1);
    stSlot(dst, sz, IRB.CreateSelect(cond, t, f));
    IRB.CreateBr(DispatchBB);
  }

  // OP_UNREACHABLE
  IRB.SetInsertPoint(bbUnreach);
  IRB.CreateUnreachable();
}

// ─────────────────────────────────────────────────────────────────────────────
// Main entry point
// ─────────────────────────────────────────────────────────────────────────────
PreservedAnalyses Virtualization::run(Module &M, ModuleAnalysisManager &AM) {
  bool AnyModified = false;

  for (Function &F : M) {
    if (!enabled &&
        getFunctionAnnotation(&F).find("vmp") == std::string::npos)
      continue;
    if (!canVirtualize(F))
      continue;

    // Demote PHIs/escaped regs to stack (semantically transparent).
    fixStack(F);
    AnyModified = true;

    VMPContext C;
    translateFunction(C, F);

    if (C.Failed) {
      // F is left demoted (still correct, just deoptimized). Never virtualize.
      continue;
    }

    buildInterpreter(C, F);
  }

  return AnyModified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
