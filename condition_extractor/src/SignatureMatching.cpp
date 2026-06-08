#include "SignatureMatching.hpp"

#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include <iterator>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/AMDGPUAddrSpace.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <string>

#include "Config.h"

using namespace llvm;

namespace {
/**
 * Removes typedefs, const, volatile qualifiers from the debug type information.
 */
const llvm::DIType *strip_qualifiers(const llvm::DIType *t) {
  while (auto *d = dyn_cast_or_null<DIDerivedType>(t)) {
    switch (d->getTag()) {
    case llvm::dwarf::DW_TAG_typedef:
    case llvm::dwarf::DW_TAG_const_type:
    case llvm::dwarf::DW_TAG_volatile_type:
    case llvm::dwarf::DW_TAG_atomic_type:
    case llvm::dwarf::DW_TAG_restrict_type:
      // get the next type
      t = d->getBaseType();
      continue;
    default:
      return t;
    }
  }
  return t;
}

/**
 * This will convert the debug information into a string in the following way:
 * No type void:
 *  "void"
 * Primitive Types:
 *   - float, int, short, ... -> "B[<size_in_bits>,
 * <encoding>]". The encoding can signed/unsigned/float.
 * Composite Types:
 *   - structure, class, union -> "ST[<struct name>]"
 *   - array
 *   - enumeration
 * Pointers:
 *   - int*, float* -> TP[B[32, signed]]
 *   - void (*)(int, float) ->
 * @param t - the type to convert to a string representation.
 */
std::string type_string(const llvm::DIType *t) {
  t = strip_qualifiers(t);
  if (!t) {
    return "void";
  }

  std::string out;
  raw_string_ostream os(out);
  t->printTree(os);
  // GLOBAL_LOG("Trying to resolve: {}\n", out);
  //  First handle primitive types.
  if (auto *b = dyn_cast<DIBasicType>(t)) {
    return "B[" + std::to_string(b->getSizeInBits()) + "," +
           std::to_string(b->getEncoding()) + "]";
  }
  // Second handle composite types.
  if (auto *c = dyn_cast<DICompositeType>(t)) {
    switch (c->getTag()) {
    case dwarf::DW_TAG_class_type:
    case dwarf::DW_TAG_union_type:
    case dwarf::DW_TAG_structure_type: {
      auto id = c->getIdentifier();
      return "ST[" + (id.empty() ? std::string("anonymous") : id.str()) + "]";
    }
    case dwarf::DW_TAG_array_type:
      return "AR[" + type_string(c->getBaseType()) + "]";
    case dwarf::DW_TAG_enumeration_type:
      return "E[" + std::to_string(c->getSizeInBits()) + "]";
    default:
      return "??";
    }
  }
  // Third handle pointer types.
  // Because pointer types point to other "types" we have a recursive
  // function call here.
  // Also for now for all other derived types like typedefs just skip.
  if (auto *p = dyn_cast<DIDerivedType>(t)) {
    std::string out;
    raw_string_ostream os(out);
    p->printTree(os);
    // GLOBAL_LOG("Trying to strip:\n{}\n", out);
    switch (p->getTag()) {
    case dwarf::DW_TAG_pointer_type:
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type: {
      const DIType *pointee = strip_qualifiers(p->getBaseType());
      // if it is a pointer to a function try to resolve the signature
      if (auto *sr = dyn_cast_or_null<DISubroutineType>(pointee)) {
        return "TP[" + signature(sr) + "]";
      }
      return "TP[" + type_string(p->getBaseType()) + "]";
    }
    default:
      // skip all other DIDerivedTypes
      return type_string(p->getBaseType());
    }
  }

  if (auto *sub = dyn_cast<DISubroutineType>(t)) {
    return signature(sub);
  }

  return "U??";
}

const llvm::DIType *strip_to_aggregate(const llvm::DIType *t) {
  t = strip_qualifiers(t);
  if (t) {
    if (auto *d = dyn_cast<DIDerivedType>(t))
      if (d->getTag() == llvm::dwarf::DW_TAG_pointer_type)
        t = strip_qualifiers(d->getBaseType());
  }
  return t;
}

const llvm::DIType *trace_callee_ditype(const llvm::Value *v,
                                        const llvm::Function *F,
                                        const slot_type_map_t &slots,
                                        std::set<const llvm::Value *> visited);

// Part of resolving a function pointer origin, if they are initialized through
// a gep instruction.
const llvm::DIType *
member_ditype_at_gep(const GetElementPtrInst *gep, const llvm::Function *F,
                     const slot_type_map_t &slots,
                     std::set<const llvm::Value *> &visited) {
  // here we have to walk the composite
  // we have getSourceOperandType(), getResultOperandType()
  const DIType *agg =
      trace_callee_ditype(gep->getPointerOperand(), F, slots, visited);
  // remove a pointer reference if needed to get to aggregate type.
  agg = strip_to_aggregate(agg);

  // Here we assume that we have found a struct/union/class type or an array
  // type.
  for (auto idx = gep->idx_begin() + 1; idx != gep->idx_end(); ++idx) {
    auto *stripped = strip_qualifiers(agg);
    if (!stripped)
      return nullptr;
    auto *comp = dyn_cast<DICompositeType>(stripped);

    if (!comp)
      return nullptr;

    if (comp->getTag() == llvm::dwarf::DW_TAG_array_type) {
      // int a[100] or int a[i]; -> base_type: int
      agg = comp->getBaseType();
      continue;
    }
    // struct/class/union:
    // get the index first. Only constant integers allowed.
    auto *ci = dyn_cast<ConstantInt>(idx->get());

    if (!ci)
      return nullptr;

    uint64_t el_idx = ci->getZExtValue();
    const DIType *n = nullptr;
    uint64_t k = 0;
    // In the DICompositeType try to find the member (field) that matches the
    // index of the GEP instruction.
    for (DINode *e : comp->getElements()) {
      if (!e)
        continue;
      auto *m = dyn_cast<DIDerivedType>(e);
      if (!m || m->getTag() != dwarf::DW_TAG_member) {
        continue;
      }
      if (k++ == el_idx) {
        // we found the correct element in the class
        // return
        n = m->getBaseType();
        break;
      }
    }
    // write the found element back to return type.
    agg = n;
  }
  return agg;
}

/**
 * Use static code analysis to track the origin of the function pointer.
 * If we can find it use the associated debug record to fully extract the
 * signature of the function call.
 * The algorithm uses a simple intraprocedural dfs backward scan to traverse the
 * control flow graph.
 */
const llvm::DIType *trace_callee_ditype(const llvm::Value *v,
                                        const llvm::Function *F,
                                        const slot_type_map_t &slots,
                                        std::set<const llvm::Value *> visited) {
  if (!v || !visited.insert(v).second)
    return nullptr;

  // clear all bitcasts and geps
  v = v->stripPointerCasts();

  // best case: found alloca or global var.
  // here we can read the type that is associated to the function pointer.
  if (auto it = slots.find(v); it != slots.end()) {
    std::string out;
    raw_string_ostream os(out);
    v->print(os);
    // GLOBAL_LOG("Found the allocation of the function pointer {}", out);
    return it->second;
  }

  // Case Load Instruction:
  // go to definition using use-def
  if (auto *load_in = dyn_cast<LoadInst>(v)) {
    return trace_callee_ditype(load_in->getPointerOperand(), F, slots, visited);
  }

  // Function pointer is a formal parameter, we can get its type directly from
  // the parent functions debug information.
  if (auto *arg = dyn_cast<Argument>(v)) {
    const Function *parent = arg->getParent();
    if (DISubprogram *SP = parent ? parent->getSubprogram() : nullptr) {
      DITypeRefArray ts = SP->getType()->getTypeArray();
      unsigned di = arg->getArgNo() + 1; // +1 because it starts at return type
      if (di < ts.size())
        return ts[di];
    }
    return nullptr;
  }

  // Function pointer is the result of a call -> get type directly from callee's
  // return type
  if (auto *call_in = dyn_cast<CallBase>(v)) {
    if (const Function *callee = call_in->getCalledFunction())
      if (DISubprogram *sub = callee->getSubprogram())
        return sub->getType()->getTypeArray()[0];
    return nullptr;
  }

  if (auto *gep_in = dyn_cast<GetElementPtrInst>(v)) {
    return member_ditype_at_gep(gep_in, F, slots, visited);
  }

  if (auto *phi = dyn_cast<PHINode>(v)) {
    for (const Value *in : phi->incoming_values()) {
      if (auto *t = trace_callee_ditype(in, F, slots, visited)) {
        return t;
      }
    }
    return nullptr;
  }

  if (auto *sel = dyn_cast<SelectInst>(v)) {
    if (auto *t = trace_callee_ditype(sel->getTrueValue(), F, slots, visited))
      return t;
    return trace_callee_ditype(sel->getFalseValue(), F, slots, visited);
  }

  return nullptr;
}
} // namespace

// O(n), where n is the number of SSA instructions
slot_type_map_t build_slot_map(const llvm::Module &M) {
  slot_type_map_t slot_map;
  for (const Function &F : M) {
    for (const Instruction &i : instructions(F)) {
      // Each local variable has a llvm.dbg.declare
      for (llvm::DbgRecord &DR : i.getDbgRecordRange()) {
        if (auto *DVR = llvm::dyn_cast<DbgVariableRecord>(&DR)) {
          if (DVR->isDbgDeclare()) {
            llvm::DILocalVariable *Var = DVR->getVariable();
            auto val = DVR->getAddress();
            std::string str;
            raw_string_ostream os(str);
            val->print(os);
            auto type = Var->getType();
            GLOBAL_LOG("Found var: {}", str);
            GLOBAL_LOG("Var Name: {}", Var->getName().str());
            GLOBAL_LOG("Var Type: {}\n\n", type->getName().str());
            slot_map.insert({val, type});
          } else if (DVR->isDbgValue()) {
            llvm::Value *Val = DVR->getValue();
          } else if (DVR->isDbgAssign()) {
          }
        }
      }
    }
  }

  for (const GlobalVariable &gv : M.globals()) {
    SmallVector<DIGlobalVariableExpression *, 1> gves;
    gv.getDebugInfo(gves);
    for (auto *gve : gves) {
      if (DIGlobalVariable *v = gve->getVariable()) {
        GLOBAL_LOG("Var Name: {}", v->getName().str());
        GLOBAL_LOG("Var Type: {}\n\n", v->getType()->getName().str());
        slot_map[&gv] = v->getType();
      }
    }
  }
  return slot_map;
}

const llvm::DISubroutineType *resolve_callbase(const llvm::CallBase *cs,
                                               const slot_type_map_t &slots) {
  std::set<const llvm::Value *> seen;
  // how do we get the debug info of a call instruction.
  // we have an indirect call -> so get the value of the called function.
  const DIType *t = trace_callee_ditype(cs->getCalledOperand(),
                                        cs->getFunction(), slots, seen);
  /*std::string out;
  raw_string_ostream os(out);
  t->printTree(os);
  GLOBAL_LOG("{}", out);*/

  // the returned type of the call instruction could be a pointer to the
  // function pointer
  // -> strip first
  t = strip_qualifiers(t);
  /*
  out.clear();
  t->printTree(os);
  GLOBAL_LOG("After stripping: {}", out);*/
  if (t) {
    if (auto *di = dyn_cast<DIDerivedType>(t)) {
      if (di->getTag() == dwarf::DW_TAG_pointer_type) {
        t = strip_qualifiers(di->getBaseType());
      }
    }
  }
  return dyn_cast_or_null<DISubroutineType>(t);
}

std::string signature(const llvm::DISubroutineType *sr) {
  if (!sr)
    return {};
  // this will get the signature types of the function in an array:
  // [return type, param1 type, param2 type]
  DITypeRefArray types = sr->getTypeArray();
  if (types.size() == 0)
    return {};

  std::string s = "FT[" + std::to_string(types.size()) + ", ";
  s += type_string(types[0]);

  /**
   * iterate through each parameter and get the type parameter string
   */
  for (unsigned i = 1; i < types.size(); ++i) {
    s += "," + type_string(types[i]);
  }
  s += "]";
  return s;
}

std::string signature(const llvm::Function *f) {
  // GLOBAL_LOG("Resolve function signature {}", f->getName().str());
  if (!f)
    return {};
  if (auto *sub = f->getSubprogram())
    return signature(sub->getType());

  return {};
}
