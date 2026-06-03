#include "SignatureMatching.hpp"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

using slot_type_map_t =
    std::unordered_map<const llvm::Value *, const llvm::DIType *>;

static slot_type_map_t build_slot_map(const llvm::Module &M) {
  for (const Function &F : M) {
    for (const Instruction &i : instructions(F)) {
    }
  }
}
// TODO: 1. Finish this -> getting type of callsites and functions and matching
// them.
// 2. compare the matches with the original liberator code
// 3.
void resolve_callbase(llvm::CallBase *cs) {
  // how do we get the debug info of an call instruction.
  // we have an indirect call -> so get the value of the called function.
  const Value *callee = cs->getCalledOperand();

  const Value *slot = callee;
  if (auto *li = dyn_cast<LoadInst>(callee)) {
    slot = li->getPointerOperand()->stripPointerCasts();
  }
}
