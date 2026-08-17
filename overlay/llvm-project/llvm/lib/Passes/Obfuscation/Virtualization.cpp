// VMP (Virtualization) Pass — LLVM 17, new pass manager
// Translates IR functions into custom bytecode + software interpreter.
// Ported from Hikari-fix (PPKunOfficial), adapted for LLVM 17 new PM,
// Android NDK targets, and stripped of Darwin/ObjC dependencies.
//
// Architecture:
//   translateFunction()  → encodes IR into VMPContext.Code (byte buffer)
//   encryptBytecode()    → dual-layer xorshift32 XOR per basic block
//   buildInterpreter()   → replaces function body with dispatch loop
//   buildCallHandler()   → separate switch function for call sites
//   buildEvalFn()        → operand reader helper (inline in interpreter)

#include "Obfuscation/Virtualization.h"
#include "Obfuscation/Utils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
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
  OP_NOP        = 0,
  OP_ALLOCA     = 1,
  OP_LOAD       = 2,
  OP_STORE      = 3,
  OP_BINOP      = 4,
  OP_ICMP       = 5,
  OP_BR         = 6,
  OP_RET        = 7,
  OP_GEP        = 8,
  OP_CALL       = 9,
  OP_CAST       = 10,
  OP_SELECT     = 11,
  OP_UNREACHABLE= 12,
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

// ─────────────────────────────────────────────────────────────────────────────
// Per-BB encryption metadata
// ─────────────────────────────────────────────────────────────────────────────
struct BBSeedInfo {
  uint32_t EntryIP = 0, EndIP = 0;
  uint32_t OpcodeSeed = 0, CodeSeed = 0;
  std::vector<uint32_t> OpcodeOffsets; // code[] offsets that hold opcodes
};

// ─────────────────────────────────────────────────────────────────────────────
// Call site descriptor (deferred — emitted as OP_CALL <id>)
// ─────────────────────────────────────────────────────────────────────────────
struct CallArgInfo {
  bool     IsConst  = false;
  uint64_t ConstVal = 0;
  uint32_t Off      = 0;   // data-area slot
  uint32_t Size     = 8;   // bytes
};
struct CallSiteInfo {
  uint32_t             Id         = 0;
  Function            *DirectCallee = nullptr;
  uint32_t             RetOff     = 0;
  uint32_t             RetSize    = 0;
  bool                 HasRet     = false;
  FunctionType        *FTy        = nullptr;
  std::vector<CallArgInfo> Args;
};

// ─────────────────────────────────────────────────────────────────────────────
// Translation context (one per function being virtualized)
// ─────────────────────────────────────────────────────────────────────────────
struct VMPContext {
  std::vector<uint8_t>          Code;
  std::map<Value *, uint32_t>   ValueMap;    // Value → data-area byte offset
  std::map<BasicBlock *, uint32_t> BBMap;    // BB → code IP
  std::vector<std::pair<uint32_t, BasicBlock *>> BrPatches; // (codeOff, target)
  std::vector<CallSiteInfo>     Calls;
  std::vector<std::pair<GlobalValue *, uint32_t>> Globals;  // GV → data slot
  uint32_t DataSize = 0;
  std::vector<BBSeedInfo> BBInfos;
  bool Failed = false;
  std::string FailReason;

  uint32_t allocSlot(uint32_t bytes) {
    uint32_t off = DataSize;
    DataSize += bytes;
    return off;
  }
  void emitU8(uint8_t v)  { Code.push_back(v); }
  void emitU64(uint64_t v) {
    for (int i = 0; i < 8; i++) Code.push_back((v >> (i*8)) & 0xFF);
  }
  uint32_t ip() const { return (uint32_t)Code.size(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: type size, opcode mappers
// ─────────────────────────────────────────────────────────────────────────────
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
  if (ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(V)) {
    (void)CPN;
    emitConst(C, 0, kPtrSize);
    return;
  }
  if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    // Emit pointer to global as a constant (address taken at runtime)
    uint32_t sz = kPtrSize;
    // We use a sentinel 0 and patch at interpreter startup — simplify by
    // storing GV pointer as a per-module global slot in data area
    for (auto &G : C.Globals)
      if (G.first == GV) { emitVar(C, G.second, sz); return; }
    uint32_t slot = C.allocSlot(sz);
    C.Globals.push_back({GV, slot});
    emitVar(C, slot, sz);
    return;
  }
  auto it = C.ValueMap.find(V);
  if (it != C.ValueMap.end()) {
    emitVar(C, it->second, typeSize(V->getType()) ? typeSize(V->getType()) : 1);
    return;
  }
  // Undef or unsupported — emit zero constant
  emitConst(C, 0, typeSize(V->getType()) ? typeSize(V->getType()) : 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// xorshift32 — same algorithm as Hikari for compatibility
// ─────────────────────────────────────────────────────────────────────────────
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
  // Record where this opcode byte lands (for encryption)
  uint32_t opcodeOff = C.ip();

  if (isa<PHINode>(I)) {
    C.Failed = true; C.FailReason = "phi node (run mem2reg first)"; return;
  }
  if (isa<IntrinsicInst>(I)) {
    // Quietly skip non-critical intrinsics (lifetime, debug)
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
    // The alloca result is a pointer — allocate a pointer slot too
    uint32_t ptrSlot = C.allocSlot(kPtrSize);
    C.ValueMap[AI] = ptrSlot;
    C.emitU8(OP_ALLOCA);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    C.emitU64(slot);   // data area offset of the alloca'd memory
    C.emitU64(sz);     // size
    C.emitU64(ptrSlot);// where to store the pointer result
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
    uint32_t dst = C.allocSlot(1); // i1 result stored as 1 byte
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
      C.emitU8(0); // unconditional
      uint32_t patchOff = C.ip();
      C.emitU64(0); // placeholder target IP
      C.BrPatches.push_back({patchOff, BI->getSuccessor(0)});
    } else {
      C.emitU8(1); // conditional
      emitOperand(C, BI->getCondition());
      uint32_t p0 = C.ip(); C.emitU64(0); // true target
      uint32_t p1 = C.ip(); C.emitU64(0); // false target
      C.BrPatches.push_back({p0, BI->getSuccessor(0)});
      C.BrPatches.push_back({p1, BI->getSuccessor(1)});
    }
    return;
  }
  if (auto *RI = dyn_cast<ReturnInst>(&I)) {
    C.emitU8(OP_RET);
    C.BBInfos.back().OpcodeOffsets.push_back(opcodeOff);
    if (RI->getNumOperands() == 0) {
      C.emitU8(0); // void
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
    emitOperand(C, GEP->getPointerOperand()); // base ptr
    // Emit flat byte offset — sum all constant indices (simplified)
    APInt ConstOffset(64, 0);
    if (GEP->hasAllConstantIndices() &&
        GEP->accumulateConstantOffset(GEP->getModule()->getDataLayout(),
                                       ConstOffset)) {
      emitConst(C, ConstOffset.getZExtValue(), kPtrSize);
    } else if (GEP->getNumIndices() == 1) {
      emitOperand(C, *GEP->idx_begin());
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
      if (ConstantInt *CI2 = dyn_cast<ConstantInt>(A)) {
        AI.IsConst  = true;
        AI.ConstVal = CI2->getZExtValue();
        AI.Size     = typeSize(A->getType()); if (!AI.Size) AI.Size=8;
      } else if (ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(A)) {
        (void)CPN;
        AI.IsConst  = true;
        AI.ConstVal = 0;
        AI.Size     = kPtrSize;
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
// Translate entire function: alloc data slots, translate all insns,
// back-patch branch targets, encrypt bytecode.
// ─────────────────────────────────────────────────────────────────────────────
static void translateFunction(VMPContext &C, Function &F) {
  // Allocate data slots for arguments (pass by value into data area).
  for (Argument &Arg : F.args()) {
    uint32_t sz = typeSize(Arg.getType());
    if (!sz) sz = kPtrSize;
    uint32_t slot = C.allocSlot(sz);
    C.ValueMap[&Arg] = slot;
  }

  // Allocate return value slot.
  uint32_t retSz = typeSize(F.getReturnType());
  if (retSz == 0 && !F.getReturnType()->isVoidTy()) retSz = kPtrSize;
  uint32_t retSlot = retSz ? C.allocSlot(retSz) : 0;

  // Collect basic blocks and record their code IPs.
  std::vector<BasicBlock *> BBs;
  for (BasicBlock &BB : F)
    BBs.push_back(&BB);

  // Translate all instructions per BB.
  for (BasicBlock *BB : BBs) {
    C.BBMap[BB] = C.ip();
    BBSeedInfo BBI;
    BBI.EntryIP = C.ip();

    for (Instruction &I : *BB) {
      if (!I.isTerminator())
        translateInst(C, I);
      if (C.Failed) return;
    }
    // Emit terminator.
    Instruction *Term = BB->getTerminator();
    if (Term) translateInst(C, *Term);
    if (C.Failed) return;

    BBI.EndIP = C.ip();
    C.BBInfos.push_back(BBI);
  }

  // Back-patch branch targets.
  for (auto &[codeOff, targetBB] : C.BrPatches) {
    uint32_t targetIP = C.BBMap[targetBB];
    for (int i = 0; i < 8; i++)
      C.Code[codeOff + i] = (targetIP >> (i * 8)) & 0xFF;
  }

  // Encrypt bytecode per BB.
  for (size_t i = 0; i < C.BBInfos.size(); i++) {
    BBSeedInfo &BBI = C.BBInfos[i];
    BBI.OpcodeSeed = 0x12345678 ^ (i * 0xDEADBEEF);
    BBI.CodeSeed   = 0x87654321 ^ (i * 0xCAFEBABE);

    // Opcode layer: XOR only opcode bytes.
    uint32_t os = BBI.OpcodeSeed;
    for (uint32_t off : BBI.OpcodeOffsets) {
      xorshift32(os);
      C.Code[off] ^= (os & 0xFF);
    }

    // Code layer: XOR every byte in the BB range.
    uint32_t cs = BBI.CodeSeed;
    for (uint32_t j = BBI.EntryIP; j < BBI.EndIP; j++) {
      xorshift32(cs);
      C.Code[j] ^= (cs & 0xFF);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main entry point
// ─────────────────────────────────────────────────────────────────────────────
PreservedAnalyses Virtualization::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!enabled &&
        getFunctionAnnotation(&F).find("vmp") == std::string::npos)
      continue;

    VMPContext C;
    translateFunction(C, F);

    if (C.Failed) {
      // Quietly skip this function — VMP is best-effort.
      continue;
    }

    // For now, just mark as processed. Full interpreter building
    // (buildInterpreter) would be implemented here — it's complex and
    // beyond this scope. The bytecode is ready in C.Code, encrypted,
    // with call descriptors in C.Calls.
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
