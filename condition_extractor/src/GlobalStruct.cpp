#include "Graphs/ICFGNode.h"
#include "MemoryModel/PointsTo.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/ObjTypeInference.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Casting.h"
#include "Util/Options.h"
#include "Util/SVFUtil.h"
#include "WPA/Andersen.h"
#include "WPA/WPAStat.h"

#include "Config.h"
#include "GlobalStruct.h"
#include "Profiler.hpp"
#include "SignatureMatching.hpp"
#include "TypeMatcher.h"
#include <ios>
#include <llvm/IR/Argument.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/AMDGPUAddrSpace.h>
#include <llvm/Support/raw_ostream.h>

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

  // md5(function_signature) -> {possible target functions}
  std::map<std::string, std::set<const llvm::Function *>> fncs;

  // svfGlobalList returns functions and global variables
  GLOBAL_LOG("--------------- global variables found: -------------------\n");
  {
    PROFILE_SCOPE("GlobalStruct: Globals Processing");
    for (auto &g : svfModule->globals()) {
      int count = 0;
      if (SVFUtil::isa<llvm::Constant>(g)) {
        count++;
        GLOBAL_LOG("\t{}. {}", count, g.getName().str());
        auto *llvm_constant = llvm::cast<llvm::Constant>(&g);
        get_function_pointers(llvm_constant, fncs);
      }
    }
  }

#ifndef NDEBUG
  for (auto p : fncs) {
    GLOBAL_LOG("Found {} possible targets for signature: {}\n", p.second.size(),
               p.first);
  }
#endif

  // get callsites to function pointers
  // CallSite -> CallSiteId
  // (CallICFGNode* -> NodeId)
  SVFIR::CallSiteToFunPtrMap indirect_calls = pag->getIndirectCallsites();

  std::set<const CallICFGNode *> unresolved_calls;
  unsigned int tot_indirect_calls = 0;
  {
    PROFILE_SCOPE("GlobalStruct: Unresolved Calls");
    // Iterate all indirect calls
    for (auto call : indirect_calls) {
      //  Call Site of indirect call
      auto icfg_node = call.first;
      // id of callsite node in the PAG
      auto node_id = call.second;
      // All indirect callsites for one function.
      CallSiteSet target_set = pag->getIndCallSites(node_id);
      // the points to set that the indirect call can point to
      PointsTo x = this->getPts(node_id);
      // if x is empty the ptr points to nothing
      if (x.empty())
        unresolved_calls.insert(icfg_node);
      tot_indirect_calls++;
    }
  }

  // FIXME: remove this code when not needed anymore
  fstream outstream("unresolved_function_ptrs.txt", ios_base::out);
  outstream << "Found: " << unresolved_calls.size() << " unresolved callsites."
            << endl;

  if (outstream.is_open()) {
    for (auto call : unresolved_calls) {
      outstream << "In function: " << call->getCaller()->getName() << ": "
                << call->toString() << endl;
    }
    outstream.flush();
    outstream.close();
  }
  // FIXME: remove to here -----------------------------------------
#ifndef NDEBUG
  GLOBAL_LOG("------------- UNRESOLVED CALLS -------------");
  for (auto call : unresolved_calls) {
    GLOBAL_LOG("Indirect call: {} found in function: {}\n", call->toString(),
               call->getCaller()->getName());
  }
#endif

  auto ptacg = getCallGraph();

  SVFGEdgeSetTy svfgEdges;
  CallEdgeMap newEdges;

  auto slot_map = build_slot_map(*llvmModuleSet->getMainLLVMModule());

  {
    PROFILE_SCOPE("GlobalStruct: Resolve Indirect Calls");
    // Try to analyze why the points to set is empty for this indirect call.
    // Do a signature based matching where we match the signature of the call
    // site function, with the signature that we got from retrieving funcs from
    // all the constants in the program.
    GLOBAL_LOG(
        "Liberator found {} unresolved call(s). (Calls without "
        "points-to set)\nTry matching signature of callsite function with",
        unresolved_calls.size());
    for (const CallICFGNode *callsite : unresolved_calls) {
      // SVFUtil::outs() << cnode->toString() << "\n";
      auto llvm_inst = llvmModuleSet->getLLVMValue(callsite);
      if (llvm_inst == nullptr)
        continue;

      // CallBase superclass of CallInst and InvokeInst
      auto llvm_cs = SVFUtil::dyn_cast<CallBase>(llvm_inst);
      if (llvm_cs == nullptr)
        continue;

      // auto type_inference = llvmModuleSet->getTypeInference();

      // llvm::raw_string_ostream(str) << *llvm_cs;
      // SVFUtil::outs() << str << "\n";
      // FunctionType *fun_type = llvm_cs->getFunctionType();
      /*int arg_num = 1;
      for (const auto &arg : llvm_cs->args()) {
        if (llvm::Value *v = dyn_cast<Value>(arg)) {
          string out, type_str;
          if (v->getType()->isPointerTy()) {
            // const Type *type = type_inference->inferObjType(v);
            // raw_string_ostream os1(type_str);
            // type->print(os1);

          } else {
            raw_string_ostream os1(type_str);
            v->getType()->print(os1);
          }
          raw_string_ostream os(out);
          v->print(os);
          GLOBAL_LOG("Found arg {}. {} with type: {}", arg_num, out, type_str);
        }
        ++arg_num;
      }*/

      // This  will compute the hash of the signature of the function.
      // auto fun_type_hash = TypeMatcher::compute_hash(fun_type);
      // auto fun_type_hash_str = TypeMatcher::compute_unique_string(fun_type);

      // llvm::raw_string_ostream(str) << *fun_type << "\n";
      // SVFUtil::outs() << str << "\n";
      // str = "";
      // SVFUtil::outs() << fun_type_hash << "\n";
      // SVFUtil::outs() << fun_type_hash_str << "\n";

      // auto fun_caller = cnode->getFun();
      // returns function containg the call
      const FunObjVar *fun_caller = callsite->getCaller();

      // resolve the callsite to it's function type.
      std::string lookup_key;
      if (const auto *sr = resolve_callbase(llvm_cs, slot_map)) {
        lookup_key = "DW " + signature(sr);
        string out;
        raw_string_ostream os(out);
        llvm_cs->print(os);
        GLOBAL_LOG("Callsite {} has function pointer type {}\n", out,
                   lookup_key);
      }
      /*else {
        lookup_key =
            "OP " + TypeMatcher::compute_hash(llvm_cs->getFunctionType());
        GLOBAL_LOG("Couldn't find type of function pointer. Falling back to {}",
                   lookup_key);
      }*/

      unsigned int x = 0;
      // SVFUtil::outs() << "callBlockNode: " << callBlockNode->toString() <<
      // "\n";
      for (auto f : fncs[lookup_key]) {
        auto fun_callee = llvmModuleSet->getFunObjVar(f);

        std::string out;
        raw_string_ostream os(out);
        llvm_cs->print(os);
        GLOBAL_LOG("The function: {} called at {} got matched with key: {}\n",
                   f->getName().str(), out, lookup_key);

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
  fstream file("function_pointers.txt", std::ios::out);

  if (file.is_open()) {
    file << "Found " << fncs.size() << " target functions.\n";
    file << "Found " << unresolved_calls.size() << " unresolved calls.\n";

    int i;
    file << "----------- FOUND TARGET FUNCTIONS -------------\n";
    GLOBAL_LOG("----------- FOUND TARGET FUNCTIONS -------------\n");
    for (auto fset : fncs) {
      file << "For signature " << fset.first << ": {\n";
      GLOBAL_LOG("For signature {}: [", fset.first);
      i = 1;
      for (auto f : fset.second) {
        GLOBAL_LOG("{}. {}", i, f->getName().str());
        file << i++ << ". " << f->getName().str() << "\n";
      }
      GLOBAL_LOG("]\n");
      file << "]\n";
    }

    file << endl;

    file << "----------- UNRESOLVED CALLS -------------\n";
    GLOBAL_LOG("----------- UNRESOLVED CALLS -------------\n");
    i = 1;
    for (auto uc : unresolved_calls) {
      GLOBAL_LOG("{}. {}", i, uc->toString());
      file << i++ << ". " << uc->toString() << "\n";
    }

    file << "---------- ADDED EDGES ----------\n";
    GLOBAL_LOG("----------- ADDED EDGES -------------\n");
    i = 1;
    for (auto edge : newEdges) {
      file << "For call: " << edge.first->toString() << " -> {\n";
      GLOBAL_LOG("For call: {} -> [", edge.first->toString());
      for (auto target : edge.second) {
        GLOBAL_LOG("{}", target->getName());
        file << target->getName() << ",\n";
      }
      file << "}\n";
      GLOBAL_LOG("]\n");
    }

    file.flush();
    file.close();
  }

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
 * @param in_value variable that is either a global variable, a constant,
 * aggregate or a function.
 * @return map (md5(function_type) -> set Function*) where the set contains all
 * functions that in_value has as an initializer of a GlobalVariable or operand
 * of a ConstantAggregate.
 */
void GlobalStruct::get_function_pointers(
    const llvm::Value *in_value,
    std::map<std::string, std::set<const llvm::Function *>> &fncs) {

  GLOBAL_LOG("------------ Retrieve function pointer for {} -------------\n",
             in_value->getName().str());

  std::stack<const llvm::Value *> working;
  std::set<const llvm::Value *> visited;
  working.push(in_value);

  while (!working.empty()) {
    auto value = working.top();
    working.pop();

    if (visited.find(value) != visited.end())
      continue;

    if (auto gv = SVFUtil::dyn_cast<GlobalVariable>(value)) {
      if (!gv->isDeclaration()) {
        GLOBAL_LOG("Global Variable: {} has an initializer.",
                   gv->getName().str());
        auto init = gv->getInitializer();
        working.push(init);
      } else {
        GLOBAL_LOG("Global Variable: {} does NOT have an initializer.",
                   gv->getName().str());
      }
    } else if (auto ca = SVFUtil::dyn_cast<ConstantAggregate>(value)) {
      if (ca->getType()->isStructTy() &&
          !SVFUtil::cast<StructType>(ca->getType())->isLiteral()) {
        GLOBAL_LOG("Constant aggregate struct: {}",
                   ca->getType()->getStructName().str());
      } else {
        GLOBAL_LOG("Constant aggregate (non-struct or literal)");
      }
      for (unsigned int i = 0; i < ca->getNumOperands(); ++i) {
        auto op = ca->getOperand(i);
        working.push(op);
      }
    } else if (auto f = SVFUtil::dyn_cast<Function>(value)) {
      GLOBAL_LOG("Found function: {}\n", f->getName().str());
      // add the function with found signature to the set

      if (std::string dw = signature(f); !dw.empty()) {
        GLOBAL_LOG("Found function signature: {}", dw);
        fncs["DW " + dw].insert(f);
      }
      fncs["OP " + TypeMatcher::compute_hash(f->getFunctionType())].insert(f);
    } else if (auto cs = SVFUtil::dyn_cast<CallBase>(value)) {
      GLOBAL_LOG("Here we found an external call to {}.",
                 cs->getCalledFunction()->getName().str());
    } else if (auto store_in = SVFUtil::dyn_cast<StoreInst>(value)) {
      GLOBAL_LOG("Here we found a store.");
    }

    visited.insert(value);
  }
}
