#include "AccessTypeHandler.h"
#include "AccessType.h"
#include "DebugInfoParser.hpp"
#include "SVFIR/SVFIR.h"
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/LLVMUtil.h>
#include <SVF-LLVM/ObjTypeInference.h>
#include <SVFIR/SVFVariables.h>
#include <Util/Casting.h>
#include <Util/ExtAPI.h>
#include <Util/GeneralType.h>
#include <WPA/Andersen.h>
#include <cstdint>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include "Config.h"
#include "ValueMetadata.hpp"

/// Defined in GlobalStruct.cpp. Declared here rather than including
/// GlobalStruct.h, whose header-scope `using namespace SVF/SVFUtil` would make
/// llvm::dyn_cast and llvm::outs ambiguous throughout this file.
SVF::Andersen *global_struct_pta(SVF::SVFIR *pag);

namespace {

const Type *getPointedType(const Value *V, std::set<const Value *> &Vis) {
  if (!V)
    return nullptr;
  if (Vis.count(V))
    return nullptr;
  Vis.insert(V);

  const Value *Base = V->stripPointerCasts();
  if (Base != V)
    return getPointedType(Base, Vis);

  if (auto *AI = SVFUtil::dyn_cast<AllocaInst>(V)) {
    return AI->getAllocatedType();
  } else if (auto *GEP = SVFUtil::dyn_cast<GetElementPtrInst>(V)) {
    return GEP->getResultElementType();
  } else if (auto *GV = SVFUtil::dyn_cast<GlobalVariable>(V)) {
    return GV->getValueType();
  } else if (auto *PHI = SVFUtil::dyn_cast<PHINode>(V)) {
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
      if (auto *Res = getPointedType(PHI->getIncomingValue(i), Vis))
        return Res;
    }
  } else if (auto *SI = SVFUtil::dyn_cast<SelectInst>(V)) {
    if (auto *Res = getPointedType(SI->getTrueValue(), Vis))
      return Res;
    return getPointedType(SI->getFalseValue(), Vis);
  } else if (auto *LI = SVFUtil::dyn_cast<LoadInst>(V)) {
    // Attempt to resolve load from a global variable with an initializer
    if (auto *GV = SVFUtil::dyn_cast<GlobalVariable>(
            LI->getPointerOperand()->stripPointerCasts())) {
      if (GV->hasInitializer()) {
        return getPointedType(GV->getInitializer(), Vis);
      }
    }
  } else if (auto *I2P = SVFUtil::dyn_cast<IntToPtrInst>(V)) {
    // If integer comes from PtrToInt, unwrap it
    if (auto *P2I = SVFUtil::dyn_cast<PtrToIntInst>(I2P->getOperand(0))) {
      return getPointedType(P2I->getOperand(0), Vis);
    }
  }

  return nullptr;
}

llvm::DIDerivedType *as_indirection(llvm::DIType *t) {
  if (liberator::is_indirection_tag(t->getTag()))
    return dyn_cast_or_null<llvm::DIDerivedType>(t);

  return nullptr;
}

llvm::DIType *di_pointee(llvm::DIType *t) {
  auto *d = as_indirection(liberator::peel_di_qualifiers(t));
  return d ? liberator::peel_di_qualifiers(d->getBaseType()) : nullptr;
}
llvm::DIType *get_pointed_di_type(const llvm::Value *ptr,
                                  const llvm::DataLayout &dl,
                                  std::set<const llvm::Value *> &vis);

bool is_di_array(const llvm::DIType *t);

bool di_matches_llvm(llvm::DIType *di, llvm::Type *ty,
                     const llvm::DataLayout &dl) {
  if (!di || !ty || !ty->isSized())
    return false;
  if (llvm::isa<llvm::StructType>(ty) && !as_indirection(di)) {
    return liberator::compare_types(di, ty, dl);
  }
  return di->getSizeInBits() != 0 &&
         di->getSizeInBits() == dl.getTypeAllocSizeInBits(ty);
}
llvm::DIType *gep_pointed_di_type(const llvm::GEPOperator *gep,
                                  const llvm::DataLayout &dl,
                                  std::set<const llvm::Value *> &vis) {
  llvm::DIType *cur = get_pointed_di_type(gep->getPointerOperand(), dl, vis);

  if (!cur || is_di_array(cur) || gep->getNumIndices() == 0)
    return cur;

  // First index can be pointer arithmetic -> if !0 it is not a single object
  // -> therefore must be array or pointer arithmetic -> treated as array by
  // isAnArray
  auto *first = llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get());
  if (!first || !first->isZero())
    return nullptr;

  // for getelementptr i8, ptr %s, i64 8 the offset can not be mapped back to a
  // member because of i8
  if (!di_matches_llvm(cur, gep->getSourceElementType(), dl))
    return nullptr;

  // now walk the gep indices and types
  auto it = llvm::gep_type_begin(gep);
  ++it; // index 1 is handled above
  for (auto idx = gep->idx_begin() + 1; idx != gep->idx_end(); ++idx, ++it) {
    if (is_di_array(cur))
      return cur;
    llvm::StructType *st = it.getStructTypeOrNull();
    auto *ci = llvm::dyn_cast<llvm::ConstantInt>(idx->get());
    if (!st || !ci)
      return nullptr;
    cur = liberator::peel_di_qualifiers(liberator::next_di_field(
        cur, st, ci->getZExtValue(), it.getIndexedType(), dl));
    if (!cur)
      return nullptr;
  }
  return cur;
}

bool is_di_array(const llvm::DIType *t) {
  auto *c = llvm::dyn_cast_or_null<llvm::DICompositeType>(t);
  return c && c->getTag() == llvm::dwarf::DW_TAG_array_type;
}

llvm::DIType *get_pointed_di_type(const llvm::Value *ptr,
                                  const llvm::DataLayout &dl,
                                  std::set<const llvm::Value *> &vis) {
  if (!ptr || !vis.insert(ptr).second)
    return nullptr;

  if (llvm::isa<llvm::BitCastOperator>(ptr) ||
      llvm::isa<llvm::AddrSpaceCastOperator>(ptr))
    return get_pointed_di_type(llvm::cast<llvm::Operator>(ptr)->getOperand(0),
                               dl, vis);

  // local variables
  if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(ptr)) {
    for (auto rec : llvm::findDVRDeclares(const_cast<llvm::AllocaInst *>(ai))) {
      if (llvm::DILocalVariable *var = rec->getVariable())
        return liberator::peel_di_qualifiers(var->getType());
    }
    // no dwarf debug info could be found for this local var.
    return nullptr;
  }

  if (auto gv = llvm::dyn_cast<llvm::GlobalVariable>(ptr)) {
    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 1> global_vars;
    gv->getDebugInfo(global_vars);
    for (auto *global_expr : global_vars)
      if (llvm::DIGlobalVariable *var = global_expr->getVariable())
        return liberator::peel_di_qualifiers(var->getType());
    return nullptr;
  }

  if (auto *li = llvm::dyn_cast<llvm::LoadInst>(ptr))
    return di_pointee(get_pointed_di_type(li->getPointerOperand(), dl, vis));

  if (auto *arg = llvm::dyn_cast<llvm::Argument>(ptr))
    return di_pointee(liberator::restore_param_di_type(arg));

  if (auto *cb = llvm::dyn_cast<llvm::CallBase>(ptr))
    return di_pointee(liberator::restore_ret_di_type(cb->getCalledFunction()));
  if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(ptr))
    return gep_pointed_di_type(gep, dl, vis);
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(ptr)) {
    for (const llvm::Value *v : phi->incoming_values()) {
      // take the first value as type if it can be determined
      if (llvm::DIType *t = get_pointed_di_type(v, dl, vis))
        return t;
    }
    return nullptr;
  }
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(ptr)) {
    if (llvm::DIType *t = get_pointed_di_type(select->getTrueValue(), dl, vis))
      return t;
    return get_pointed_di_type(select->getFalseValue(), dl, vis);
  }

  return nullptr;
}

bool DIIsAnArray(const CallBase *c) {
  Module *m = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();
  const DataLayout &data_layout = m->getDataLayout();

  const Value *dest = c->getArgOperand(0);

  uint64_t obj_size = 0;

  std::set<const Value *> vis;

  if (llvm::DIType *obj = get_pointed_di_type(dest, data_layout, vis)) {
    if (is_di_array(obj))
      return true;
    obj_size = obj->getSizeInBits();
  } else {
    std::set<const Value *> vis_llvm;
    // if we lucky we can just check the llvm type to be an array
    if (const Type *t = getPointedType(dest, vis_llvm)) {
      if (t->isArrayTy())
        return true;
      obj_size = data_layout.getTypeStoreSize(const_cast<Type *>(t));
    }
  }
  auto *cpy = dyn_cast<ConstantInt>(c->getArgOperand(2));
  if (obj_size == 0 || !cpy)
    return true;

  return obj_size != cpy->getZExtValue();
}

bool isAnArray(const CallBase *c) {
  // I assume c is at least a memcpy-like function

  Module *m = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();
  const DataLayout &data_layout = m->getDataLayout();

  // outs() << "isAnArray?\n";
  // outs() << *c << "\n";

  bool obj_size_found = false;
  bool cpy_size_found = false;
  uint64_t obj_size = 0;
  uint64_t cpy_size = 0;

  // argument 0 should be the dst pointer
  Value *dest = c->getArgOperand(0);
  std::set<const Value *> Vis;
  // FIXME: this needs a fix
  const Type *base_tye = getPointedType(dest, Vis);

  if (base_tye) {
    obj_size = data_layout.getTypeStoreSize(const_cast<Type *>(base_tye));
    obj_size_found = true;
  }

  // argument 2 should be the count
  if (auto cs = dyn_cast<ConstantInt>(c->getArgOperand(2))) {
    cpy_size = cs->getZExtValue();
    // outs() << *copy_size << "\n";
    cpy_size_found = true;
  }

  if (obj_size_found && cpy_size_found && obj_size == cpy_size)
    return false;

  return true;
}

} // namespace
namespace liberator {

string_view di_tag_name(unsigned tag) {
  switch (tag) {
  case llvm::dwarf::DW_TAG_structure_type:
    return "DW_TAG_structure_type"sv;
  case llvm::dwarf::DW_TAG_union_type:
    return "DW_TAG_union_type"sv;
  case dwarf::DW_TAG_pointer_type:
    return "DW_TAG_pointer_type"sv;
  case dwarf::DW_TAG_array_type:
    return "DW_TAG_array_type"sv;
  case dwarf::DW_TAG_member:
    return "DW_TAG_member"sv;
  case dwarf::DW_TAG_base_type:
    return "DW_TAG_base_type"sv;
  case dwarf::DW_TAG_subroutine_type:
    return "DW_TAG_subroutine_type"sv;
  case dwarf::DW_TAG_typedef:
    return "DW_TAG_typedef"sv;
  case dwarf::DW_TAG_class_type:
    return "DW_TAG_class_type"sv;
  default:
    return "DW_TAG_unknown"sv;
  }
}

void addWrteToAllFields(ValueMetadata &mdata, AccessType atNode,
                        const ICFGNode *icfgNode) {

  auto type = atNode.get_di_type();
  if (type == nullptr)
    return;

  if (auto derv = dyn_cast<DIDerivedType>(type)) {
    if (is_indirection_tag(type->getTag())) {
      AccessType tmpAcNode = atNode;
      tmpAcNode.addField(-1);
      tmpAcNode.set_kind(AccessType::kind_e::write);
      mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
    }
  }

  if (auto st = SVFUtil::dyn_cast<DICompositeType>(type)) {
    for (int i = 0; i < st->getElements().size(); ++i) {
      AccessType atField = atNode;
      atField.set_kind(AccessType::kind_e::write);
      atField.addField(i);
      if (auto f = llvm::dyn_cast_or_null<DIType>(st->getElements()[i])) {
        atField.set_type(f);
      }
      mdata.get_access_type_set().insert(atField, icfgNode);
    }
  }

  // auto t = atNode.getType();
  /*if (auto pt = SVFUtil::dyn_cast<llvm::PointerType>(type)) {
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::write);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
  }

  llvm::Type *t;
  if (auto st = SVFUtil::dyn_cast<llvm::StructType>(t)) {
    for (int f = 0; f < st->getNumElements(); f++) {
      auto ft = st->getElementType(f);
      AccessType atField = atNode;
      atField.set_kind(AccessType::kind_e::write);
      atField.addField(f);
      atField.set_type(struct_to_di(st, ));
      mdata.get_access_type_set().insert(atField, icfgNode);
    }
  }*/
}

bool handlerDispatcher(ValueMetadata &mdata, const std::string &fun,
                       const ICFGNode *icfgNode, const CallICFGNode *cs,
                       int param_num, AccessType atNode, H_SCOPE h_scope,
                       liberator::Path *path) {
  std::string suffix = "*";
  bool found = false;
  for (auto f : accessTypeHandlers) {
    std::string fk = f.first;
    auto handler = f.second;

    int fk_size = fk.length() - suffix.length();
    if (fk.compare(fk_size, suffix.length(), suffix) == 0 &&
        fun.size() >= fk_size) {
      std::string fk_clean = fk.substr(0, fk_size);
      std::string fun_clean = fun.substr(0, fk_size);
      if (fk_clean == fun_clean) {
        handler(mdata, fun, icfgNode, cs, param_num, atNode, h_scope, path);
        found = true;
      }
    } else if (fun == f.first) {
      handler(mdata, fun, icfgNode, cs, param_num, atNode, h_scope, path);
      found = true;
    }
  }
  if (!found) {
    auto handler = handler_from_annotation(fun);
    if (handler != nullptr)
      handler(mdata, fun, icfgNode, cs, param_num, atNode, h_scope, path);
  }
  return true;
}

bool hasHandlerDispatcher(ValueMetadata *mdata, const std::string &fun,
                          const ICFGNode *icfgNode, const CallICFGNode *cs,
                          int param_num, H_SCOPE h_scope) {

  std::string suffix = "*";
  for (auto f : accessTypeHandlers) {
    std::string fk = f.first;
    auto handler = f.second;

    int fk_size = fk.length() - suffix.length();
    if (fk.compare(fk_size, suffix.length(), suffix) == 0 &&
        fun.size() >= fk_size) {
      std::string fk_clean = fk.substr(0, fk_size);
      std::string fun_clean = fun.substr(0, fk_size);
      if (fk_clean == fun_clean)
        return true;
    } else if (fun == f.first) {
      return true;
    }
  }
  auto handler = handler_from_annotation(fun);
  if (handler != nullptr)
    return true;
  return false;
}

bool malloc_handler(liberator::ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {
  if (param_num == -1 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    HANDLER_LOG("Function {} is a possible malloc return value", fun_name);
    return true;
  }
  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    HANDLER_LOG("Parameter of {} is a possible malloc size parameter",
                fun_name);
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    mdata.setMallocSize(true);
    return false;
  }

  return false;
}

bool free_handler(ValueMetadata &mdata, std::string fun_name,
                  const ICFGNode *icfgNode, const CallICFGNode *cs,
                  int param_num, AccessType atNode, H_SCOPE scope, Path *path) {

  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::del);
    mdata.get_access_type_set().insert(atNode, icfgNode);
  }

  return false;
}

bool open_handler(ValueMetadata &mdata, std::string fun_name,
                  const ICFGNode *icfgNode, const CallICFGNode *cs,
                  int param_num, AccessType atNode, H_SCOPE scope, Path *path) {

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    mdata.setIsFilePath(true);

    // outs() << "icfgNode: " << icfgNode->toString() << "\n";
    // outs() << "cs: " << cs->toString() << "\n";
    // exit(1);
  }

  return false;
}

/**
 * @param mdata - out ValueMetadata
 * @param fun_name - name of caller function
 * @param icfgNode - ICFGNode of caller function
 * @param cs - callsite
 * @param param_num - index number of parameter in the function call 1 = first
 * parameter, 2 = second ...
 * @param atNode - access type of current node
 */
bool memcpy_handler(ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {
  // memcpy(void*, void*, int count)
  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  // outs() << icfgNode->toString() << "\n";
  // exit(1);

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {

    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);

    auto llvm_val = llvmModuleSet->getLLVMValue(cs);
    auto c = SVFUtil::dyn_cast<CallBase>(llvm_val);
    mdata.setIsArray(isAnArray(c));
    // if (param_num == 1) {
    //  outs() << cs->getCallSite()->toString() << "\n";
    //
    Value *v = c->getArgOperand(2);
    mdata.add_len_source(v, path);
    // }
  }

  return false;
}

bool strlen_handler(ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  // outs() << "strlen_handler\n";

  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    HANDLER_LOG("Called strlen handler. Setting AccessType to read.");
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
    mdata.setIsArray(true);
    // outs() << "HOOK IT!\n";
  }

  // exit(1);

  return false;
}

bool strcpy_handler(ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
    mdata.setIsArray(true);
  }

  return false;
}

bool memset_handler(ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {
  // memset(void*, int offset, int size);
  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  // outs() << "memset_hander\n";
  // outs() << icfgNode->toString() << "\n";

  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {

    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
    auto llvm_val = llvmModuleSet->getLLVMValue(cs);
    auto c = SVFUtil::dyn_cast<CallBase>(llvm_val);
    // this change should allow, Liberator to differentiate between
    // calls where the destination pointer is an array or not
    mdata.setIsArray(isAnArray(c));

    auto i = SVFUtil::dyn_cast<CallBase>(llvm_val);
    // Get parameter n from memset call which is the number of bytes
    // to set the memory to.
    Value *v = i->getArgOperand(2);
    mdata.add_len_source(v, path);

    if (auto par_const = dyn_cast<ConstantInt>(i->getArgOperand(1))) {
      uint64_t actual_const = par_const->getZExtValue();
      if (actual_const == 0) {
        atNode.set_kind(AccessType::kind_e::del);
        mdata.get_access_type_set().insert(atNode, icfgNode);
      }
    }

    addWrteToAllFields(mdata, atNode, icfgNode);
  }

  return false;
}

bool calloc_handler(ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if (param_num == -1 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata.get_access_type_set().insert(atNode, icfgNode);

    addWrteToAllFields(mdata, atNode, icfgNode);

    return true;
  }
  if (param_num == 1 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    mdata.setMallocSize(true);
    return false;
  }

  return false;
}

// realloc(void*, size_t)
bool realloc_handler(ValueMetadata &mdata, std::string fun_name,
                     const ICFGNode *icfgNode, const CallICFGNode *cs,
                     int param_num, AccessType atNode, H_SCOPE scope,
                     Path *path) {
  // as in malloc returned pointer is a new object.
  if (param_num == -1 && scope & C_RETURN) {
    atNode.set_kind(AccessType::kind_e::create);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    return true;
  }
  // the old pointer invalidates the new pointer
  // therefore we log a read and delete
  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    atNode.set_kind(AccessType::kind_e::del);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    return false;
  }

  // the size
  if (param_num == 1 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    mdata.setMallocSize(true);
    return false;
  }
  return false;
}

bool posix_memalign_handler(ValueMetadata &mdata, std::string fun_name,
                            const ICFGNode *icfgNode, const CallICFGNode *cs,
                            int param_num, AccessType atNode, H_SCOPE scope,
                            Path *path) {

  if (param_num == 0 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata.get_access_type_set().insert(atNode, icfgNode);

    return true;
  }

  return false;
}

// FAILED ATTEMPT TO HANDLE ASPRINTF, TOO LAZY TO MAKE IT WORK
//  bool asprintf_handler(ValueMetadata *mdata, std::string fun_name,
//      const ICFGNode* icfgNode, const CallICFGNode* cs, int param_num,
//      AccessType atNode, H_SCOPE scope, Path* path) {

//     outs() << "I AM HOOKED!\n";
//     outs() << "param: " << param_num << "\n";
//     outs() << "scope: " << scope << "\n";

//     if (param_num == 0 && scope & C_RETURN) {
//         // no need to set field, empty field set is what I need
//         atNode.set_kind(AccessType::kind_e::create);
//         mdata->get_access_type_set().insert(atNode, icfgNode);

//         return true;
//     }

//     return false;
// }

bool strdup_handler(ValueMetadata &mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if (param_num == -1 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata.get_access_type_set().insert(atNode, icfgNode);

    addWrteToAllFields(mdata, atNode, icfgNode);

    return true;
  }

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
    mdata.setIsArray(true);
  }

  return false;
}

// This function resolves the annotations for function fun in the extapi.bc.
// With this information we can resolve the correct position of the allocSize
// parameter. All it does is find all "Arg" in the code.
static vector<int> annotated_size_args(const std::string &fun) {
  std::vector<int> args;

  const FunObjVar *f = LLVMUtil::getFunObjVar(fun);

  if (!f)
    return args;

  // this will get a comma separated list of Args that belong to AllocSize
  // annotation in extapi
  std::string annotations =
      ExtAPI::getExtAPI()->getExtFuncAnnotation(f, "AllocSize:");
  for (size_t arg = annotations.find("Arg"); arg != std::string::npos;
       arg = annotations.find("Arg", arg + 3)) {
    args.push_back(std::stoi(annotations.substr(arg + 3)));
  }
  return args;
}

// looks up if param_num can be found in the size_args array. If it does
// that parameter on that position is a MallocSize parameter.
static bool is_annotated_size_arg(const std::string &fun, int param_num) {
  auto size_args = annotated_size_args(fun);
  return std::find(size_args.begin(), size_args.end(), param_num) !=
         size_args.end();
}
static size_t is_annotated_old_ptr(const std::string &fun) {
  const FunObjVar *f = LLVMUtil::getFunObjVar(fun);

  if (!f)
    return false;
  std::string annotations =
      ExtAPI::getExtAPI()->getExtFuncAnnotation(f, "OldPtr:");
  auto pos = annotations.find("Arg");

  return pos != std::string::npos ? std::stoi(annotations.substr(pos + 3))
                                  : std::string::npos;
}
bool annotated_alloc_handler(ValueMetadata &mdata, std::string fun_name,
                             const ICFGNode *icfgNode, const CallICFGNode *cs,
                             int param_num, AccessType atNode, H_SCOPE scope,
                             Path *path) {
  if (param_num == -1 && scope & C_RETURN) {
    atNode.set_kind(AccessType::kind_e::create);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    return true;
  }
  if (atNode.get_num_fields() == 0 && scope & C_PARAM &&
      is_annotated_size_arg(fun_name, param_num)) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    mdata.setMallocSize(true);
  }

  return false;
}
bool annotated_realloc_handler(ValueMetadata &mdata, std::string fun_name,
                               const ICFGNode *icfgNode, const CallICFGNode *cs,
                               int param_num, AccessType atNode, H_SCOPE scope,
                               Path *path) {

  // does the same thing as annotated_alloc_handler
  bool r = annotated_alloc_handler(mdata, fun_name, icfgNode, cs, param_num,
                                   atNode, scope, path);

  if (atNode.get_num_fields() == 0 && scope & C_PARAM &&
      is_annotated_old_ptr(fun_name) == param_num) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(atNode, icfgNode);
    atNode.set_kind(AccessType::kind_e::del);
    mdata.get_access_type_set().insert(atNode, icfgNode);
  }

  return r;
}

static handler_t handler_from_annotation(const std::string &name) {
  const FunObjVar *f = LLVMUtil::getFunObjVar(name);
  if (!f)
    return nullptr;

  ExtAPI *ext = ExtAPI::getExtAPI();
  if (ext->is_realloc(f))
    return annotated_realloc_handler;
  if (ext->is_alloc(f))
    return annotated_alloc_handler;
  if (ext->is_memcpy(f))
    return memcpy_handler;
  if (ext->is_memset(f))
    return memset_handler;
  return nullptr;
}
/**
See explanation in AccessType.cpp function: predefined_access_type_dispatcher

You can define handlers for specific functions that will manually update the
access type set.
*/
AccessTypeHandlerMap accessTypeHandlers = {
    {"malloc", malloc_handler},
    {"free", free_handler},
    {"open", open_handler},
    {"open64", open_handler},
    {"fopen", open_handler},
    {"fopen64", open_handler},
    {"llvm.memcpy.*", memcpy_handler},
    {"strcpy", strcpy_handler},
    // {"strdup", &strcpy_handler},
    {"strlen", strlen_handler},
    {"llvm.memset.*", memset_handler},
    {"calloc", calloc_handler},
    {"posix_memalign", posix_memalign_handler},
    {"realloc", realloc_handler},
    // {"__asprintf_chk", &asprintf_handler},
    {"strdup", strdup_handler}};
} // namespace liberator
