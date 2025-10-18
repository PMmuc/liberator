#include "Graphs/ICFGNode.h"
#include "MemoryModel/PointsTo.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Casting.h"
#include "Util/Options.h"
#include "Util/SVFUtil.h"
#include "WPA/Andersen.h"
#include "WPA/WPAStat.h"

#include "GlobalStruct.h"
#include "TypeMatcher.h"

using namespace SVF;
using namespace SVFUtil;

std::unique_ptr<GlobalStruct> GlobalStruct::gspta;

/// GlobalStruct analysis
void GlobalStruct::analyze() {

  // I always keep a string variable
  std::string str;

  // let's do the base class analysis
  FlowSensitive::analyze();

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  Module *svfModule = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();
  Module *m = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();

  // TODO: Is this actually necessary? As modules has all functions
  std::map<std::string, std::set<const llvm::Function *>> fncs;

  for (auto &g : svfModule->getGlobalList()) {
    if (SVFUtil::isa<llvm::Constant>(g)) {
      // SVFUtil::outs() << g->toString() << "\n";
      auto *llvm_constant = llvm::cast<llvm::Constant>(&g);
      get_function_pointers(llvm_constant, fncs);
    }
  }

  SVFIR::CallSiteToFunPtrMap map = pag->getIndirectCallsites();

  std::set<const CallICFGNode *> unresolved_calls;
  unsigned int tot_indirect_calls = 0;
  for (auto el : map) {
    auto icfg_node = el.first;
    auto node_id = el.second;
    auto target_set = pag->getIndCallSites(node_id);
    auto x = this->getPts(node_id);
    if (x.empty())
      unresolved_calls.insert(icfg_node);
    tot_indirect_calls++;
  }

  auto ptacg = getCallGraph();

  SVFGEdgeSetTy svfgEdges;
  CallEdgeMap newEdges;

  // SVFUtil::outs() << "My unresolved calls:\n";
  // Actually resolving call targets
  for (const CallICFGNode *cnode : unresolved_calls) {
    // SVFUtil::outs() << cnode->toString() << "\n";
    auto llvm_inst = llvmModuleSet->getLLVMValue(cnode->getCaller());
    if (llvm_inst == nullptr)
      continue;

    // CallBase superclass of CallInst and InvokeInst
    auto llvm_cs = SVFUtil::dyn_cast<CallBase>(llvm_inst);
    if (llvm_cs == nullptr)
      continue;

    // llvm::raw_string_ostream(str) << *llvm_cs;
    // SVFUtil::outs() << str << "\n";

    auto fun_type = llvm_cs->getFunctionType();
    auto fun_type_hash = TypeMatcher::compute_hash(fun_type);
    auto fun_type_hash_str = TypeMatcher::compute_unique_string(fun_type);

    // llvm::raw_string_ostream(str) << *fun_type << "\n";
    // SVFUtil::outs() << str << "\n";
    // str = "";
    // SVFUtil::outs() << fun_type_hash << "\n";
    // SVFUtil::outs() << fun_type_hash_str << "\n";

    // auto fun_caller = cnode->getFun();
    const FunObjVar *fun_caller = cnode->getCaller();

    // FIXME: Maybe this needs a fix as i don't know if the changes I have done
    // are correct

    const CallBase *llvm_cs1 = SVFUtil::dyn_cast<CallBase>(cnode);

    unsigned int x = 0;
    // SVFUtil::outs() << "callBlockNode: " << callBlockNode->toString() <<
    // "\n";
    for (auto f : fncs[fun_type_hash]) {
      auto fun_callee = llvmModuleSet->getFunObjVar(f);

      // if (ExtAPI::getExtAPI()->is_ext(fun_callee))
      //     SVFUtil::outs() << "it is external!\n";
      // else
      //     SVFUtil::outs() << "it is internal!\n";
      newEdges[cnode].insert(fun_callee);
      getIndCallMap()[cnode].insert(fun_callee);
      ptacg->addIndirectCallGraphEdge(cnode, fun_caller, fun_callee);
    }
    // SVFUtil::outs() << "connected to: " << x << "\n";
    // SVFUtil::outs() << "----\n";
  }

  // SVFUtil::outs() << "[DEBUG] early stop\n";
  // exit(1);

  this->new_edges = newEdges;

  connectCallerAndCallee(newEdges, svfgEdges);
  updateConnectedNodes(svfgEdges);
}

/// Initialize analysis
void GlobalStruct::initialize() { FlowSensitive::initialize(); }

/// Finalize analysis
void GlobalStruct::finalize() { FlowSensitive::finalize(); }

void GlobalStruct::get_function_pointers(
    const llvm::Value *in_value,
    std::map<std::string, std::set<const llvm::Function *>> &fncs) {

  std::stack<const llvm::Value *> working;
  working.push(in_value);
  std::set<const llvm::Value *> visited;
  std::string str;

  while (!working.empty()) {
    auto value = working.top();
    working.pop();

    if (visited.find(value) != visited.end())
      continue;

    if (auto gv = SVFUtil::dyn_cast<GlobalVariable>(value)) {
      if (gv->hasInitializer()) {
        auto init = gv->getInitializer();
        working.push(init);
      }
    } else if (auto ca = SVFUtil::dyn_cast<ConstantAggregate>(value)) {
      for (unsigned int i = 0; i < ca->getNumOperands(); ++i) {
        auto op = ca->getOperand(i);
        working.push(op);
      }
    } else if (auto f = SVFUtil::dyn_cast<Function>(value)) {
      auto fun_type = f->getFunctionType();
      auto k = TypeMatcher::compute_hash(fun_type);
      fncs[k].insert(f);
    }

    visited.insert(value);
  }
}
