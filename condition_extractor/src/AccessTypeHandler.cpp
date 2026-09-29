#include "AccessTypeHandler.h"
#include "DebugInfoParser.hpp"
#include "Instrumentation.h"
#include "SVFIR/SVFIR.h"
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/ObjTypeInference.h>
#include <Util/Casting.h>
#include <Util/GeneralType.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include "Config.h"

/// Defined in GlobalStruct.cpp. Declared here rather than including
/// GlobalStruct.h, whose header-scope `using namespace SVF/SVFUtil` would make
/// llvm::dyn_cast and llvm::outs ambiguous throughout this file.
SVF::Andersen *global_struct_pta(SVF::SVFIR *pag);

namespace {

llvm::Type *deduce_type(llvm::Value *v) {
  llvm::Type *t = nullptr;
  if (auto AI = SVFUtil::dyn_cast<llvm::AllocaInst>(v)) {
    t = AI->getAllocatedType();
  } else if (auto *GEP = SVFUtil::dyn_cast<llvm::GetElementPtrInst>(v)) {
    t = GEP->getResultElementType();
  } else if (auto *ARG = SVFUtil::dyn_cast<llvm::Argument>(v)) {
    // TODO: there is no easy way to find what type a pointer argument points
    // to. We would need to find the uses of the function and determine the
    // actual parameter types.
    t = nullptr;
  } else if (const auto *GLOBAL = SVFUtil::dyn_cast<llvm::GlobalVariable>(v)) {
    t = GLOBAL->getValueType();
  } else if (auto *CI = SVFUtil::dyn_cast<llvm::CallInst>(v)) {
    llvm::Function *callee = CI->getCalledFunction();
    if (callee && !callee->isDeclaration()) {
      // TODO: This can also be a pointer, where we would need to search for
      // the underlying type again. So this is recursive
      t = callee->getType();
    }
  }
  return t;
}

llvm::Type *deduce_argument_type(llvm::Argument *arg) {
  llvm::Function *F = arg->getParent();
  unsigned arg_index = arg->getArgNo();

  for (auto user : F->users()) {
    if (auto CI = SVFUtil::dyn_cast<CallBase>(user)) {
      if (arg_index > CI->arg_size())
        continue;
      auto actual_arg = CI->getArgOperand(arg_index);
      llvm::Value *stripped = actual_arg->stripPointerCasts();

      return deduce_type(stripped);
    }
  }

  return nullptr;
}

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

/**
 * @AccessType at -
 */
void return_object_type(const AccessType &at, const ICFGNode *icfg_node,
                        const llvm::Type *legacy_t) {
  auto module_set = LLVMModuleSet::getLLVMModuleSet();
  auto *pag = SVF::SVFIR::getPAG();
  llvm::Module *mod = module_set->getMainLLVMModule();
  const llvm::DataLayout &dl = mod->getDataLayout();

  auto type_str = [](const llvm::Type *ty) {
    if (!ty)
      return std::string("<null>");
    if (const auto *st = llvm::dyn_cast<llvm::StructType>(ty))
      // st->getName() asserts on anonymous structs.
      if (!st->isLiteral() && st->hasName())
        return st->getName().str();
    std::string buf;
    llvm::raw_string_ostream os(buf);
    ty->print(os);
    return os.str();
  };

  auto llvm_bytes = [&dl](const llvm::Type *ty) -> uint64_t {
    if (!ty || !ty->isSized())
      return 0;

    return dl.getTypeAllocSize(const_cast<llvm::Type *>(ty)).getFixedValue();
  };

  const llvm::Type *dwarf_t = nullptr;
  llvm::DIType *raw_di = at.get_di_type();
  llvm::DIType *di = liberator::decay_di_type(raw_di);
  if (!di) {
    llvm::outs() << "[TYPE] no dwarf found. CAUSE: "
                 << (raw_di ? "decayed" : "no DIType on AccessType") << ")\n";
  } else {
    llvm::StringRef di_name = di->getName();
    llvm::outs() << "[TYPE] dwarf"
                 << (di_name.empty() ? llvm::StringRef("<anonymous>") : di_name)
                 << " tag=" << di_tag_name(di->getTag())
                 << " size=" << (di->getSizeInBits() / 8) << "Bytes\n";
    if (auto *comp = llvm::dyn_cast<llvm::DICompositeType>(di)) {
      unsigned idx = 0;
      for (llvm::DINode *e : comp->getElements()) {
        auto *member = llvm::dyn_cast_or_null<llvm::DIDerivedType>(e);
        if (!member || member->getTag() != llvm::dwarf::DW_TAG_member)
          continue;
        llvm::DIType *peeled = peel_di_qualifiers(member->getBaseType());
        llvm::StringRef peeled_name =
            peeled ? peeled->getName() : llvm::StringRef();
        llvm::outs() << "[TYPE] [" << ++idx << "] " << member->getName()
                     << " : "
                     << (peeled_name.empty() ? llvm::StringRef("<anonymous>")
                                             : peeled_name)
                     << " (" << (member->getSizeInBits() / 8)
                     << "Bytes at offset " << (member->getOffsetInBits() / 8)
                     << ")\n";
      }
    }

    dwarf_t = resolve_di_type_to_llvm(di, *mod);
    llvm::outs() << "[TYPE] -> llvm: " << type_str(dwarf_t) << " ("
                 << llvm_bytes(dwarf_t) << " Bytes\n";
  }

  // Fallback to SVF's type system
  SVF::Andersen *ander = global_struct_pta(pag);
  const llvm::Type *svf_t = nullptr;
  uint64_t svf_best = 0;
  unsigned nobj = 0;

  for (const ICFGNode *n : *icfg_node->getBB()) {
    const llvm::Value *v = module_set->getLLVMValue(n);
    const auto *ret_inst = llvm::dyn_cast_or_null<llvm::ReturnInst>(v);
    if (!ret_inst)
      continue;
    const llvm::Value *ret_val = ret_inst->getReturnValue();
    if (!ret_val || !module_set->hasValueNode(ret_val))
      continue;

    SVF::NodeID ret_node_id = module_set->getValueNode(ret_val);
    if (!pag->hasGNode(ret_node_id))
      continue;

    for (SVF::NodeID target_id : ander->getPts(ret_node_id)) {
      if (!pag->hasGNode(target_id))
        continue;
      SVF::SVFVar *obj = pag->getGNode(target_id);
      ++nobj;

      if (!module_set->hasLLVMValue(obj)) {
        llvm::outs() << "[TYPE] pts obj with id: " << target_id
                     << " has no llvm value - probably a blackhole/dummy\n";
        continue;
      }

      const llvm::Value *obj_value = module_set->getLLVMValue(obj);
      const char *kind = "other ";
      // NOTE: an object in SVF can only be a stack, global variable, function
      // or result of malloc (heap).
      if (llvm::isa<llvm::AllocaInst>(obj_value))
        kind = "alloca";
      else if (llvm::isa<llvm::GlobalVariable>(obj_value))
        kind = "global";
      else if (llvm::isa<Function>(obj_value)) {
        kind = "function";
      } else if (llvm::isa<llvm::CallBase>(obj_value))
        kind = "heap-call";

      const llvm::Type *obj_type =
          module_set->getTypeInference()->inferObjType(obj_value);
      llvm::outs() << "[TYPE] pts obj " << target_id << " " << kind
                   << " svf-inferred: " << type_str(obj_type) << " ("
                   << llvm_bytes(obj_type) << "Bytes\n";

      if (llvm_bytes(obj_type) >= svf_best) {
        svf_best = llvm_bytes(obj_type);
        svf_t = obj_type;
      }
    }
  }

  // chose between dwarf and svf_t type
  const llvm::Type *chosen = dwarf_t ? dwarf_t : svf_t;
  string_view via = dwarf_t ? "dwarf" : (svf_t ? "svf" : "none");
  llvm::outs() << "[TYPE] objects=" << nobj << " chosen=" << type_str(chosen)
               << " via=" << via << "\n";

  llvm::outs() << "[TYPE] legacy=" << type_str(legacy_t);
  if (!legacy_t)
    llvm::outs() << " (legacy FAILED)";
  else if (chosen == legacy_t)
    llvm::outs() << " (agree)";
  else
    llvm::outs() << " (DIFFER)";
  llvm::outs() << "\n";

  if (dwarf_t && svf_t && llvm_bytes(dwarf_t) != llvm_bytes(svf_t)) {
    llvm::outs() << "[TYPE] size mismatch dwarf=" << llvm_bytes(dwarf_t)
                 << "Bytes svf=" << llvm_bytes(svf_t)
                 << "Bytes - likely a cast through a handle type\n";
  }

  llvm::outs().flush();
}
void addWrteToAllFields(ValueMetadata &mdata, AccessType atNode,
                        const ICFGNode *icfgNode) {

  // outs() << "addWrteToAllFields\n";
  // // outs() << "type: " << *atNode.getType() << "\n";
  // outs() << "node: " << atNode.toString() << "\n";
  // if (atNode.getOriginalCastType() == nullptr)
  //     outs() << "PROBABLY not from a cast\n";
  // else {
  //     outs() << "ORIGINAL TYPE BEFORE CAST\n";
  //     outs() << *atNode.getOriginalCastType() << "\n";
  // }

  auto moduleSet = LLVMModuleSet::getLLVMModuleSet();
  auto pag = SVF::SVFIR::getPAG();

  // FIXME: We assume that the code will contain a return instruction. We should
  // not assume that

  // TODO: Find an efficient solution that finds the type of the underlying
  // using the use def chain
  const llvm::Type *t = nullptr;
  Value *base = nullptr;

  // Singleton: just returns the already-analysed instance. Ask GlobalStruct
  // rather than AndersenWaveDiff::createAndersenWaveDiff() - GlobalStruct is
  // itself an Andersen now, and createAndersenWaveDiff() would build and solve
  // a second, independent one.
  SVF::Andersen *ander = global_struct_pta(pag);

  // use points to analysis to find the type of the return type of the current
  // function
  for (const ICFGNode *icfg : *icfgNode->getBB()) {
    auto inst = moduleSet->getLLVMValue(icfg);
    auto ret_inst = SVFUtil::dyn_cast<llvm::ReturnInst>(inst);
    if (!ret_inst)
      continue;

    Value *ret_val = ret_inst->getReturnValue();

    // if ret void skip
    if (!ret_val)
      continue;
    auto ret_node_id = moduleSet->getValueNode(ret_val);
    if (pag->hasGNode(ret_node_id)) {
      auto node_id = pag->getGNode(ret_node_id);

      // Get all the objects that the return value can point to, in a NodeBS
      // format (list).
      const SVF::PointsTo &pts = ander->getPts(node_id->getId());

      // go through each object the points to set can point to.
      for (SVF::NodeID target_id : pts) {
        // can be address-taken or top-level variable
        if (!pag->hasGNode(target_id))
          continue;

        auto node = pag->getGNode(target_id);
        // not every SVFVar has a corresponding llvm value.
        if (!moduleSet->hasLLVMValue(node))
          continue;

        if (auto *target_obj = moduleSet->getLLVMValue(node)) {
          // actual allocation site
          if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(target_obj)) {
            t = AI->getAllocatedType();
            SVFUtil::outs() << "Points to Alloca of type: ";
            AI->getAllocatedType()->print(llvm::outs());
            SVFUtil::outs() << "\n";
          } else if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(target_obj)) {
            SVFUtil::outs() << "Points to Global of type: ";
            t = GV->getValueType();
            GV->getValueType()->print(llvm::outs());
            SVFUtil::outs() << "\n";
          } else if (auto *F = SVFUtil::dyn_cast<llvm::Function>(target_obj)) {

          } else if (auto *CI = SVFUtil::dyn_cast<llvm::CallBase>(target_obj)) {
            // TODO: Forward logic. Look for constructor for new, look for GEP
            // for malloc
            for (auto &U : CI->uses()) {
              for (auto it = U->uses().begin(); it != U->uses().end(); ++it) {
                if (auto *GEP =
                        SVFUtil::dyn_cast<llvm::GetElementPtrInst>(it->get())) {
                  // TODO: check if a use can also be an index in GEP
                  // instruction
                  t = GEP->getResultElementType();
                }
              }
            }
          }
        }
      }
    }

    // strip GEP that have only zero indices to get to the definition of the
    // base pointer.
    base = ret_val->stripPointerCasts();
    auto t = deduce_type(base);
    if (!t)
      deduce_type(ret_val);
  }

  // return_object_type(atNode, icfgNode, t);

  if (!t) {
    SVFUtil::errs()
        << "[ERROR] addWrteToAllFields: Type of Function: "
        << icfgNode->getFun()->getName()
        << " - could not be deduced. Maybe return type is an argument.\n";
    return;
  }
  /*
    if (atNode.getOriginalCastType() != nullptr) {
      t = atNode.getOriginalCastType();
    } else {
      t = atNode.getType();
    }
    */

  // auto t = atNode.getType();
  // FIXME: fix the opaque pointer problem
  if (auto pt = SVFUtil::dyn_cast<llvm::PointerType>(t)) {

    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::write);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
  }

  if (auto st = SVFUtil::dyn_cast<llvm::StructType>(t)) {
    for (int f = 0; f < st->getNumElements(); f++) {
      auto ft = st->getElementType(f);
      AccessType atField = atNode;
      atField.set_kind(AccessType::kind_e::write);
      atField.addField(f);
      // FIXME: add di type for that struct element
      atField.set_llvm_type(ft, nullptr);
      mdata.get_access_type_set().insert(atField, icfgNode);
    }
  }
}

bool handlerDispatcher(ValueMetadata &mdata, const std::string &fun,
                       const ICFGNode *icfgNode, const CallICFGNode *cs,
                       int param_num, AccessType atNode, H_SCOPE h_scope,
                       liberator::Path *path) {
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
        handler(mdata, fun, icfgNode, cs, param_num, atNode, h_scope, path);
    } else if (fun == f.first) {
      handler(mdata, fun, icfgNode, cs, param_num, atNode, h_scope, path);
    }
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

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  // outs() << "memset_hander\n";
  // outs() << icfgNode->toString() << "\n";

  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {

    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata.get_access_type_set().insert(tmpAcNode, icfgNode);
    mdata.setIsArray(true);

    auto llvm_val = llvmModuleSet->getLLVMValue(cs);
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
    // {"__asprintf_chk", &asprintf_handler},
    {"strdup", strdup_handler}};
} // namespace liberator
