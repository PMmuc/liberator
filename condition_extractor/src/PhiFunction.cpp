#include "PhiFunction.h"
#include "SVF-LLVM/LLVMModule.h"
#include <llvm/Support/Casting.h>

void get_phi_function(Module *svfModule, ICFG *icfg, PHIFun &phi,
                      PHIFunInv &phi_inv) {
  // iterate all functions
  SVF::Module::const_iterator it = svfModule->begin();
  SVF::Module::const_iterator eit = svfModule->end();

  for (; it != eit; ++it) {
    // skip functions that don't have a declaration.
    // example external functions not defined
    // or intrinsics for example llvm debug intrinsics
    if (it->isDeclaration() || it->isIntrinsic())
      continue;

    auto fun =
        LLVMModuleSet::getLLVMModuleSet()->getFunObjVar(&it->getFunction());

    // when a function is declared in LLVM IR and is called this will still
    // return nullptr without definition.
    FunEntryICFGNode *fun_entry = icfg->getFunEntryICFGNode(fun);
    FunExitICFGNode *fun_exit = icfg->getFunExitICFGNode(fun);

    if (fun_entry == nullptr) {
      outs() << fun->toString() << " has no incoming edges.\n";
      continue;
    }

    for (auto e1 : fun_entry->getInEdges()) {
      auto call_edge = dyn_cast<CallCFGEdge>(e1);
      if (!call_edge)
        continue;
      const auto *inst_src_fun_entry = call_edge->getCallSite();

      for (auto e2 : fun_exit->getOutEdges()) {
        if (auto ret_edge = dyn_cast<RetCFGEdge>(e2)) {
          const auto *inst_src_fun_exit = ret_edge->getCallSite();

          if (inst_src_fun_entry == inst_src_fun_exit) {
            phi[call_edge] = ret_edge;
            phi_inv[ret_edge] = call_edge;
          }
        }
      }
    }
  }
}
