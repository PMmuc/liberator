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

#include "Config.h"
#include "GlobalStruct.h"
#include "Profiler.hpp"
#include "TypeMatcher.h"

using namespace SVF;
using namespace SVFUtil;

std::unique_ptr<GlobalStruct> GlobalStruct::gspta;

/// GlobalStruct analysis
// Makes Points To Analysis
void GlobalStruct::analyze() {
  PROFILE_SCOPE("GlobalStruct::analyze Total Time");

  // I always keep a string variable
  std::string str;

  // let's do the base class analysis
  {
    PROFILE_SCOPE("GlobalStruct: FlowSensitive::analyze");
    FlowSensitive::analyze();
  }

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  Module *svfModule = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();

  std::map<std::string, std::set<const llvm::Function *>> fncs;

  // svfGlobalList returns functions and global variables
  SVFUtil::outs() << "--------------- Global Variables: -------------------\n";
  {
    PROFILE_SCOPE("GlobalStruct: Globals Processing");
    for (auto &g : svfModule->getGlobalList()) {
      int count = 0;
      if (SVFUtil::isa<llvm::Constant>(g)) {
        count++;
        SVFUtil::outs() << count << ". " << g.getName().str() << "\n";
        GLOBAL_LOG("{}. {}", count, g.getName().str());
        auto *llvm_constant = llvm::cast<llvm::Constant>(&g);
        get_function_pointers(llvm_constant, fncs);
      }
    }
  }

  // get callsites to function pointers
  // CallSite -> CallSiteId
  // (CallICFGNode* -> NodeId)
  SVFIR::CallSiteToFunPtrMap indirect_calls = pag->getIndirectCallsites();

  std::set<const CallICFGNode *> unresolved_calls;
  unsigned int tot_indirect_calls = 0;
  {
    PROFILE_SCOPE("GlobalStruct: Unresolved Calls");
    for (auto call : indirect_calls) {
      // Call Site of indirect call
      auto icfg_node = call.first;
      // id of callsite node
      auto node_id = call.second;
      // All Call Sites
      CallSiteSet target_set = pag->getIndCallSites(node_id);
      // the points to set that the indirect call can point to
      PointsTo x = this->getPts(node_id);
      // if x is empty the ptr points to nothing
      if (x.empty())
        unresolved_calls.insert(icfg_node);
      tot_indirect_calls++;
    }
  }

  auto ptacg = getCallGraph();

  SVFGEdgeSetTy svfgEdges;
  CallEdgeMap newEdges;

  {
    PROFILE_SCOPE("GlobalStruct: Resolve Indirect Calls");
    // Try to analyze why the points to set is empty for this indirect call.
    // Do a signature based matching where we match the signature of the call
    // site function, with the signature that we got from retrieving funcs from
    // all the constants in the program.
    for (const CallICFGNode *callsite : unresolved_calls) {
      // SVFUtil::outs() << cnode->toString() << "\n";
      auto llvm_inst = llvmModuleSet->getLLVMValue(callsite->getCaller());
      if (llvm_inst == nullptr)
        continue;

      // CallBase superclass of CallInst and InvokeInst
      auto llvm_cs = SVFUtil::dyn_cast<CallBase>(llvm_inst);
      if (llvm_cs == nullptr)
        continue;

      // llvm::raw_string_ostream(str) << *llvm_cs;
      // SVFUtil::outs() << str << "\n";
      FunctionType *fun_type = llvm_cs->getFunctionType();
      // This  will compute the hash of the signature of the function.
      auto fun_type_hash = TypeMatcher::compute_hash(fun_type);
      // auto fun_type_hash_str = TypeMatcher::compute_unique_string(fun_type);

      // llvm::raw_string_ostream(str) << *fun_type << "\n";
      // SVFUtil::outs() << str << "\n";
      // str = "";
      // SVFUtil::outs() << fun_type_hash << "\n";
      // SVFUtil::outs() << fun_type_hash_str << "\n";

      // auto fun_caller = cnode->getFun();
      // returns function containg the call
      const FunObjVar *fun_caller = callsite->getCaller();

      // FIXME: Maybe this needs a fix as i don't know if the changes I have
      // done are correct getCallsite retrives the llvm::CallBase instruction so
      // maybe the line 90 in the orginal code is redundant. I am assuming that
      // callBlockNode in the orginal code is just cnode.

      unsigned int x = 0;
      // SVFUtil::outs() << "callBlockNode: " << callBlockNode->toString() <<
      // "\n";
      for (auto f : fncs[fun_type_hash]) {
        auto fun_callee = llvmModuleSet->getFunObjVar(f);

        // if (ExtAPI::getExtAPI()->is_ext(fun_callee))
        //     SVFUtil::outs() << "it is external!\n";
        // else
        //     SVFUtil::outs() << "it is internal!\n";
        newEdges[callsite].insert(fun_callee);
        getIndCallMap()[callsite].insert(fun_callee);
        ptacg->addIndirectCallGraphEdge(callsite, fun_caller, fun_callee);
      }
      // SVFUtil::outs() << "connected to: " << x << "\n";
      // SVFUtil::outs() << "----\n";
    }
  }

  // SVFUtil::outs() << "[DEBUG] early stop\n";
  // exit(1);

  this->new_edges = newEdges;

  {
    PROFILE_SCOPE("GlobalStruct: Connect Edges");
    connectCallerAndCallee(newEdges, svfgEdges);
    updateConnectedNodes(svfgEdges);
  }
}

/// Initialize analysis
void GlobalStruct::initialize() { FlowSensitive::initialize(); }

/// Finalize analysis
void GlobalStruct::finalize() { FlowSensitive::finalize(); }

/**
 * @param in_value variable that is either a global variable, a constant
 * aggregate or a function.
 * @return map (md5(function_type) -> set Function*) where the set contains all
 * functions that in_value has as an initializer of a GlobalVariable or operand
 * of a ConstantAggregate.
 */
void GlobalStruct::get_function_pointers(
    const llvm::Value *in_value,
    std::map<std::string, std::set<const llvm::Function *>> &fncs) {

  SVFUtil::outs() << "--------- get_function_pointers("
                  << in_value->getName().str() << ")\n";

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
      SVFUtil::outs() << ca->getName().str() << "\n";
      for (unsigned int i = 0; i < ca->getNumOperands(); ++i) {
        auto op = ca->getOperand(i);
        working.push(op);
      }
    } else if (auto f = SVFUtil::dyn_cast<Function>(value)) {
      SVFUtil::outs() << "Found function " << f->getName().str() << "\n";
      auto fun_type = f->getFunctionType();
      auto k = TypeMatcher::compute_hash(fun_type);
      fncs[k].insert(f);
    }

    visited.insert(value);
  }
}
