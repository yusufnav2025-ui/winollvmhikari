#include "Obfuscation/ConstantEncryption.h"
#include "Obfuscation/Utils.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include <random>
#include <vector>

using namespace llvm;

// Returns true if the instruction should be skipped entirely for constant
// encryption. These instructions require their operands to be compile-time
// constants for correctness or ABI reasons.
static bool shouldSkipInst(const Instruction &I) {
  if (isa<SwitchInst>(I))        return true; // case labels must be constants
  if (isa<PHINode>(I))           return true; // incoming values are positional
  if (isa<IntrinsicInst>(I))     return true; // LLVM intrinsics are sensitive
  if (isa<GetElementPtrInst>(I)) return true; // GEP indices control layout
  if (I.isAtomic())              return true; // atomic ordering must be const
  return false;
}

// Returns true if this particular ConstantInt operand should not be encrypted.
static bool shouldSkipConstant(const ConstantInt *CI) {
  unsigned W = CI->getBitWidth();
  // Only handle standard widths (8, 16, 32, 64).
  if (W != 8 && W != 16 && W != 32 && W != 64) return true;
  // Skip trivial values 0 and 1 — encrypting them adds noise but the XOR
  // instruction itself can be constant-folded back by a later optimizer pass,
  // so they're not worth the IR bloat.
  if (CI->isZero() || CI->isOne()) return true;
  return false;
}

PreservedAnalyses ConstantEncryption::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!enabled &&
        getFunctionAnnotation(&F).find("constenc") == std::string::npos)
      continue;

    // Phase 1: collect every (instruction, operand-index) pair that holds a
    // ConstantInt we want to encrypt.  Must collect before transforming to
    // avoid iterator invalidation.
    std::vector<std::pair<Instruction *, unsigned>> ToEncrypt;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (shouldSkipInst(I)) continue;
        for (unsigned i = 0; i < I.getNumOperands(); i++) {
          const ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(i));
          if (!CI || shouldSkipConstant(CI)) continue;
          ToEncrypt.emplace_back(&I, i);
        }
      }
    }

    if (ToEncrypt.empty()) continue;
    Changed = true;

    // Phase 2: replace each collected operand with:
    //   enc_const  = original XOR key   (compile-time constant, stored in binary)
    //   key_const  = key                (compile-time constant)
    //   xor_inst   = enc_const XOR key  (runtime instruction = original value)
    std::mt19937_64 RNG(std::random_device{}());

    for (auto &[Inst, OpIdx] : ToEncrypt) {
      ConstantInt *CI = cast<ConstantInt>(Inst->getOperand(OpIdx));
      Type *Ty       = CI->getType();
      unsigned W     = Ty->getIntegerBitWidth();

      APInt Key(W, RNG());          // random key, same width
      APInt Enc = CI->getValue() ^ Key; // encrypted value

      ConstantInt *EncC = cast<ConstantInt>(ConstantInt::get(Ty, Enc));
      ConstantInt *KeyC = cast<ConstantInt>(ConstantInt::get(Ty, Key));

      // Insert XOR just before the using instruction so the decrypted value
      // is available exactly when the instruction needs it.
      BinaryOperator *Xor =
          BinaryOperator::Create(BinaryOperator::Xor, EncC, KeyC, "", Inst);

      Inst->setOperand(OpIdx, Xor);
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
