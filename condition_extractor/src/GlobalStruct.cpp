#include <memory>

#include "LibfuzzUtil.h"
#include "MemoryModel/PointsTo.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFModule.h"
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
  SVFModule *svfModule = LLVMModuleSet::getLLVMModuleSet()->getSVFModule();
  Module *m = LLVMModuleSet::getLLVMModuleSet()->getMainLLVMModule();

  std::map<std::string, std::set<const llvm::Function *>> fncs;

  for (auto g : svfModule->getGlobalSet()) {
    if (SVFUtil::isa<SVFConstant>(g)) {
      // SVFUtil::outs() << g->toString() << "\n";
      auto llvm_val = llvmModuleSet->getLLVMValue(g);
      get_function_pointers(llvm_val, &fncs);
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

  // auto ptacg = getPTACallGraph();
  auto ptacg = ptaCallGraph;

  SVFGEdgeSetTy svfgEdges;
  CallEdgeMap newEdges;

  // SVFUtil::outs() << "My unresolved calls:\n";
  // Actually resolving call targets
  for (auto cnode : unresolved_calls) {
    // SVFUtil::outs() << cnode->toString() << "\n";
    auto cs = cnode->getCallSite();
    auto llvm_inst = llvmModuleSet->getLLVMValue(cs);
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
    auto fun_caller = cnode->getCaller();

    const CallICFGNode *callBlockNode =
        pag->getICFG()->getCallICFGNode(cnode->getCallSite());

    unsigned int x = 0;
    // SVFUtil::outs() << "callBlockNode: " << callBlockNode->toString() <<
    // "\n";
    for (auto f : fncs[fun_type_hash]) {
      auto fun_callee = llvmModuleSet->getSVFFunction(f);

      // if (ExtAPI::getExtAPI()->is_ext(fun_callee))
      //     SVFUtil::outs() << "it is external!\n";
      // else
      //     SVFUtil::outs() << "it is internal!\n";
      newEdges[callBlockNode].insert(fun_callee);
      getIndCallMap()[callBlockNode].insert(fun_callee);
      ptacg->addIndirectCallGraphEdge(callBlockNode, fun_caller, fun_callee);
    }
    // SVFUtil::outs() << "connected to: " << x << "\n";
    // SVFUtil::outs() << "----\n";
  }

  // SVFUtil::outs() << "[DEBUG] early stop\n";
  // exit(1);

  this->new_edges = newEdges;

  std::string file_path = libfuzz::output_file_dir + "/" +
                          libfuzz::current_target_name +
                          "_function_pointers.txt";
  std::fstream file(file_path, std::ios_base::out);
  if (file.is_open()) {
    file << "Found " << fncs.size() << " target functions.\n";
    file << "Found " << unresolved_calls.size() << " unresolved calls.\n";

    int i;
    file << "----------- FOUND TARGET FUNCTIONS -------------\n";
    for (auto fset : fncs) {
      file << "For signature " << fset.first << ": {\n";
      i = 1;
      for (auto f : fset.second) {
        file << i++ << ". " << f->getName().str() << "\n";
      }
      file << "}\n";
    }
    file << endl;
    file << "----------- UNRESOLVED CALLS -------------\n";
    i = 1;
    for (auto uc : unresolved_calls) {
      file << i++ << ". " << uc->toString() << "\n";
    }
    file << "---------- ADDED EDGES ----------\n";
    i = 1;
    for (auto edge : newEdges) {
      file << "For call: " << edge.first->toString() << " -> {\n";
      for (auto target : edge.second) {
        file << target->getName() << ",\n";
      }
      file << "}\n";
    }
    file.flush();
    file.close();
  }

  connectCallerAndCallee(newEdges, svfgEdges);
  updateConnectedNodes(svfgEdges);
}

/// Initialize analysis
void GlobalStruct::initialize() { FlowSensitive::initialize(); }

/// Finalize analysis
void GlobalStruct::finalize() { FlowSensitive::finalize(); }

void GlobalStruct::get_function_pointers(
    const llvm::Value *in_value,
    std::map<std::string, std::set<const llvm::Function *>> *fncs) {

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
      (*fncs)[k].insert(f);
    }

    visited.insert(value);
  }
}
