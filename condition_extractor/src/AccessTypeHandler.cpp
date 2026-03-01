#include "AccessTypeHandler.h"
#include "SVFIR/SVFIR.h"
#include <SVF-LLVM/LLVMModule.h>
#include <Util/Casting.h>
#include <Util/GeneralType.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

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

void addWrteToAllFields(ValueMetadata *mdata, AccessType atNode,
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

  // singleton: just return pointer
  SVF::Andersen *ander = SVF::AndersenWaveDiff::createAndersenWaveDiff(pag);

  // use points to analysis to find the type of the return type of the current
  // function
  for (const ICFGNode *icfg : *icfgNode->getBB()) {
    auto inst = moduleSet->getLLVMValue(icfg);
    auto ret_inst = SVFUtil::dyn_cast<llvm::ReturnInst>(inst);
    if (!ret_inst)
      continue;

    ret_inst->getType()->print(llvm::outs());
    Value *ret_val = ret_inst->getReturnValue();

    if (!ret_val)
      continue;

    auto svf_val = pag->getGNode(moduleSet->getValueNode(ret_val));

    if (pag->hasGNode(svf_val->getId())) {
      auto node_id = pag->getGNode(moduleSet->getValueNode(ret_val));

      const SVF::PointsTo &pts = ander->getPts(node_id->getId());

      for (SVF::NodeID target_id : pts) {
        if (!pag->hasGNode(target_id))
          continue;

        auto node = pag->getGNode(target_id);
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

    base = ret_val->stripPointerCasts();
    auto t = deduce_type(base);
    if (!t)
      deduce_type(ret_val);
  }

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
    mdata->getAccessTypeSet()->insert(tmpAcNode, icfgNode);
  }

  if (auto st = SVFUtil::dyn_cast<llvm::StructType>(t)) {
    outs() << st->getStructName() << "\n";
    for (int f = 0; f < st->getNumElements(); f++) {
      auto ft = st->getElementType(f);
      AccessType atField = atNode;
      atField.set_kind(AccessType::kind_e::write);
      atField.addField(f);
      atField.set_llvm_type(ft);
      mdata->getAccessTypeSet()->insert(atField, icfgNode);
    }
  }
}
bool malloc_handler(liberator::ValueMetadata *mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if (param_num == -1 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);
    return true;
  }
  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);
    mdata->setMallocSize(true);
    return false;
  }

  return false;
}

bool free_handler(ValueMetadata *mdata, std::string fun_name,
                  const ICFGNode *icfgNode, const CallICFGNode *cs,
                  int param_num, AccessType atNode, H_SCOPE scope, Path *path) {

  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::del);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);
  }

  return false;
}

bool open_handler(ValueMetadata *mdata, std::string fun_name,
                  const ICFGNode *icfgNode, const CallICFGNode *cs,
                  int param_num, AccessType atNode, H_SCOPE scope, Path *path) {

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);
    mdata->setIsFilePath(true);

    // outs() << "icfgNode: " << icfgNode->toString() << "\n";
    // outs() << "cs: " << cs->toString() << "\n";
    // exit(1);
  }

  return false;
}

bool memcpy_handler(ValueMetadata *mdata, std::string fun_name,
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
    mdata->getAccessTypeSet()->insert(tmpAcNode, icfgNode);

    auto llvm_val = llvmModuleSet->getLLVMValue(cs);
    auto c = SVFUtil::dyn_cast<CallBase>(llvm_val);
    mdata->setIsArray(isAnArray(c));
    // if (param_num == 1) {
    //  outs() << cs->getCallSite()->toString() << "\n";
    auto i = SVFUtil::dyn_cast<CallBase>(llvm_val);
    Value *v = i->getArgOperand(2);
    mdata->addFunParam(v, path);
    // }
  }

  return false;
}

bool strlen_handler(ValueMetadata *mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  // outs() << "strlen_handler\n";

  if (param_num == 0 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata->getAccessTypeSet()->insert(tmpAcNode, icfgNode);
    mdata->setIsArray(true);
    // outs() << "HOOK IT!\n";
  }

  // exit(1);

  return false;
}

bool strcpy_handler(ValueMetadata *mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata->getAccessTypeSet()->insert(tmpAcNode, icfgNode);
    mdata->setIsArray(true);
  }

  return false;
}

bool memset_hander(ValueMetadata *mdata, std::string fun_name,
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
    mdata->getAccessTypeSet()->insert(tmpAcNode, icfgNode);
    mdata->setIsArray(true);

    auto llvm_val = llvmModuleSet->getLLVMValue(cs);
    auto i = SVFUtil::dyn_cast<CallBase>(llvm_val);
    Value *v = i->getArgOperand(2);
    mdata->addFunParam(v, path);

    if (auto par_const = dyn_cast<ConstantInt>(i->getArgOperand(1))) {
      uint64_t actual_const = par_const->getZExtValue();
      if (actual_const == 0) {
        atNode.set_kind(AccessType::kind_e::del);
        mdata->getAccessTypeSet()->insert(atNode, icfgNode);
      }
    }

    addWrteToAllFields(mdata, atNode, icfgNode);
  }

  return false;
}

bool calloc_handler(ValueMetadata *mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if (param_num == -1 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);

    addWrteToAllFields(mdata, atNode, icfgNode);

    return true;
  }
  if (param_num == 1 && atNode.get_num_fields() == 0 && scope & C_PARAM) {
    atNode.set_kind(AccessType::kind_e::read);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);
    mdata->setMallocSize(true);
    return false;
  }

  return false;
}

bool posix_memalign_handler(ValueMetadata *mdata, std::string fun_name,
                            const ICFGNode *icfgNode, const CallICFGNode *cs,
                            int param_num, AccessType atNode, H_SCOPE scope,
                            Path *path) {

  if (param_num == 0 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);

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
//         mdata->getAccessTypeSet()->insert(atNode, icfgNode);

//         return true;
//     }

//     return false;
// }

bool strdup_handler(ValueMetadata *mdata, std::string fun_name,
                    const ICFGNode *icfgNode, const CallICFGNode *cs,
                    int param_num, AccessType atNode, H_SCOPE scope,
                    Path *path) {

  if (param_num == -1 && scope & C_RETURN) {
    // no need to set field, empty field set is what I need
    atNode.set_kind(AccessType::kind_e::create);
    mdata->getAccessTypeSet()->insert(atNode, icfgNode);

    addWrteToAllFields(mdata, atNode, icfgNode);

    return true;
  }

  if ((param_num == 0 || param_num == 1) && atNode.get_num_fields() == 0 &&
      scope & C_PARAM) {
    AccessType tmpAcNode = atNode;
    tmpAcNode.addField(-1);
    tmpAcNode.set_kind(AccessType::kind_e::read);
    mdata->getAccessTypeSet()->insert(tmpAcNode, icfgNode);
    mdata->setIsArray(true);
  }

  return false;
}

/**
See explanation in AccessType.cpp function: predefined_access_type_dispatcher

You can define handlers for specific functions that will manually update the
access type set.
*/
AccessTypeHandlerMap accessTypeHandlers = {
    {"malloc", &malloc_handler},
    {"free", &free_handler},
    {"open", &open_handler},
    {"open64", &open_handler},
    {"fopen", &open_handler},
    {"fopen64", &open_handler},
    {"llvm.memcpy.*", &memcpy_handler},
    {"strcpy", &strcpy_handler},
    // {"strdup", &strcpy_handler},
    {"strlen", &strlen_handler},
    {"llvm.memset.*", &memset_hander},
    {"calloc", &calloc_handler},
    {"posix_memalign", &posix_memalign_handler},
    // {"__asprintf_chk", &asprintf_handler},
    {"strdup", &strdup_handler}};
} // namespace liberator
