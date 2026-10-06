#include "IntraAnalysis.hpp"
#include "AccessTypeHandler.h"
#include "Config.h"
#include "DebugInfoParser.hpp"
#include "ValueMetadata.hpp"
#include <SVF-LLVM/LLVMModule.h>
#include <SVFIR/SVFIR.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Casting.h>

#include "Instrumentation.h"

namespace liberator {

static size_t di_break_samples = 0;

std::string getType(llvm::Type *t) {
  auto &dl =
      LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule()->getDataLayout();
  auto size_bytes = std::to_string(dl.getTypeStoreSize(t));
  switch (t->getTypeID()) {
  case Type::IntegerTyID:
    return "Integer " + size_bytes;
  case Type::FloatTyID:
    return "Float " + size_bytes;
  case Type::DoubleTyID:
    return "Double " + size_bytes;
  case Type::HalfTyID:
    return "Half " + size_bytes;
  case Type::PointerTyID:
    return "Ptr " + size_bytes;
  case Type::StructTyID: {
    // Literal (anonymous) structs match StructTyID too, and getStructName()
    // asserts on them.
    const auto *st = llvm::cast<StructType>(t);
    std::string name = st->isLiteral() ? "<literal>" : st->getName().str();
    return "Struct " + name + " with size " + size_bytes;
  }
  case Type::ArrayTyID:
    return "Array " + size_bytes;
  }

  return "";
}

bool handleGep(const VFGNode *vNode, AccessType &acNode, AccessTypeSet &ats,
               ValueMetadata &mdata, Path &p) {
  auto llvmModuleSet = SVF::LLVMModuleSet::getLLVMModuleSet();
  if (auto gep_stmt = SVFUtil::dyn_cast<GepVFGNode>(vNode)) {

    auto llvm_inst = llvmModuleSet->getLLVMValue(gep_stmt->getValue());

    if (auto gep_inst = SVFUtil::dyn_cast<GetElementPtrInst>(llvm_inst)) {

      //
      /* %struct.Point = type { i32, i32 }
       * %gep = getelementptr %struct.Point
       * , ptr %points, i32 0, i64 5, i32 1
       *
       * TODO: this is a problem because getPointerOperand() is returning
       * just 'ptr' in LLVM 16+.
       */

      auto sType = gep_inst->getSourceElementType();
      auto dType = gep_inst->getResultElementType();

      auto printType = [](llvm::Type *t, std::string_view msg) {
        if (t->isStructTy()) {
          GEP_LOG("{} {}\n", msg, getType(t));
        } else if (t->isArrayTy()) {
          GEP_LOG("{} {}[]", msg, getType(t));
        }
      };

      printType(sType, "Source type:");
      printType(dType, "Result type:");

      auto ditype = peel_di_qualifiers(acNode.get_di_type());

      // Both operands need guarding: peel_di_qualifiers returns null when the
      // path carries no DWARF type, and Type::getStructName() is a hard
      // cast<StructType> that asserts for e.g. `getelementptr i32, ptr %p,
      // i64 %i` - the shape an array parameter such as int *p produces.
      // isStructTy() alone is not enough: literal (anonymous) structs are
      // struct-typed but getName() asserts on them.
      // Note these arguments are evaluated even when the GEPHandler log tag
      // is disabled, so the guard matters regardless of -log.
      GEP_LOG("ditype: {} source type: {}\n",
              ditype ? ditype->getName().str() : std::string("<no di type>"),
              !sType->isStructTy() ? std::string("<non-struct>")
              : llvm::cast<StructType>(sType)->isLiteral()
                  ? std::string("<literal struct>")
                  : sType->getStructName().str());
      const llvm::DataLayout &dl = gep_inst->getDataLayout();
      llvm::DIType *path_di = acNode.get_di_type();
      bool types_matching = false;

      if (path_di) {
        types_matching = compare_types(path_di, sType, dl);
        GEP_LOG("di: {} matches gep source type: {}\n",
                path_di->getName().str(), types_matching);
        // if the path tracks a void* try to recover the actual type
        // using the struct. If that is not possible we skip the path completly,
        // otherwise flags that are set in the field access would be attributed
        // to the base object's summary. For example s->f1, if f1 is a
        // mallocSize would save IsMallocSize in s -> wrong summary for s.
        if (!types_matching && !decay_di_type(path_di)) {
          if (auto *llvm_st = dyn_cast<StructType>(sType)) {
            if (llvm::DIType *rec =
                    struct_to_di(llvm_st, *gep_inst->getModule())) {
              acNode.set_type(rec);
              types_matching = compare_types(rec, sType, dl);
              di_instr.gep_void_recovered++;
            }
          }
          types_matching = true;
        }
        // if the type can not be recovered we skip the path.
        if (!types_matching && isa<StructType>(sType)) {
          di_instr.gep_untyped_skip++;
          return true;
        }
      } else {
        // No debug info, or the DI chain broke on an earlier GEP.
        // No way to tell which field this GEP is selecting for structs.
        // Following the field addresses would track the fields on the base
        // type. Therefore we stop tracking the path here.
        di_instr.gep_no_di++;
        if (gep_inst->hasAllConstantIndices() && gep_inst->getNumIndices() > 1)
          return true;
        // We track arrays accesses if the types are matching.
        types_matching = true;
      }
      // GEP is ptr, but path has type information and is a double pointer -> we
      // still want to track array accesses
      if (!types_matching && sType->isPointerTy()) {
        auto tmp_path =
            dyn_cast_or_null<llvm::DIDerivedType>(peel_di_qualifiers(path_di));
        if (tmp_path &&
            tmp_path->getTag() == llvm::dwarf::DW_TAG_pointer_type) {
          auto elem = dyn_cast_or_null<llvm::DIDerivedType>(
              peel_di_qualifiers(tmp_path->getBaseType()));
          if (elem && elem->getTag() == llvm::dwarf::DW_TAG_pointer_type)
            types_matching = true;
        }
      }

      if (types_matching) {
        // SVFUtil::isa<PointerType>(dType)) {
        // Handle struct and array accesses
        // has all Constant Indices requries
        if (gep_inst->hasAllConstantIndices() &&
            gep_inst->getNumIndices() > 1) {
          // Note: getNumIndices returns the number of indices after the
          // base pointer. EXAMPLE: ... i32 0, i32 1, i32 2 will return 3
          int pos = 1;
          // this will point to the first index operand in the GEP instruction
          auto gep_type_it = llvm::gep_type_begin(gep_inst);
          for (; pos <= gep_inst->getNumIndices(); pos++, ++gep_type_it) {
            // pos == 1 will return i32 0
            // dereference of pointer
            if (pos == 1) {
              // TODO: What happens when pos == 1 but getOperand(pos) is not
              // zero?
              // -> then we have an array of pointers
              // If == 0 this just says that it gets dereferenced.
              AccessType tmpAcNode = acNode;
              GEP_LOG("Adding a temporary AccessType to the AccessTypeSet.\n");
              GEP_LOG("The AccessTypeSet now has {} entries.\n",
                      ats.size() + 1);
              tmpAcNode.addField(-1);
              tmpAcNode.set_kind(AccessType::kind_e::read);
              ats.insert(tmpAcNode, vNode->getICFGNode());
            } else {
              // This will be useful for nested structs and recursive structs.
              // We can just iterate over each type in the struct that gets
              // accessed by the GEP.
              // Example: struct A {int f1; }; struct B { A* f1; }; B b;
              // b->f1->f1. GetElementPtr %struct.B, ptr %base, i64 0, i32 0,
              // i32 0 ---> this will return {%struct.B, %struct.A},
              // {%struct.A, i32} Or for arrays in structs struct Inner { int
              // f1; }; struct Outer { struct Inner f1[5]; }; so if you want
              // to access Outer o; o->f1[2]->f1; llvm would create
              // GetElementPtr %struct.Outer, ptr %base, i64 0, i32 0, i32 2,
              // i32 0
              llvm::Type *container_ty = gep_type_it.getStructTypeOrNull();
              llvm::Type *step_result_type = gep_type_it.getIndexedType();

              uint64_t idx = dyn_cast<ConstantInt>(gep_inst->getOperand(pos))
                                 ->getZExtValue();

              // Use DWARF to get the next type that we need to track in the
              // path.
              llvm::DIType *prev_di = acNode.get_di_type();
              llvm::DIType *next_di = next_di_field(prev_di, container_ty, idx,
                                                    step_result_type, dl);
              if (next_di)
                di_instr.ok++;
              else if (!prev_di)
                di_instr.poisoned++;
              else {
                di_instr.fresh_break++;
                if (di_break_samples < 30) {
                  di_break_samples++;
                  llvm::DIType *decayed = decay_di_type(prev_di);
                  auto *st = dyn_cast_or_null<llvm::StructType>(container_ty);
                  llvm::outs()
                      << "[DIBREAK] on='"
                      << (decayed ? decayed->getName() : " decayed null")
                      << "' di_tag=" << (decayed ? decayed->getTag() : 0)
                      << " idx=" << idx << " llvm_container="
                      << (st ? (st->isLiteral() ? "literal" : st->getName())
                             : "null/array")
                      << " n_elems=" << (st ? st->getNumElements() : 0) << "\n";
                  llvm::outs().flush();
                }
              }
              GEP_LOG("Adding field to AccessType {}\n", idx);
              acNode.addField(idx);
              // note that next_di can be nullptr
              // keep the old di type if we could not deduce the new one
              acNode.set_type(next_di);
              const std::string *visit_key = di_key(decay_di_type(prev_di));
              int visit_idx = container_ty ? static_cast<int>(idx) : -1;
              if (visit_key)
                acNode.add_visited_type(visit_key, visit_idx);
            }
          }
        } else if (acNode.get_num_fields() ==
                   0) { // only check for arrays on base pointers not on
                        // fields of structs.
          auto d = gep_inst->getOperand(1);
          bool is_array = false;
          if (!SVFUtil::isa<ConstantInt>(d)) { // p[i] with a variable i
                                               // save i in mdata.
            // case A: %q = getelementptr i8, ptr %p, i64 %n -> p[i]
            //  and %q = getelementptr i8, ptr %p, i64 %n, i64, i64 3 ->
            //  p[i]->f3
            //  but  p->f3[i] is not tracked
            is_array = true;
            // this is for setLenDependency
            mdata.addIndex(d);           // record the index value
            mdata.add_len_source(d, &p); // record the context as well
          } else if (gep_inst->getNumIndices() == 1) // pointer arithmetic
          {
            // case B: %r = getelementptr i8, ptr %p, i64 16
            is_array = true;
            mdata.addIndex(gep_inst); // record the gep instruction itself
          }
          if (is_array) {
            GEP_LOG("Setting is_array to true for {}\n", vNode->toString());
            mdata.setIsArray(true);
          }
        } else {
          // if the gep is somekind of other pointer arithmetic we skip.
          return true;
        }
      }
    } // end GEP Processing
    else {
      // when GEP converts to an constant expression, we can skip it
      return true;
    }
  }

  return false;
}

void handleActualParam(const VFGNode *vNode, AccessType &acNode,
                       ValueMetadata &mdata, Path &p) {
  // outs() << "****: " << vNode->toString() << "\n";
  auto actual_param = SVFUtil::dyn_cast<ActualParmSVFGNode>(vNode);
  // 1 - get icfg node from vNode
  auto icfg_node = actual_param->getICFGNode();
  // 2 - check it is a call inst
  auto cs = actual_param->getCallSite();
  auto param = actual_param->getParam();
  // 3 - check it is in the newedge relation
  auto m = ValueMetadata::myCallEdgeMap_inst;
  if (m.find(cs) != m.end() && param != nullptr) {
    // 4 - for each target check handling
    for (auto t : m[cs]) {
      APARM_LOG("Found indirect call to function {}\n", t->getName());
      int n_param = 0;
      // determine index of the our parameter
      for (auto p : cs->getActualParms()) {
        if (p == param)
          break;
        n_param++;
      }

      handlerDispatcher(mdata, t->getName(), vNode->getICFGNode(), cs, n_param,
                        acNode, C_PARAM, &p);
    }
  }
}

local_result_t transfer_function(const VFGNode *vNode, AccessType acNode,
                                 ValueMetadata &mdata, Path &p) {
  auto *llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
  AccessTypeSet &ats = mdata.get_access_type_set();
  bool skip_node = false;
  // Processing of the node
  switch (vNode->getNodeKind()) {
  case VFGNode::VFGNodeK::Load: {
    acNode.set_kind(AccessType::kind_e::read);
    ats.insert(acNode, vNode->getICFGNode());
  } break;
  case VFGNode::VFGNodeK::Store: {
    auto *prevValue = p.getPrevValue();

    auto llvm_val = llvm_module_set->getLLVMValue(vNode->getValue());

    if (prevValue != nullptr && SVFUtil::isa<StoreInst>(llvm_val)) {
      auto inst = SVFUtil::cast<StoreInst>(llvm_val);

      // the whole reason of prevValue is to distinguish between if a
      // parameter is used in a store to write to it or to read from it.
      if (inst->getPointerOperand() == prevValue)
        acNode.set_kind(AccessType::kind_e::write);
      else if (inst->getValueOperand() == prevValue)
        acNode.set_kind(AccessType::kind_e::read);

      ats.insert(acNode, vNode->getICFGNode());

      if (vNode->hasIncomingEdge()) {
        for (auto it : vNode->getInEdges()) {
          if (auto node = SVFUtil::dyn_cast<AddrVFGNode>(it->getSrcNode())) {
            if (auto call = SVFUtil::dyn_cast<llvm::CallInst>(
                    llvm_module_set->getLLVMValue(node->getValue()))) {
              llvm::Function *callee = call->getCalledFunction();
              if (callee && callee->getName() == "malloc") {
                // no need to set field, empty field set is what I need
                acNode.set_kind(AccessType::kind_e::create);
                ats.insert(acNode, vNode->getICFGNode());
                LOCAL_LOG("Function {} has a malloc return value",
                          vNode->getICFGNode()->getFun()->getName());
              }
            }
          }
        }
      }
    }
  } break;
  case SVF::VFGNode::VFGNodeK::Gep:
    (acNode.get_di_type() ? di_instr.gep_bu_ok : di_instr.gep_bu_null)++;
    skip_node = handleGep(vNode, acNode, ats, mdata, p);
    break;
  case VFGNode::VFGNodeK::Copy: {
    if (auto stmt_vfg_node =
            SVFUtil::dyn_cast<StmtVFGNode>(vNode->getValue())) {
      // this is for ptrtoint instructions.
      LOCAL_LOG("{}\n", vNode->toString());
      auto inst = llvm_module_set->getLLVMValue(stmt_vfg_node);
      // auto inst = SVFUtil::dyn_cast<GetElementPtrInst>(lllvm_inst);

      // auto inst = SVFUtil::dyn_cast<Instruction>(vNode->getValue());

      acNode.set_kind(AccessType::kind_e::read);
      ats.insert(acNode, vNode->getICFGNode());

      if (auto bitcastinst = SVFUtil::dyn_cast<BitCastInst>(inst)) {
        auto dst_typ = bitcastinst->getDestTy();
        auto src_typ = bitcastinst->getSrcTy();

        // if (acNode.getNumFields() != 0 &&
        //     TypeMatcher::compare_types(src_typ, acNode.getType())) {

        // outs() << "src_typ " << *src_typ << "\n";
        // outs() << "acNode.getType() " << *acNode.getType() << "\n";

        // if (TypeMatcher::compare_types(src_typ, acNode.getType())) {
        //     // I want the node the original type after the cast this
        //     // may turn out useful for mem* api operations since
        //     // they tend to cast to i8* before being invoked
        //     acNode.setOriginalCastType(acNode.getType());
        //     acNode.setType(dst_typ);
        //     ats->insert(acNode, vNode->getICFGNode());
        // }
        // else {
        //     skipNode = true;
        // }

        // if (dst_typ != seek_type && dst_typ != i8ptr_typ) {
        //     skipNode = true;
        // }
      }
    }
  } break;
  case SVF::VFGNode::VFGNodeK::Cmp:
    acNode.set_kind(AccessType::kind_e::read);
    ats.insert(acNode, vNode->getICFGNode());
    break;
  case SVF::VFGNode::VFGNodeK::BinaryOp:
    acNode.set_kind(AccessType::kind_e::read);
    ats.insert(acNode, vNode->getICFGNode());
    break;
  case SVF::VFGNode::VFGNodeK::AParm:
    handleActualParam(vNode, acNode, mdata, p);
    break;
  } // end switch statement

  const llvm::Value *prev_value =
      vNode->getValue() == nullptr
          ? nullptr
          : llvm_module_set->getLLVMValue(vNode->getValue());

  return local_result_t{std::move(acNode), prev_value, skip_node};
}

} // namespace liberator
