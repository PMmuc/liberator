#include "PhiFunction.h"
#include "SVF-LLVM/LLVMModule.h"

void getPhiFunction(Module *svfModule, ICFG *icfg, PHIFun *phi,
                    PHIFunInv *phi_inv) {
  SVF::Module::const_iterator it = svfModule->begin();
  SVF::Module::const_iterator eit = svfModule->end();

  for (; it != eit; ++it) {
    if (it->isDeclaration() || it->isIntrinsic())
      continue;

    auto fun = LLVMModuleSet::getLLVMModuleSet()->getFunObjVar(&(*it));

    // outs() << fun->getName() << " [in DOM]\n";
    CallCFGEdge *call_edge;
    RetCFGEdge *ret_edge;
    ICFGNode::const_iterator it_fun_entry, eit_fun_entry;
    ICFGNode::const_iterator it_fun_exit, eit_fun_exit;

    // when a function is declared in LLVM IR and is called this will still
    // return nullptr without definition.
    FunEntryICFGNode *fun_entry = icfg->getFunEntryICFGNode(fun);
    FunExitICFGNode *fun_exit = icfg->getFunExitICFGNode(fun);

    if (fun_entry == nullptr) {
      outs() << fun->toString() << " has no incoming edges.\n";
      continue;
    }

    it_fun_entry = fun_entry->InEdgeBegin();
    eit_fun_entry = fun_entry->InEdgeEnd();

    for (; it_fun_entry != eit_fun_entry; ++it_fun_entry) {
      call_edge = (CallCFGEdge *)(*it_fun_entry);
      const auto *inst_src_fun_entry = call_edge->getCallSite();

      it_fun_exit = fun_exit->OutEdgeBegin();
      eit_fun_exit = fun_exit->OutEdgeEnd();
      for (; it_fun_exit != eit_fun_exit; ++it_fun_exit) {
        ret_edge = (RetCFGEdge *)(*it_fun_exit);
        const auto *inst_src_fun_exit = ret_edge->getCallSite();

        if (inst_src_fun_entry == inst_src_fun_exit) {
          phi->operator[](call_edge) = ret_edge;
          phi_inv->operator[](ret_edge) = call_edge;
        }
      }
    }
  }
}
