#include "AccessType.h"
#include "AccessTypeHandler.h"
#include "AccessTypeIO.h"
#include "Config.h"
#include "ValueMetadata.hpp"

#include "Graphs/ICFGNode.h"
#include "Graphs/IRGraph.h"
#include "Graphs/SVFG.h"
#include "SVF-LLVM/BasicTypes.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/ObjTypeInference.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Casting.h"
#include "Util/SVFUtil.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"
#include <Graphs/VFGNode.h>
#include <MSSA/SVFGBuilder.h>
#include <Util/GeneralType.h>
#include <Util/WorkList.h>
#include <chrono>
#include <iostream>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/TimeProfiler.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#define MAX_STACKSIZE 20

namespace {
bool leadsToBitCastOfType(const VFGNode *vn, Type *targetType) {
  // TODO: follow def-use only inside the function!
  // return true of n leads to a bitcast whose destination is of type targetType

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  outs() << "Analyze: " << vn->toString() << "\n";

  // for (auto vn: n->getVFGNodes()) {

  std::set<const VFGNode *> visited;
  std::vector<const VFGNode *> worklist;

  worklist.push_back(vn);

  outs() << "vn: " << vn->toString() << "\n";

  while (!worklist.empty()) {

    auto n = worklist.back();
    worklist.pop_back();

    if (visited.find(n) != visited.end())
      continue;

    if (SVFUtil::isa<GepVFGNode>(n))
      continue;

    // only intra-exploration
    if (SVFUtil::isa<InterPHIVFGNode>(n))
      continue;

    // NOTE: alternative way to remain in function, to use as alternative
    // auto fun = n->getFun();
    // // I am visiting a non-visted function, skip it
    // if (vf->find(fun) == vf->end())
    //     continue;

    if (n->getNodeKind() == VFGNode::VFGNodeK::Copy &&
        SVFUtil::isa<StmtVFGNode>(n)) {

      auto stmt_vfg_node = SVFUtil::dyn_cast<StmtVFGNode>(n);
      auto inst = llvmModuleSet->getLLVMValue(stmt_vfg_node->getValue());

      if (auto bitcastinst = SVFUtil::dyn_cast<BitCastInst>(inst)) {
        auto dst_typ = bitcastinst->getDestTy();
        auto src_typ = bitcastinst->getSrcTy();

        if (dst_typ == targetType) {
          // const Instruction **AAA;
          // *bitcast_ret = bitcastinst;
          return true;
        }
      }
    }

    for (auto in : n->getOutEdges()) {
      auto pn = in->getDstNode();
      worklist.push_back(pn);
    }

    visited.insert(n);
  }

  // }

  return false;
}
// NOT EXPOSED FUNCTIONS -- THESE FUNCTIONS ARE MEANT FOR ONLY INTERNAL USAGE!
bool areConnected(const VFGNode *, const VFGNode *);
bool areConnectedCtx(const VFGNode *, const VFGNode *, liberator::Path *);
std::set<const VFGNode *> getDefinitionSet(const VFGNode *);
std::set<const VFGNode *> getDefinitionSetCtx(const VFGNode *,
                                              liberator::Path *);
/**
 * @return: the node that has no predecessors.
 */
std::set<const VFGNode *> getDefinitionSetForRet(const VFGNode *,
                                                 std::set<const FunObjVar *> &);
// bool leadsToBitCastOfType(const ICFGNode*,Type*,const Instruction**);
bool leadsToBitCastOfType(const VFGNode *, Type *);
bool areCompatible(FunctionType *, FunctionType *);
// NOT EXPOSED FUNCTIONS -- END!

bool areConnectedCtx(const VFGNode *a, const VFGNode *b,
                     liberator::Path *path) {

  std::set<const VFGNode *> defA = getDefinitionSet(a);
  std::set<const VFGNode *> defB = getDefinitionSetCtx(b, path);
  std::set<const VFGNode *> intersection;

  // outs() << "DefA:" << a->toString() << "\n";
  // for (auto e: defA)
  //     outs() << e->toString() << "\n";
  // outs() << "DefB:" << b->toString() << "\n";
  // for (auto e: defB)
  //     outs() << e->toString() << "\n";

  std::set_intersection(defA.begin(), defA.end(), defB.begin(), defB.end(),
                        std::inserter(intersection, intersection.begin()));

  return !intersection.empty();
}

bool areConnected(const VFGNode *a, const VFGNode *b) {

  std::set<const VFGNode *> defA = getDefinitionSet(a);
  std::set<const VFGNode *> defB = getDefinitionSet(b);
  std::set<const VFGNode *> intersection;

  // outs() << "DefA:" << a->toString() << "\n";
  // for (auto e: defA)
  //     outs() << e->toString() << "\n";
  // outs() << "DefB:" << b->toString() << "\n";
  // for (auto e: defB)
  //     outs() << e->toString() << "\n";

  std::set_intersection(defA.begin(), defA.end(), defB.begin(), defB.end(),
                        std::inserter(intersection, intersection.begin()));

  return !intersection.empty();
}
std::set<const VFGNode *>
getDefinitionSetForRet(const VFGNode *n, std::set<const FunObjVar *> &vf) {
  std::set<const VFGNode *> definitions;
  std::set<const VFGNode *> visited;
  std::vector<const VFGNode *> worklist;

  worklist.push_back(n);
  while (!worklist.empty()) {
    auto n = worklist.back();
    worklist.pop_back();
    if (visited.find(n) != visited.end())
      continue;
    if (SVFUtil::isa<GepVFGNode>(n))
      continue;
    auto fun = n->getFun();
    // I am visiting a non-visted function, skip it
    if (vf.find(fun) == vf.end())
      continue;
    int n_parents = 0;
    for (auto in : n->getInEdges()) {
      auto pn = in->getSrcNode();
      worklist.push_back(pn);
      n_parents++;
    }
    // Maybe select some classes, e.g., alloca, param
    if (n_parents == 0)
      definitions.insert(n);
    visited.insert(n);
  }

  return definitions;
}

std::set<const VFGNode *> getDefinitionSetCtx(const VFGNode *n,
                                              liberator::Path *path_in) {

  std::set<const VFGNode *> definitions;

  std::set<const VFGNode *> visited;
  std::vector<const VFGNode *> worklist;

  liberator::Path path = *path_in;

  // outs() << "n: " << n->toString() << "\n";

  worklist.push_back(n);
  while (!worklist.empty()) {
    auto n = worklist.back();
    worklist.pop_back();
    if (visited.find(n) != visited.end())
      continue;
    int n_parents = 0;
    for (auto in : n->getInEdges()) {
      if (auto src = SVFUtil::dyn_cast<ActualParmVFGNode>(in->getSrcNode())) {
        auto cs = src->getCallSite();
        if (path.isCorrect(cs)) {
          path.popFrame();
          // outs() << "This is correct!!!\n";
        } else
          continue;
      }
      // outs() << in->toString() << "\n";

      auto pn = in->getSrcNode();
      worklist.push_back(pn);
      n_parents++;
    }
    // Maybe select some classes, e.g., alloca, param
    if (n_parents == 0)
      definitions.insert(n);
    visited.insert(n);
  }

  return definitions;
}

std::set<const VFGNode *> getDefinitionSet(const VFGNode *n) {
  std::set<const VFGNode *> definitions;
  std::set<const VFGNode *> visited;
  std::vector<const VFGNode *> worklist;

  worklist.push_back(n);
  while (!worklist.empty()) {
    auto n = worklist.back();
    worklist.pop_back();
    if (visited.find(n) != visited.end())
      continue;
    int n_parents = 0;
    for (auto in : n->getInEdges()) {
      auto pn = in->getSrcNode();
      worklist.push_back(pn);
      n_parents++;
    }
    // Maybe select some classes, e.g., alloca, param
    if (n_parents == 0)
      definitions.insert(n);
    visited.insert(n);
  }

  return definitions;
}
/**
 * Checks if the return value is a global variable
 * @param icfgNode - node of w
 */
bool doesReturnGlobalVarConst(const ICFGNode *icfgNode) {

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  // outs() << icfgNode->toString() << "\n";
  // outs() << "bb: " << icfgNode->getBB()->toString() << "\n";

  bool itReturnGlobalVarConst = false;
  for (auto i : icfgNode->getBB()->getICFGNodeList()) {

    // outs() << "i: " << i->toString() << "\n";
    auto x = llvmModuleSet->getLLVMValue(i);

    if (auto retinst = SVFUtil::dyn_cast<llvm::ReturnInst>(x)) {
      // outs() << "x: " << *x << "\n";

      auto rv = retinst->getReturnValue();

      // outs() << "[START CHECK]\n";
      // outs() << "rv: " << *rv << "\n";

      // strip value of BitCasts, Geps and ConstantExpr
      Value *stripped = rv->stripPointerCasts();

      if (SVFUtil::isa<llvm::GlobalVariable>(stripped)) {
        // outs() << "is a global variable\n";
        itReturnGlobalVarConst = true;
      }

      /*if (SVFUtil::isa<llvm::ConstantExpr>(rv)) {
        // outs() << "is a ConstantExpr\n";

        auto cexp = SVFUtil::cast<ConstantExpr>(rv);

        // BUG: this is a memory leak as the asi instruction is never deleted
        auto asi = cexp->getAsInstruction();

        // outs() << "asi: " << *asi << "\n";

        if (SVFUtil::isa<llvm::BitCastInst>(asi)) {
          // outs() << "asi is a BitCastInst variable\n";
          auto src = asi->getOperand(0);
          if (SVFUtil::isa<llvm::GlobalVariable>(src)) {
            // outs() << "src is a global variable\n";
            itReturnGlobalVarConst = true;
          }
        }
      }*/

      // outs() << "[END CHECK]\n";
    }
  }

  return itReturnGlobalVarConst;
}

} // namespace

namespace liberator {

bool AccessType::equals(std::string s) const {
  return s == to_string(*this, false);
}

bool handlerDispatcher(liberator::ValueMetadata *, std::string,
                       const ICFGNode *, const CallICFGNode *, int, AccessType,
                       H_SCOPE h_scope, liberator::Path *path);
bool hasHandlerDispatcher(liberator::ValueMetadata *, std::string,
                          const ICFGNode *, const CallICFGNode *, int,
                          H_SCOPE h_scope);
// TODO:
// STATE: added profiling for extractReturnMetadata
// NEXT_STEPS: AllocaInst seem to be the culprit, as for each allocainst in a
// function we call extractParameterMetadata
inline void
handleIntraICFGNodes(IntraICFGNode *node, llvm::Type *retType,
                     std::set<const Instruction *> &allocainst_set) {
  auto llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  INTRA_LOG("IntraICFGNode: {}", node->toString());
  if (!node->getSVFStmts().empty()) {
    for (auto stmt : node->getSVFStmts()) {
      INTRA_LOG("{}\n", stmt->toString());
    }
    const SVFStmt *stmt = node->getSVFStmts().front();
    if (stmt == nullptr) {
      cout << node->toString() << " has not statements.\n";
    }
    const SVFVar *var = stmt->getValue();
    const auto llvminst = llvmModuleSet->getLLVMValue(var);

    if (auto alloca = SVFUtil::dyn_cast<AllocaInst>(llvminst)) {
      INTRA_LOG("alloca {}\n", alloca->getName());
      if (alloca->getAllocatedType() == retType) {
        // outs() << "[INFO] => type ok!\n";
        // alloca_set.insert(vfgnode);
        allocainst_set.insert(alloca);
      }
    } else if (auto callinst = SVFUtil::dyn_cast<CallInst>(llvminst)) {
      // FIXME: is this code ever called or not? Because the instruction is
      // a CallICFGNode not an IntraICFGNode
      INTRA_LOG("callinst {}\n", callinst->getCaller()->getName());
      FunctionType *ftype = callinst->getFunctionType();
      if (ftype->getReturnType() == retType) {
        // outs() << "[INFO] => type ok!\n";
        // alloca_set.insert(vfgnode);
        allocainst_set.insert(callinst);
      }
    } else if (auto bitcastinst = SVFUtil::dyn_cast<BitCastInst>(llvminst)) {
      if (bitcastinst->getDestTy() == retType) {
        INTRA_LOG("bitcastinst {}\n", bitcastinst->getName());
        // outs() << "[INFO] => type ok!\n";
        // alloca_set.insert(vfgnode);
        allocainst_set.insert(bitcastinst);
        // bitcastinst_set.insert(bitcastinst);
      }
    }
  }
}

ValueMetadata::MyCallEdgeMap ValueMetadata::myCallEdgeMap_inst;
ValueMetadata extractReturnMetadata(const SVFG &vfg, const Value *llvmval) {
  llvm::TimeTraceScope TimeScope("extractReturnMetadata", [llvmval]() {
    if (auto *Inst = llvm::dyn_cast<llvm::Instruction>(llvmval)) {
      return Inst->getFunction()->getName().str();
    }
    return llvmval->getName().str();
  });
  // SVFValue *val = LLVMModuleSet::getLLVMModuleSet()->getSVFValue(llvmval);
  auto *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();
  auto nodeid = llvmModuleSet->getValueNode(llvmval);

  RETURN_LOG("---------- extractReturnMetadata called -------------\n");

  SVFIR *pag = SVFIR::getPAG();

  // pag->getICFG()->getICFGNode(nodeid);

  // TODO: Why is this needed here?
  // PointerAnalysis *pta = vfg->getPTA();

  PAGNode *pNode = pag->getGNode(nodeid);
  // const VFGNode* vNode =
  // vfg->getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pNode)); need a stack ->
  // FILO let S be a stack std::vector<Path> worklist; std::set<Path> visited;
  // S.push(v)
  // worklist.push_back(Path(vNode));

  ValueMetadata mdata;
  mdata.setValue(llvmval);

  Module *svfModule = llvmModuleSet->getMainLLVMModule();

  ICFG *icfg = pag->getICFG();

  auto svf_function = pNode->getFunction();
  const Function *fun =
      SVFUtil::dyn_cast<Function>(llvmModuleSet->getLLVMValue(svf_function));
  const FunObjVar *svfun = pNode->getFunction();

  if (fun->isDeclaration())
    return mdata;

  FunExitICFGNode *fun_exit = icfg->getFunExitICFGNode(svfun);
  Type *retType = fun->getReturnType();

  if (!SVFUtil::isa<llvm::PointerType>(retType))
    return mdata;
  RETURN_LOG("--- RETURNS A POINTER ---\n");

  if (doesReturnGlobalVarConst(fun_exit)) {
    AccessType acNodeConst(retType);
    addWrteToAllFields(&mdata, acNodeConst, fun_exit);
    return mdata;
  }

  PHIFun phi;
  PHIFunInv phi_inv;
  getPhiFunction(svfModule, icfg, &phi, &phi_inv);

  // std::set<const VFGNode*> alloca_set;
  // std::set<const Value*> allocainst_set;
  std::set<const Instruction *> allocainst_set;
  // std::set<const Value*> bitcastinst_set;

  std::set<const FunObjVar *> visited_functions;

  // how many alloca?
  FunEntryICFGNode *entry_node = icfg->getFunEntryICFGNode(svfun);

  std::stack<std::pair<ICFGNode *, std::stack<ICFGEdge *>>> working;

  std::set<ICFGNode *> visited;

  std::stack<ICFGEdge *> empty_stack;
  working.push(std::make_pair(entry_node, empty_stack));

  AccessTypeSet *ats = mdata.getAccessTypeSet();

  uint64_t total_main_loop_ns = 0;
  uint64_t total_def_set_ns = 0;
  uint64_t total_def_loop_ns = 0;
  uint64_t total_param_meta_ns = 0;
  auto function_start = std::chrono::high_resolution_clock::now();

  auto t_loop_start = std::chrono::high_resolution_clock::now();
  while (!working.empty()) {

    auto el = working.top();
    working.pop();

    ICFGNode *node = el.first;
    std::stack<ICFGEdge *> curr_stack = el.second;

    if (auto intra_stmt = SVFUtil::dyn_cast<IntraICFGNode>(node)) {
      handleIntraICFGNodes(intra_stmt, retType, allocainst_set);
    } // end IntraICFGNode
    else if (auto call_node = SVFUtil::dyn_cast<CallICFGNode>(node)) {
      // Handling calls
      // outs() << "[INFO] " << call_node->toString() << " \n";
      RETURN_LOG("CallICFGNode: {}\n", call_node->toString());

      if (!config_t::instance()->consider_indirect_calls &&
          call_node->isIndirectCall())
        continue;

      // LLVM::Function that gets called
      auto callee_funobj = call_node->getCalledFunction();
      // TODO: why is this needed here?
      // auto callee = llvmModuleSet->getLLVMValue(callee_funobj);

      auto stmts = call_node->getSVFStmts();
      const llvm::CallBase *inst = nullptr;

      for (auto stmt : stmts) {
        const auto var = stmt->getValue();
        auto val = llvmModuleSet->getLLVMValue(var);
        inst = SVFUtil::dyn_cast<CallBase>(val);
        if (!inst)
          continue;
        /*if (!stmts.empty()) {
          const SVFStmt *svfStmt = stmts.front();

          const auto var = svfStmt->getValue();
          auto val = llvmModuleSet->getLLVMValue(var);
          inst = SVFUtil::dyn_cast<CallBase>(val);
        }*/

        // RETURN_LOG("[INFO] callinst2 {}\n", *inst);
        FunctionType *ftype = inst->getFunctionType();
        bool ret_type_is_ok = false;

        // print indirect jumps
        /*for (auto as : myCallEdgeMap_inst) {
          for (auto s : as.second) {
            outs() << s->toString() << "\n";
          }
        }*/

        if (ftype->getReturnType() == retType) {
          // outs() << "[INFO] => type ok!\n";
          // alloca_set.insert(vfgnode);
          allocainst_set.insert(inst);
          ret_type_is_ok = true;
        } else if (ValueMetadata::myCallEdgeMap_inst.find(call_node) !=
                   ValueMetadata::myCallEdgeMap_inst.end()) {
          // outs() << "[DEBUG] -> call_node is in the edge map\n";
          RETURN_LOG("[INFO] call node: {} has ind jump? \n",
                     call_node->toString());
          auto targets = ValueMetadata::myCallEdgeMap_inst[call_node];
          for (auto t : targets) {
            std::string fun = t->getName();
            RETURN_LOG("[INFO] t->getName(): {}\n", fun);
            if (hasHandlerDispatcher(&mdata, fun, node, call_node, -1,
                                     C_RETURN)) {
              RETURN_LOG("[INFO] has dispatcher!\n");
              // allocainst_set.insert(inst);
              ret_type_is_ok = true;
            }
          }
        }

        if (ret_type_is_ok &&
            ValueMetadata::myCallEdgeMap_inst.find(call_node) !=
                ValueMetadata::myCallEdgeMap_inst.end()) {
          // outs() << "[DEBUG] -> call_node is in the edge map\n";
          auto targets = ValueMetadata::myCallEdgeMap_inst[call_node];
          for (auto t : targets) {
            std::string fun = t->getName();
            // malloc handler
            AccessType acNode(retType);
            ValueMetadata mdata_tmp;
            handlerDispatcher(&mdata_tmp, fun, node, call_node, -1, acNode,
                              C_RETURN, nullptr);
            RETURN_LOG("[INFO] After handlerDispatcher\nmeta_tmp:\n");
            RETURN_LOG("{}\n", to_string(mdata_tmp, false));

            bool added_create = false;
            for (auto at : *mdata_tmp.getAccessTypeSet()) {
              if (at.get_kind() == AccessType::kind_e::create) {
                outs() << "I added a create!!!\n";
                added_create = true;
                break;
              }
            }
            auto ret_node = call_node->getRetICFGNode();
            const SVFVar *ret_node_val = ret_node->getActualRet();

            if (ret_node_val) {
              // TODO: check if this is correct
              // old code:
              // const auto xx = ret_node_val->getValue();
              // PAGNode *zz = pag->getGNode(pag->getValueNode(xx));
              const auto xx = llvmModuleSet->getLLVMValue(ret_node_val);
              PAGNode *zz = pag->getGNode(ret_node_val->getId());
              const VFGNode *vNode;
              if (!vfg.hasDefSVFGNode(SVFUtil::cast<SVF::ValVar>(zz))) {
                RETURN_LOG("zz has no Def Nodes\n");
              } else {
                vNode = vfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(zz));
                RETURN_LOG("vNode: {}\n", vNode->toString());
              }

              // if a "create" is added, I need to check it leads
              // to a bitcast, otherwise I ignore it
              // const Instruction **inst_bitcast;
              if (added_create && leadsToBitCastOfType(vNode, retType)) {
                // allocainst_set.insert(*inst_bitcast);
                RETURN_LOG("I allocated a bitcast!!\n");
                // outs() << "bitcast: " << *(*inst_bitcast) << "\n";
                AccessType acNode(retType);
                // ValueMetadata mdata;
                handlerDispatcher(&mdata, fun, node, call_node, -1, acNode,
                                  C_RETURN, nullptr);
              }
            }
          }
        }
        // visited_functions.insert(t);
      }

      // if (callee != nullptr) {
      //     std::string fun = callee->getName();
      //     // malloc handler
      //     AccessType acNode(retType);
      //     handlerDispatcher(&mdata, fun, node, call_node, -1,
      //                         acNode, C_RETURN);

      //     for (unsigned p = 0; p < ftype->getNumParams(); p++) {
      //         handlerDispatcher(&mdata, fun, node, call_node, p,
      //                             acNode, C_RETURN);
      //     }

      // }
    }

    // We'll go through the children and add unknown ones to our work list.
    // outs() << "NODE: " << node->toString() << "\n";
    if (node->hasOutgoingEdge()) {
      ICFGNode::const_iterator it = node->OutEdgeBegin();
      ICFGNode::const_iterator eit = node->OutEdgeEnd();

      for (; it != eit; ++it) {
        ICFGEdge *edge = *it;
        ICFGNode *dst = edge->getDstNode();

        if (visited.find(dst) != visited.end()) {
          // We've seen it already

          // BUG: if CallCFGEdge and already visited, then skip the
          // call and go to the next return
          if (auto call_edge = SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
            ICFGEdge *next_ret = phi[call_edge];
            ICFGNode *dst_new = next_ret->getDstNode();
            // next_ret
            // curr_stack.push(next_ret);
            working.push(std::make_pair(dst_new, curr_stack));
          }

          // outs() << "\talready visited: ";
          // outs() << dst->toString() << "\n";
          continue;
        }

        if (auto ret_edge = SVFUtil::dyn_cast<RetCFGEdge>(edge)) {

          if (curr_stack.size() != 0) {
            ICFGEdge *ret = curr_stack.top();
            if (ret_edge == ret) {
              curr_stack.pop();
              working.push(std::make_pair(dst, curr_stack));
              visited.insert(dst);
            }
          }
        } else if (auto call_edge = SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
          ICFGEdge *next_ret = phi[call_edge];
          curr_stack.push(next_ret);
          working.push(std::make_pair(dst, curr_stack));
          visited.insert(dst);
        } else {
          working.push(std::make_pair(dst, curr_stack));
          visited.insert(dst);
        }
      }
    }
  }
  total_main_loop_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now() - t_loop_start)
          .count();
  // We have visited all the nodes

  for (auto v : visited)
    visited_functions.insert(v->getFun());

  auto pXX = fun_exit->getFormalRet();
  RETURN_LOG("-----------------------------------\n");
  RETURN_LOG("{}\n", pXX->toString());
  RETURN_LOG("-----------------------------------\n");
  const VFGNode *XX = vfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pXX));
  if (LLVMModuleSet::getLLVMModuleSet()->hasLLVMValue(pXX)) {
    const llvm::Value *val =
        LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(pXX);
    std::string buffer;
    llvm::raw_string_ostream os(buffer);
    val->print(os);
    RETURN_LOG("{}\n", buffer);
  } else {
    RETURN_LOG("VFGNode has an SVFVar but no LLVM Value\n");
  }
  auto *value = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(XX->getValue());
  if (value == nullptr) {
    RETURN_LOG("no llvm instruction\n");
  }

  // outs() << "[INFO] Visited " << visited_functions.size() << "
  // functions\n"; for (auto f: visited_functions)
  //     outs() << "fun: " << f->getName() << "\n";

  // std::set<const VFGNode*> defA = getDefinitionSet(XX);
  auto t_defset_start = std::chrono::high_resolution_clock::now();
  std::set<const VFGNode *> defA =
      getDefinitionSetForRet(XX, visited_functions);
  total_def_set_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now() - t_defset_start)
          .count();

  // outs() << "ORIGINAL RETURN:\n";
  // outs() << XX->toString() << "\n";
  // outs() << "DEFINITION:\n";
  auto t_defloop_start = std::chrono::high_resolution_clock::now();
  for (auto n : defA) {
    if (auto s = SVFUtil::dyn_cast<StmtVFGNode>(n)) {
      auto svfinst = SVFUtil::dyn_cast<Instruction>(
          llvmModuleSet->getLLVMValue(s->getValue()));
      if (svfinst == nullptr)
        continue;

      // outs() << n->toString() << "\n";
      // outs() << "LLVM inst:\n";
      // outs() << *llvminst << "\n";
      // outs() << "Fun:\n";
      // outs() << n->getFun()->getName() << "\n";
      // outs() << "-----\n";

      auto pagedge = s->getSVFStmt();
      auto node = pagedge->getICFGNode();

      if (auto call_node = SVFUtil::dyn_cast<CallICFGNode>(node)) {

        // outs() << "This comes from a call\n";
        // Handling calls
        if (!config_t::instance()->consider_indirect_calls &&
            call_node->isIndirectCall())
          continue;

        const auto inst =
            dyn_cast<CallBase>(llvmModuleSet->getLLVMValue(call_node));
        auto callee = LLVMUtil::getCallee(inst);

        // // outs() << "[INFO] callinst2 " << *inst << "\n";
        FunctionType *ftype = inst->getFunctionType();
        // if (ftype->getReturnType() == retType) {
        //     // outs() << "[INFO] => type ok!\n";
        //     // alloca_set.insert(vfgnode);
        //     allocainst_set.insert(inst);
        // }

        if (callee != nullptr) {
          std::string fun = callee->getName().str();
          // malloc handler
          AccessType acNode(retType);
          handlerDispatcher(&mdata, fun, node, call_node, -1, acNode, C_RETURN,
                            nullptr);

          for (unsigned p = 0; p < ftype->getNumParams(); p++) {
            handlerDispatcher(&mdata, fun, node, call_node, p, acNode, C_RETURN,
                              nullptr);
          }
        }
      }
    }
  }
  total_def_loop_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now() - t_defloop_start)
          .count();
  // outs() << "DONE!\n";
  // exit(0);

  // outs() << "[INFO] extractParameterMetadata part\n";

  // outs() << "mdata [1] " << mdata.getSummary();
  // if (mdata.isFilePath()) {
  //     outs() << "IsFilePath -> True \n";
  // } else {
  //     outs() << "IsFilePath -> False \n";
  // }

  // std::map<const Instruction*, AccessTypeSet> all_ats;
  std::map<const Instruction *, ValueMetadata> all_ats;
  auto t_param_start = std::chrono::high_resolution_clock::now();

  uint64_t total_alloc_loop_ns = 0;
  uint64_t total_merge_loop_ns = 0;
  uint64_t total_extractParam_ns = 0;
  uint64_t total_nestedCheck_ns = 0;

  for (auto a : allocainst_set) {
    auto t_alloc_start = std::chrono::high_resolution_clock::now();
    // outs() << "[INFO] paramAT() " << *a << " -- ";
    RETURN_LOG("{}\n", a->getFunction()->getName().str());

    auto a_id = LLVMModuleSet::getLLVMModuleSet()->getValueNode(a);

    // FIXME: This is performance critical when allocainst_set is huge
    auto t2 = std::chrono::high_resolution_clock::now();
    ValueMetadata mdata = extractParameterMetadata(vfg, a, retType, a_id);
    auto t3 = std::chrono::high_resolution_clock::now();
    total_extractParam_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

    // outs() << "mdata_xx [2] " << mdata_xx.getSummary();
    // if (mdata_xx.isFilePath()) {
    //     outs() << "IsFilePath -> True \n";
    // } else {
    //     outs() << "IsFilePath -> False \n";
    // }

    // outs() << "[STARTING POINT] " << *a << "\n";
    // outs() << " result -> " << mdata.getAccessNum() << "AT\n";
    // outs() << " result -> " << mdata.toString(false) << "\n";
    // // exit(1);

    // XXX: TO REMOVE LATER
    bool do_not_return = true;
    for (auto at : *mdata.getAccessTypeSet()) {
      // for (auto at: *mdata_xx.getAccessTypeSet()) {
      if (at.get_kind() == AccessType::kind_e::ret) {
        auto l_ats_all_nodes = at.getICFGNodes();
        for (auto inst : l_ats_all_nodes) {
          if (inst == fun_exit) {
            for (auto at2 : *mdata.getAccessTypeSet())
              // for (auto at2: *mdata_xx.getAccessTypeSet())
              for (auto inst2 : at.getICFGNodes())
                ats->insert(at2, inst2);
            do_not_return = false;
            break;
          }
        }
      }
    }
    if (do_not_return)
      all_ats[a] = mdata;

    auto t4 = std::chrono::high_resolution_clock::now();
    total_nestedCheck_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(t4 - t3).count();

    auto t_alloc_end = std::chrono::high_resolution_clock::now();
    total_alloc_loop_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                               t_alloc_end - t_alloc_start)
                               .count();
  }

  // outs() << "Get Summary:\n";
  // I just merge all!
  // FIXME: This is performance critical
  auto t_merge_start = std::chrono::high_resolution_clock::now();
  for (auto el : all_ats) {
    // outs() << "Func: " << el.first->getFunction()->getName().str() << "\n";
    // outs() << "Inst: " << *el.first << "\n";
    // outs() << el.second.getSummary();
    // outs() << "----\n";
    for (auto atx : *el.second.getAccessTypeSet())
      for (auto inst : atx.getICFGNodes())
        ats->insert(atx, inst);
  }
  auto t_merge_end = std::chrono::high_resolution_clock::now();
  total_merge_loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t_merge_end - t_merge_start)
                            .count();

  total_param_meta_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now() - t_param_start)
          .count();
  auto function_end = std::chrono::high_resolution_clock::now();
  uint64_t total_func_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               function_end - function_start)
                               .count();

  if (total_func_ns > 0) {
    outs() << "\n[PROFILING] `extractReturnMetadata` Time Breakdown:\n";
    outs() << "  Total Function Time: " << (total_func_ns / 1e6) << " ms\n";
    outs() << "  1. Main ICFG Loop  : " << (total_main_loop_ns / 1e6) << " ms ("
           << ((double)total_main_loop_ns / total_func_ns) * 100.0 << "%)\n";
    outs() << "  2. getDefSetForRet : " << (total_def_set_ns / 1e6) << " ms ("
           << ((double)total_def_set_ns / total_func_ns) * 100.0 << "%)\n";
    outs() << "  3. Process Def Loop: " << (total_def_loop_ns / 1e6) << " ms ("
           << ((double)total_def_loop_ns / total_func_ns) * 100.0 << "%)\n";
    outs() << "  4. paramMeta Loop  : " << (total_param_meta_ns / 1e6)
           << " ms (" << ((double)total_param_meta_ns / total_func_ns) * 100.0
           << "%)\n";
    outs() << "     - allocainst elems : " << allocainst_set.size() << "\n";
    outs() << "     - alloc_loop total : " << (total_alloc_loop_ns / 1e6)
           << " ms\n";
    outs() << "       --> extractParam : " << (total_extractParam_ns / 1e6)
           << " ms\n";
    outs() << "       --> nested Check : " << (total_nestedCheck_ns / 1e6)
           << " ms\n";
    outs() << "       --> untimed overhead: "
           << ((total_alloc_loop_ns - total_extractParam_ns -
                total_nestedCheck_ns) /
               1e6)
           << " ms\n";
    outs() << "     - merge loop total  : " << (total_merge_loop_ns / 1e6)
           << " ms\n";
    outs() << "  Other (Wait)       : "
           << ((double)(total_func_ns - total_main_loop_ns - total_def_set_ns -
                        total_def_loop_ns - total_param_meta_ns) /
               1e6)
           << " ms\n\n";
  }

  return mdata;
}

/**
If exists, call the predefined handler for function fun.

@param: mdata: the access type set to be updated by the handler
@param: fun: the name of the called function
@param: ifcgNode: the function currently analyzed
@param: cs: the callsite node
@param: param_num: the parameter number

@return: boolean value indicating if the analysis should continue on the
subfield. For example, it might be false for a cast to indicate we do not try
to follow further child of the node. default true.
*/
bool handlerDispatcher(ValueMetadata *mdata, std::string fun,
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

/**
It checks if the target function is handled by our dispatchers.

@param: ats: the access type set to be updated by the handler
@param: fun: the name of the function
@param: node: the node currently analyzed

@return: boolean value indicating if the function is handled by our
dispacthers
*/
bool hasHandlerDispatcher(ValueMetadata *mdata, std::string fun,
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

bool areCompatible(FunctionType *caller, FunctionType *callee) {

  bool are_comp = false;

  if (caller->isVarArg()) {

    are_comp = caller->getReturnType() == callee->getReturnType();

    int p;
    for (p = 0; p < caller->getNumParams(); p++)
      are_comp &= caller->getParamType(p) == callee->getParamType(p);

  } else {
    are_comp = caller == callee;
  }

  return are_comp;
}

std::vector<std::string>
extractDependencyAmongParameters(const SVF::SVFVar *current_parm,
                                 ValueMetadata &mdata, SVF::SVFG &svfg,
                                 const FunObjVar *fun) {

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  std::set<std::string> set_by;

  SVFIR *pag = SVFIR::getPAG();

  PAG::FunToArgsListMap funmap_par = pag->getFunArgsMap();
  auto fun_params = funmap_par[fun];

  auto ats = mdata.getAccessTypeSet();
  auto ats_it = ats->begin();
  auto ats_end = ats->end();
  for (; ats_it != ats_end; ++ats_it) {
    auto at = *ats_it;
    // outs() << at.toString() << "\n";
    if (at.get_kind() == AccessType::kind_e::write) {
      // outs() << at.toString() << "\n";
      // outs() << "the instructions:\n";
      for (auto node : at.getICFGNodes()) {

        auto *intra_n = SVFUtil::dyn_cast<IntraICFGNode>(node);
        if (intra_n == nullptr)
          continue;

        auto *llvm_inst = llvmModuleSet->getLLVMValue(
            intra_n->getSVFStmts().front()->getValue());

        auto *store_inst = SVFUtil::dyn_cast<StoreInst>(llvm_inst);
        if (store_inst == nullptr)
          continue;

        auto src = store_inst->getValueOperand();

        // outs() << *store_inst << "\n";
        // outs() << *src << "\n";
        auto llvm_val = llvmModuleSet->getValueNode(src);
        PAGNode *pS = pag->getGNode(llvm_val);
        const VFGNode *vS = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pS));
        unsigned int p_idx = 0;
        for (const SVFVar *p : fun_params) {
          if (p == current_parm) {
            p_idx++;
            continue;
          }
          // TODO: check if this is correct
          // if the id of SVFVar is what we search for here
          PAGNode *pP = pag->getGNode(p->getId());
          const VFGNode *vP =
              svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pP));

          if (areConnected(vP, vS)) {
            set_by.insert("param_" + std::to_string(p_idx));
          }

          p_idx++;
        }
      }
    }
  }

  std::vector<std::string> set_by_list;
  for (auto d : set_by)
    set_by_list.push_back(d);

  return set_by_list;
}

std::string extractLenDependencyParameter(const SVF::SVFVar *current_parm,
                                          ValueMetadata &mdata, SVF::SVFG &svfg,
                                          const FunObjVar *fun) {

  // // outs() << "CURRENT PARAM: \n";
  // // outs() << current_parm->toString() << "\n";

  auto par_type = current_parm->getType();
  auto llvm_type = LLVMModuleSet::getLLVMModuleSet()->getLLVMType(par_type);

  if (!SVFUtil::isa<PointerType>(llvm_type))
    return "";

  std::string dependent_param = "";

  SVFIR *pag = SVFIR::getPAG();

  PAG::FunToArgsListMap funmap_par = pag->getFunArgsMap();
  auto fun_params = funmap_par[fun];

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  // seek dependencies through loops
  for (auto i : mdata.getIndexes()) {
    // outs() << "I: " << *i << "\n";

    llvm::Instruction *ii = SVFUtil::dyn_cast<llvm::Instruction>(i);

    // just in case
    if (ii == nullptr)
      continue;

    DominatorTree dom_tree(*ii->getFunction());
    LoopInfo loop_info(dom_tree);
    Loop *l = loop_info.getLoopFor(ii->getParent());

    if (l == nullptr) {
      continue;
    }

    SmallVector<llvm::BasicBlock *> exits;
    l->getExitingBlocks(exits);
    for (auto e : exits) {
      auto v = &e->back();
      // outs() << "Exit Cond:\n" << *v << "\n";

      PAGNode *pV = pag->getGNode(llvmModuleSet->getValueNode(v));
      const VFGNode *vV = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pV));
      PAGNode *pI = nullptr;
      PAGNode *pP = nullptr;

      bool index_control_loop = false;
      bool param_control_loop = false;
      for (auto i : mdata.getIndexes()) {
        pI = pag->getGNode(llvmModuleSet->getValueNode(i));
        const VFGNode *vI = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pI));

        if (areConnected(vI, vV)) {
          // outs() << "Index control Loop\n";
          index_control_loop = true;
          break;
        }
      }

      int p_idx = 0;
      for (auto p : fun_params) {
        if (p == current_parm) {
          p_idx++;
          continue;
        }

        auto llvm_p_val = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(p);

        // std::string str;
        // llvm::raw_string_ostream rawstr(str);
        // rawstr << *llvm_p_val->getType();
        // outs() << "Testing par " << p_idx << "  (index loop phase)\n";
        // outs() << str << "\n";
        if (SVFUtil::isa<PointerType>(llvm_p_val->getType())) {
          // outs() << "It is a pointer, skip it (index loop phase)!\n";
          p_idx++;
          continue;
        }

        // pP = const_cast<llvm::Value*>(p->getValue());
        pP = pag->getGNode(p->getId());
        const VFGNode *vP = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pP));
        // const_cast<llvm::Value*>(p->getValue());
        // outs() << "P: " << pP->toString() << "\n";
        if (areConnected(vP, vV)) {
          // outs() << "Param control Loop\n";
          param_control_loop = true;
          break;
        }
        p_idx++;
        // else
        //     outs() << "no control!\n";
      }

      if (param_control_loop && index_control_loop) {
        // outs() << "Index: " << pI->toString() << "\n";
        // outs() << "Param: " << pP->toString() << "\n";
        dependent_param = "param_" + std::to_string(p_idx);
      }
    }
  }

  if (dependent_param == "")
    for (auto el : mdata.getFunParams()) {
      // outs() << *fs << "\n";
      auto fs = el.first;
      auto path = el.second; // Path == Context == Stack
      // path.dump_stack();
      // SVFUtil::outs() << "---------\n";
      // continue;

      auto llvm_val = llvmModuleSet->getValueNode(fs);
      PAGNode *pS = pag->getGNode(llvm_val);
      const VFGNode *vS = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pS));

      int p_idx = 0;
      bool param_control_len = false;
      for (auto p : fun_params) {
        if (p == current_parm) {
          p_idx++;
          continue;
        }

        auto p_val = p;

        auto llvm_p_val =
            LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(p_val);

        // std::string str;
        // llvm::raw_string_ostream rawstr(str);
        // rawstr << *llvm_p_val->getType();
        // outs() << "Testing par " << p_idx << "\n";
        // outs() << str << "\n";
        if (SVFUtil::isa<PointerType>(llvm_p_val->getType())) {
          // outs() << "It is a pointer, skip it!\n";
          p_idx++;
          continue;
        }

        PAGNode *pP = pag->getGNode(p->getId());

        const VFGNode *vP = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pP));
        // const_cast<llvm::Value*>(p->getValue());
        // outs() << "P: " << pP->toString() << "\n";
        if (areConnectedCtx(vP, vS, &path)) {
          // outs() << "connected!\n";
          param_control_len = true;
          break;
        }
        p_idx++;
        // else
        //     outs() << "no control!\n";
      }

      if (param_control_len) {
        dependent_param = "param_" + std::to_string(p_idx);
      }
    }

  return dependent_param;
}

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
  case Type::StructTyID:
    return "Struct " + t->getStructName().str() + " with size " + size_bytes;
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

      // outs() << "[DEBUG] GEP under analysis:\n";
      // outs() << *inst << "\n";
      // outs() << vNode->toString() << "\n";
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

      // outs() << "[DEBUG]\n";

      // outs() << "sType:\n";
      // outs() << *sType << "\n";
      // outs() << TypeMatcher::compute_hash(sType) << "\n";

      // outs() << "dType:\n";
      // outs() << *dType << "\n";
      // outs() << TypeMatcher::compute_hash(dType) << "\n";

      // outs() << "pType:\n";
      // outs() << *pType << "\n";
      // outs() << TypeMatcher::compute_hash(pType) << "\n";

      // outs() << "acNode.getType():\n";
      // outs() << *acNode.getType() << "\n";
      // outs() << TypeMatcher::compute_hash(acNode.getType()) << "\n";
      // exit(1);

      // outs() << "compare_types(pType, acNode.getType()) "
      //     << TypeMatcher::compare_types(pType, acNode.getType())
      //     << "\n";

      // outs() << "[DEBUG END]\n";

      // this avoids us to move into strange pointer-offset operations
      // that look like field access
      // if (SVFUtil::isa<llvm::StructType>(sType) &&
      //  AccessTypeSet::isSameType(pType, acNode.getType()) ) {
      auto printType = [](llvm::Type *t, std::string_view msg) {
        if (t->isStructTy()) {
          GEP_LOG("{} {}\n", msg, getType(t));
        } else if (t->isArrayTy()) {
          GEP_LOG("{} {}[]", msg, getType(t));
        }
      };

      printType(sType, "Source type:");
      printType(dType, "Result type:");
      vNode->getDefSVFVars();

      if (TypeMatcher::compare_types(sType, acNode.get_llvm_type()) &&
          !acNode.is_visited(sType)) {
        // SVFUtil::isa<PointerType>(dType)) {
        // Here we only consider struct or constant array accesses
        if (gep_inst->hasAllConstantIndices() &&
            gep_inst->getNumIndices() > 1) {
          int pos = 1;
          // Note: getNumIndices returns the number of indices after the
          // base pointer. EXAMPLE: ... i32 0, i32 1, i32 3 will return 2
          for (; pos <= gep_inst->getNumIndices(); pos++) {
            // pos == 1 is the field offset or the constant arry offset
            if (pos == 1) {
              AccessType tmpAcNode = acNode;
              GEP_LOG("Adding a temporary AccessType to the AccessTypeSet.\n");
              GEP_LOG("The AccessTypeSet now has {} entries.\n",
                      ats.size() + 1);
              tmpAcNode.addField(-1);
              tmpAcNode.set_kind(AccessType::kind_e::read);
              ats.insert(tmpAcNode, vNode->getICFGNode());
            } else {
              ConstantInt *CI =
                  dyn_cast<ConstantInt>(gep_inst->getOperand(pos));
              uint64_t idx = CI->getZExtValue();
              GEP_LOG("Adding a constant to the AccesType {}\n",
                      CI->getZExtValue());
              acNode.addField(idx);
              acNode.set_llvm_type(dType);
            }
          }
        } else if (acNode.get_num_fields() == 0) {

          // is_array = !SVFUtil::isa<ConstantInt>(
          //     inst->getOperand(1)) ||
          //     inst->getNumIndices() == 1;
          // if (is_array) {
          //     auto d = inst->getOperand(1);
          //     mdata.addIndex(d);
          // }

          bool is_array = false;

          auto d = gep_inst->getOperand(1);
          if (!SVFUtil::isa<ConstantInt>(d)) {
            GEP_LOG("Adding index {} to mdata\n", d->getName());
            GEP_LOG("Setting is_array to true\n");
            is_array = true;
            mdata.addIndex(d);
            mdata.addFunParam(d, &p);
          } else if (gep_inst->getNumIndices() == 1) {
            is_array = true;
            GEP_LOG("Array index {}\n", gep_inst->getName());
            mdata.addIndex(gep_inst);
          }

        } else {
          return true;
        }
        acNode.add_visited_type(sType);
      }
      // else {
      //     outs() << "[DEBUG] GEP incoherent: \n";

      //     outs() << "instruction:\n";
      //     outs() << vNode->toString() << "\n";

      //     outs() << "sType:\n";
      //     outs() << *sType << "\n";

      //     outs() << "dType:\n";
      //     outs() << *dType << "\n";

      //     outs() << "pType:\n";
      //     outs() << *pType << "\n";

      //     outs() << "acNode.getType():\n";
      //     outs() << *acNode.getType() << "\n";
      //     outs() << "\n";
      //     exit(1);
      // }
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

      handlerDispatcher(&mdata, t->getName(), vNode->getICFGNode(), cs, n_param,
                        acNode, C_PARAM, &p);
    }
  }
}

struct access_path_t {
public:
  const Type *base_ty;
  vector<int> indices;
  vector<Type *> field_ty;
  Value *v;
  bool is_struct;
  bool is_array;
};

struct param_access_site_t {
  const ICFGNode *node; // where in the program the read happens
  enum class kind_e {
    e_direct_read,
    e_indirect_read,
    e_direct_write,
    e_indirect_write
  };
};

struct param_access_summary_t {
  std::vector<param_access_site_t> sites;

  // wich abstract objects this function reads/writes
  NodeBS read_objs;
  NodeBS write_objs;
};

// TODO: get other possible representations of keys.
std::string make_key(const Function *F, NodeID param_id) {
  std::string key = F->getName().str() + "#" + std::to_string(param_id);
  return key;
}

class param_access_tracker_t {
  SVFIR *pag;
  PointerAnalysis *pta;
  SVFG *svfg;
  CallGraph *cg;

  std::unordered_map<std::string, param_access_summary_t> memo;

public:
  param_access_tracker_t(SVFIR *p, PointerAnalysis *a, SVFG *s)
      : pag(p), pta(a), svfg(s), cg(a->getCallGraph()) {}

  param_access_summary_t analyze(const Function *F, NodeID param_id) {
    std::string key = make_key(F, param_id);

    auto it = memo.find(key);
    if (it != memo.end())
      return it->second;

    memo[key] = param_access_summary_t{};

    param_access_summary_t summary;

    for (const auto &bb : *F) {
      for (auto it = bb.begin(); it != bb.end(); ++it) {
        const Instruction *i = &(*it);
        // TODO: How is the CallICFGNode graph created
        auto inst = LLVMModuleSet::getLLVMModuleSet()->getCallICFGNode(i);
        for (const auto *stmt : pag->getSVFStmtList(inst)) {
          if (const LoadStmt *ld = SVFUtil::dyn_cast<LoadStmt>(stmt)) {
            SVFUtil::outs() << ld->toString() << "\n";
          }
        }
      }
    }
  }
};

void my_proc(const SVFGNode *n, access_path_t &ap) {
  LLVMModuleSet *module_set = LLVMModuleSet::getLLVMModuleSet();
  if (auto formal_param = SVFUtil::dyn_cast<FormalParmVFGNode>(n)) {
    auto callee = formal_param->getICFGNode()->getFun();
    std::string mangled = callee->getName();
    std::string demangle = llvm::demangle(mangled);
    stringstream ss;
    callee->getReturnType()->print(ss);
    MY_EX_LOG(
        "Argument is passed to function {} unmangled {} with return type: {}\n",
        mangled, demangle, ss.str());
  } else if (auto gep = SVFUtil::dyn_cast<GepVFGNode>(n)) {
    if (gep->getType())
      MY_EX_LOG("{}\n", gep->getType()->toString());
    auto gepV = module_set->getLLVMValue(gep->getValue());
    if (!gepV)
      return;
    auto gep_inst = dyn_cast<GetElementPtrInst>(gepV);
    if (!gep_inst)
      return;
    if (gep_inst->hasAllConstantIndices() && gep_inst->getNumIndices() > 1) {
      auto *CI = dyn_cast<ConstantInt>(gep_inst->getOperand(2));
      uint64_t idx = CI->getZExtValue();
      ap.indices.push_back(idx);
      if (ap.base_ty) {
        if (auto st = dyn_cast<StructType>(ap.base_ty)) {
          // we have to store struct accesses
          // we have to differentiate array accesses
          if (st->getStructElementType(idx)->getTypeID() == Type::PointerTyID) {
            ap.indices.push_back(idx);
            // TODO: here we need to infer the type of the access again
            for (auto I : gep_inst->users()) {
              if (auto store = dyn_cast<StoreInst>(I)) {
                if (auto global =
                        dyn_cast<GlobalVariable>(store->getValueOperand())) {
                  // TODO: wie mappe ich den Typ jetzt auf die Class A?
                  auto gTy = global->getValueType();
                  std::string buffer;
                  raw_string_ostream os(buffer);
                  gTy->print(os);
                  MY_EX_LOG("Global Variable: {}\n", buffer);
                  ap.field_ty.push_back(gTy);
                }
              }
            }
          }
        }
      }
    } else if (auto store_inst = SVFUtil::dyn_cast<StoreVFGNode>(n)) {
    }
    if (gep_inst) {
      std::string buffer;
      raw_string_ostream os(buffer);
      gep_inst->getSourceElementType()->print(os);
      MY_EX_LOG("{}\n", buffer);
      gep_inst->getResultElementType()->print(os);
      MY_EX_LOG("{}\n", buffer);
      // now what if the gep is followed by a store
      // it can be a write to a struct or array
      for (auto I : gep_inst->users()) {
        if (auto store = dyn_cast<StoreInst>(I)) {
          if (auto global =
                  dyn_cast<GlobalVariable>(store->getValueOperand())) {
            // wie mappe ich den Typ jetzt auf die Class A?
            auto gTy = global->getValueType();
          }
        }
      }
    }
  }
}

ValueMetadata my_extract_parameter_metadata(const SVFG &vfg, const Value *val,
                                            const Type *seek_type) {
  ValueMetadata res;
  MY_EX_LOG("In EXTRACT PARAMETER\n");
  PAG *pag = PAG::getPAG();
  auto *module_set = LLVMModuleSet::getLLVMModuleSet();
  // TODO: is there always a GNode to each LLVM value?
  auto param_node = module_set->getValueNode(val);
  PAGNode *arg_node = pag->getGNode(param_node);
  // this is necessary to get the definition node of the SVFVar in the SVFG
  // graph
  auto vNode = vfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(arg_node));
  if (config_t::instance()->debug) {
    PARAM_META_LOG("Working node:\n");
    PARAM_META_LOG("A.-> {}\n", vNode->toString());
    PARAM_META_LOG("B.-> {}\n", vNode->getFun()->getName());
    // PARAM_META_LOG("Stack size: {}\n", p.getStackSize());

    PARAM_META_LOG("[IN EDGES]\n");
    for (VFGNode::const_iterator it = vNode->InEdgeBegin(),
                                 eit = vNode->InEdgeEnd();
         it != eit; ++it) {
      VFGEdge *edge = *it;

      // TODO: Remove
      // STATE: I wrote the my_extract_parameter_metadata function
      // NEXT STEPS: implement the same functionalty as extractParameterMetadata
      // but first try to understand of CHI and MU nodes are inserted and how
      // the VFG is created of SVF and write it down in the thesis
      if (SVFUtil::isa<SVF::DirectSVFGEdge>(edge))
        PARAM_META_LOG("direct:\n");
      else
        PARAM_META_LOG("indirect:\n");

      VFGNode *succNode = edge->getSrcNode();
      PARAM_META_LOG("{}\n", succNode->toString());

      PARAM_META_LOG("[OUT EDGES]\n");
      for (VFGNode::const_iterator it = vNode->OutEdgeBegin(),
                                   eit = vNode->OutEdgeEnd();
           it != eit; ++it) {
        VFGEdge *edge = *it;

        if (SVFUtil::isa<SVF::DirectSVFGEdge>(edge))
          PARAM_META_LOG("direct:\n");
        else
          PARAM_META_LOG("indirect:\n");

        VFGNode *succNode = edge->getDstNode();
        PARAM_META_LOG("{}\n", succNode->toString());
      }
    }
  }

  auto type_inference = module_set->getTypeInference();
  auto param_type = type_inference->inferObjType(val);
  std::string buffer;
  raw_string_ostream os(buffer);
  param_type->print(os);
  MY_EX_LOG("Inferred Type of paramter: {}\n", buffer);

  access_path_t path;
  path.base_ty = param_type;

  // val will be an formal argument when starting.
  //
  FILOWorkList<const SVFGNode *> worklist;
  worklist.push(vNode);
  Set<const SVFGNode *> visited;

  while (!worklist.empty()) {
    auto v = worklist.pop();
    if (visited.count(v))
      continue;
    visited.insert(v);
    MY_EX_LOG("Visited: {}\n", v->toString());
    my_proc(v, path);
    for (auto e : v->getOutEdges()) {
      auto u = e->getDstNode();
      worklist.push(u);
    }
  }

  // here i can output the results
  if (auto st = dyn_cast<StructType>(path.base_ty)) {
    MY_EX_LOG("There are {} indices and {} field type(s)\n",
              path.indices.size(), path.field_ty.size());
    for (int i = 0; i < path.indices.size(); ++i) {
      string buffer;
      raw_string_ostream os(buffer);
      st->print(os);
      string type_buffer;
      raw_string_ostream os1(type_buffer);
      if (path.field_ty.size() > i)
        path.field_ty[i]->print(os);
      MY_EX_LOG("Found accesss to {} at index {} with type {}\n", buffer,
                path.indices[i], type_buffer);
    }
  }

  for (PAGEdge *edge : arg_node->getOutEdges()) {
    if (GepStmt *gepEdge = SVFUtil::dyn_cast<GepStmt>(edge)) {
      auto tst = gepEdge->getAccessPath();
      MY_EX_LOG("{}\n", tst.dump());
    }
  }

  return res;
}

ValueMetadata extractParameterMetadata(const SVFG &vfg, const Value *val,
                                       const Type *seek_type,
                                       unsigned paramId) {
  llvm::TimeTraceScope TimeScope("extractParameterMetadata", [val]() {
    if (auto *Inst = llvm::dyn_cast<llvm::Instruction>(val)) {
      return Inst->getFunction()->getName().str();
    }
    return val->getName().str();
  });
  SVFIR *pag = SVFIR::getPAG();

  LLVMModuleSet *llvmModuleSet = LLVMModuleSet::getLLVMModuleSet();

  // PointerAnalysis *pta = vfg.getPTA();

  // some types I might need later
  LLVMContext &cxt = LLVMModuleSet::getLLVMModuleSet()->getContext();
  // auto i8ptr_typ = PointerType::getInt8PtrTy(cxt);

  // auto llvm_val = llvmModuleSet->getValueNode(val);
  NodeID llvm_val = paramId;

  PAGNode *pNode = pag->getGNode(llvm_val);
  if (!vfg.hasDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pNode))) {
    ValueMetadata mdata_empty;
    return mdata_empty;
  }

  ValueMetadata mdata;
  mdata.setValue(val);

  // need a stack -> FILO
  // let S be a stack
  std::vector<Path> worklist;
  std::set<Path> visited;
  // S.push(v)
  // worklist.push_back(Path(vNode));
  worklist.push_back(Path(vfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pNode)),
                          val, seek_type));

  // if (seek_type)
  //     outs() << "DEBUG: seek_type: " << *seek_type << "\n";

  // outs() << "DEBUG: val: " << *val << "\n";

  AccessTypeSet *ats = mdata.getAccessTypeSet();
  bool is_array = false;

  // std::set<std::string> visitedFunctions;
  bool continue_debug = false;

  /// Traverse along VFG
  // while S is not empty do

  uint64_t total_visited_lookup_ns = 0;
  uint64_t total_visited_insert_ns = 0;
  uint64_t total_switch_ns = 0;
  uint64_t total_out_edges_ns = 0;

  uint64_t total_edge_path_copy_ns = 0;
  uint64_t total_edge_call_ns = 0;
  uint64_t total_edge_ret_ns = 0;
  uint64_t total_edge_worklist_ns = 0;

  auto loop_start_time = std::chrono::high_resolution_clock::now();

  while (!worklist.empty()) {
    // v = S.pop()
    Path p = worklist.back();
    worklist.pop_back();

    const VFGNode *vNode = p.getNode();
    AccessType acNode = p.getAccessType();

    // visitedFunctions.insert(vNode->getFun()->getName());

    if (config_t::instance()->debug) {
      PARAM_META_LOG("Working node:\n");
      PARAM_META_LOG("A.-> {}\n", vNode->toString());
      PARAM_META_LOG("B.-> {}\n", vNode->getFun()->getName());
      PARAM_META_LOG("AT: {}\n", to_string(acNode));
      // PARAM_META_LOG("Stack size: {}\n", p.getStackSize());

      if (to_string(acNode).rfind(config_t::instance()->debug_condition, 0) ==
          std::string::npos) {
        PARAM_META_LOG("[STOP]\n");
        for (auto h : p.getSteps()) {
          PARAM_META_LOG("{}\n", h.first->toString());
          PARAM_META_LOG("{}\n", h.first->getFun()->getName());
          PARAM_META_LOG("{}\n\n", to_string(h.second));
        }

        PARAM_META_LOG("-> last node <-\n");
        PARAM_META_LOG("{}\n", vNode->toString());
        PARAM_META_LOG("{}\n", vNode->getFun()->getName());
        PARAM_META_LOG("{}\n\n", to_string(acNode));

        PARAM_META_LOG("[IN EDGES]\n");
        for (VFGNode::const_iterator it = vNode->InEdgeBegin(),
                                     eit = vNode->InEdgeEnd();
             it != eit; ++it) {
          VFGEdge *edge = *it;

          if (SVFUtil::isa<SVF::DirectSVFGEdge>(edge))
            PARAM_META_LOG("direct:\n");
          else
            PARAM_META_LOG("indirect:\n");

          VFGNode *succNode = edge->getSrcNode();
          PARAM_META_LOG("{}\n", succNode->toString());
        }

        PARAM_META_LOG("[OUT EDGES]\n");
        for (VFGNode::const_iterator it = vNode->OutEdgeBegin(),
                                     eit = vNode->OutEdgeEnd();
             it != eit; ++it) {
          VFGEdge *edge = *it;

          if (SVFUtil::isa<SVF::DirectSVFGEdge>(edge))
            PARAM_META_LOG("direct:\n");
          else
            PARAM_META_LOG("indirect:\n");

          VFGNode *succNode = edge->getDstNode();
          PARAM_META_LOG("{}\n", succNode->toString());
        }

        exit(1);
      }
    }

    bool not_visited = false;
    {
      llvm::TimeTraceScope TimeScope("Visited Set Lookup");
      auto t_start = std::chrono::high_resolution_clock::now();
      not_visited = visited.find(p) == visited.end();
      auto t_end = std::chrono::high_resolution_clock::now();
      total_visited_lookup_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
              .count();
    }

    // if v is not labeled as discovered then
    if (not_visited) {

      // outs() << "Process:\n";
      // outs() << vNode->toString() << "\n";

      // label v as discovered
      {
        llvm::TimeTraceScope TimeScope("Visited Set Insert");
        auto t_start = std::chrono::high_resolution_clock::now();
        visited.insert(p);
        auto t_end = std::chrono::high_resolution_clock::now();
        total_visited_insert_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_end -
                                                                 t_start)
                .count();
      }

      bool skipNode = false;

      {
        llvm::TimeTraceScope TimeScope("Node Type Switch");
        // Processing of the node
        auto t_start = std::chrono::high_resolution_clock::now();
        switch (vNode->getNodeKind()) {
        case VFGNode::VFGNodeK::Load: {
          acNode.set_kind(AccessType::kind_e::read);
          ats->insert(acNode, vNode->getICFGNode());
        } break;
        case VFGNode::VFGNodeK::Store: {
          auto *prevValue = p.getPrevValue();

          auto llvm_val = llvmModuleSet->getLLVMValue(vNode->getValue());

          if (prevValue != nullptr && SVFUtil::isa<StoreInst>(llvm_val)) {
            auto inst = SVFUtil::cast<StoreInst>(llvm_val);

            if (inst->getPointerOperand() == prevValue)
              acNode.set_kind(AccessType::kind_e::write);
            else if (inst->getValueOperand() == prevValue)
              acNode.set_kind(AccessType::kind_e::read);

            ats->insert(acNode, vNode->getICFGNode());

            if (vNode->hasIncomingEdge()) {
              for (auto it : vNode->getInEdges()) {
                if (auto node =
                        SVFUtil::dyn_cast<AddrVFGNode>(it->getSrcNode())) {
                  if (auto call = SVFUtil::dyn_cast<llvm::CallInst>(
                          llvmModuleSet->getLLVMValue(node->getValue()))) {
                    llvm::Function *callee = call->getCalledFunction();
                    if (callee && callee->getName() == "malloc") {
                      // no need to set field, empty field set is what I need
                      acNode.set_kind(AccessType::kind_e::create);
                      mdata.getAccessTypeSet()->insert(acNode,
                                                       vNode->getICFGNode());
                      HANDLER_LOG("Function {} has a malloc return value",
                                  vNode->getICFGNode()->getFun()->getName());
                    }
                  }
                }
              }
            }
          }
        } break;
        case SVF::VFGNode::VFGNodeK::Gep:
          skipNode = handleGep(vNode, acNode, *ats, mdata, p);
          break;
        case VFGNode::VFGNodeK::Copy: {
          if (auto stmt_vfg_node =
                  SVFUtil::dyn_cast<StmtVFGNode>(vNode->getValue())) {
            // this is for ptrtoint instructions.
            COPY_LOG("{}\n", vNode->toString());
            auto inst = llvmModuleSet->getLLVMValue(stmt_vfg_node);
            // auto inst = SVFUtil::dyn_cast<GetElementPtrInst>(lllvm_inst);

            // auto inst = SVFUtil::dyn_cast<Instruction>(vNode->getValue());

            acNode.set_kind(AccessType::kind_e::read);
            ats->insert(acNode, vNode->getICFGNode());

            // XXX: casting operations complitate things a lot. For the time
            // being I just leave it.

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
          ats->insert(acNode, vNode->getICFGNode());
          break;
        case SVF::VFGNode::VFGNodeK::BinaryOp:
          acNode.set_kind(AccessType::kind_e::read);
          ats->insert(acNode, vNode->getICFGNode());
          break;
        case SVF::VFGNode::VFGNodeK::AParm:
          handleActualParam(vNode, acNode, mdata, p);
          break;
        } // end switch statement
        auto t_end = std::chrono::high_resolution_clock::now();
        total_switch_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                               t_end - t_start)
                               .count();
      } // end Node Type Switch scope

      // if (Instruction::isCast(inst->getOpcode()))
      //     skipNode = true;
      // else if (vNode->getNodeKind() == VFGNode::VFGNodeK::FRet) {
      //     // outs() << "[INFO] I found a FormalRet\n";
      //     // outs() << vNode->toString() << "\n";
      //     acNode.set_kind(AccessType::Access::ret);
      //     ats->insert(acNode, vNode->getICFGNode());
      // }

      if (skipNode) {
        GEP_LOG("Skipping node: {}", vNode->toString());
        continue;
      }

      p.setAccessType(acNode);
      if (vNode->getValue() == nullptr)
        p.setPrevValue(nullptr);
      else
        p.setPrevValue(llvmModuleSet->getLLVMValue(vNode->getValue()));

      if (vNode->hasOutgoingEdge()) {
        llvm::TimeTraceScope TimeScope("Outgoing Edges Traversal");
        auto t_start = std::chrono::high_resolution_clock::now();
        // outs() << "Children of: \n";
        // outs() << vNode->toString() << "\n";
        for (VFGNode::const_iterator it = vNode->OutEdgeBegin(),
                                     eit = vNode->OutEdgeEnd();
             it != eit; ++it) {
          VFGEdge *edge = *it;

          // VFGNode *succNode2 = edge->getDstNode();
          //  outs() << "INSPECT?: " << succNode2->toString() << "\n";

          // follow indirect jumps if a store or IntraMSSA
          // probably add a flag
          // TODO: Whats up with this code?
          if (vNode->getNodeKind() != VFGNode::VFGNodeK::Store &&
              vNode->getNodeKind() != VFGNode::VFGNodeK::MIntraPhi) {
            // try to follow only Direct Edges
            if (SVFUtil::isa<SVF::IndirectSVFGEdge>(edge)) {
              // VFGNode* succNode2 = edge->getDstNode();
              // outs() << "SKIP: " << succNode2->toString() << "\n";
              continue;
            }
          }
          // outs() << "I PROCEED WITH THIS\n";

          VFGNode *succNode = edge->getDstNode();
          // Add the current ICFGNode to the history of the path
          auto t_path_start = std::chrono::high_resolution_clock::now();
          Path p_succ = p;
          p_succ.addStep(vNode->getICFGNode());
          auto t_path_end = std::chrono::high_resolution_clock::now();
          total_edge_path_copy_ns +=
              std::chrono::duration_cast<std::chrono::nanoseconds>(t_path_end -
                                                                   t_path_start)
                  .count();

          bool ok_continue = true;

          const CallICFGNode *cs = nullptr;
          bool isACall = false;

          if (auto call_node = SVFUtil::dyn_cast<ActualParmVFGNode>(succNode)) {
            cs = call_node->getCallSite();
            HANDLER_LOG("ActualParmVFGNode {}\n", succNode->toString());
            isACall = true;
          } else if (auto call_node =
                         SVFUtil::dyn_cast<ActualINSVFGNode>(succNode)) {
            cs = call_node->getCallSite();
            // Call MU
            HANDLER_LOG("ActualINSVFGNode {}\n", succNode->toString());
            isACall = true;
          } else if (auto ret_node =
                         SVFUtil::dyn_cast<ActualRetVFGNode>(succNode)) {
            cs = ret_node->getCallSite();
            isACall = false;
          } else if (auto ret_node =
                         SVFUtil::dyn_cast<ActualOUTSVFGNode>(succNode)) {
            // Call CHI
            cs = ret_node->getCallSite();
            isACall = false;
          } else if (auto addr_node =
                         SVFUtil::dyn_cast<AddrVFGNode>(succNode)) {
            if (auto *inst = llvm::dyn_cast<llvm::Instruction>(
                    llvmModuleSet->getLLVMValue(addr_node->getValue()))) {
              if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
                llvm::Function *callee = call->getCalledFunction();
                if (callee && callee->getName() == "malloc") {
                  HANDLER_LOG("Found a malloc in an AddrVFGNode\n");
                }
              }
            }
          }

          auto t_call_start = std::chrono::high_resolution_clock::now();
          if (cs && isACall) {
            p_succ.pushFrame(cs);
            HANDLER_LOG("The callstack now has {} entries.",
                        p_succ.getStackSize());
            if (p_succ.getStackSize() >= MAX_STACKSIZE) {
              HANDLER_LOG("STACK SIZE REACHED\n");
              ok_continue = false;
              // outs() << "[INFO] Stack size too big!\n";
            } else if (!config_t::instance()->consider_indirect_calls &&
                       cs->isIndirectCall()) {
              HANDLER_LOG("Indirect Call: {}\n", cs->toString());
              ok_continue = false;
              // outs() << "[INFO] Indirect call, I stop!\n";
              // it is a direct call, check for stubs
            } else {
              if (!cs->isIndirectCall()) {
                // outs() << "[INFO] ActualParmVFGNode:\n";
                HANDLER_LOG("Direct Call: Function {} calling {} with {} "
                            "parameters.\n {}\n",
                            cs->getCaller()->getName(),
                            cs->getCalledFunction()->getName(),
                            cs->getActualParms().size(), cs->toString());
                std::string fun = cs->getCalledFunction()->getName();
                bool can_handle_parameter = false;

                SVF::PAGNode *param = nullptr;
                if (auto call_node =
                        SVFUtil::dyn_cast<ActualParmVFGNode>(succNode)) {
                  param = const_cast<SVF::ValVar *>(call_node->getParam());
                  can_handle_parameter = true;
                } else if (auto call_node =
                               SVFUtil::dyn_cast<FormalParmVFGNode>(succNode)) {
                  param = const_cast<SVF::ValVar *>(call_node->getParam());
                  can_handle_parameter = true;
                  // } else {
                  //     outs() << "it is none!!\n";
                }

                // outs() << "succ node:\n";
                // outs() << succNode->toString() << "\n";

                if (can_handle_parameter) {
                  assert(param && "Param not found!\n");

                  int n_param = 0;
                  for (auto p : cs->getActualParms()) {
                    if (p == param)
                      break;
                    n_param++;
                  }
                  HANDLER_LOG("Parameter index: {}", n_param);

                  ok_continue =
                      handlerDispatcher(&mdata, fun, vNode->getICFGNode(), cs,
                                        n_param, acNode, C_PARAM, &p);
                }
              }
            }
          }
          auto t_call_end = std::chrono::high_resolution_clock::now();
          total_edge_call_ns +=
              std::chrono::duration_cast<std::chrono::nanoseconds>(t_call_end -
                                                                   t_call_start)
                  .count();

          // aka is a ret
          auto t_ret_start = std::chrono::high_resolution_clock::now();
          if (cs && !isACall) {
            ok_continue = p_succ.isCorrect(cs);
            if (ok_continue) {
              if (cs->getCaller() && cs->getCalledFunction())
                HANDLER_LOG("Returning to matching function. Caller: {} and "
                            "callee: {}\n",
                            cs->getCaller()->getName(),
                            cs->getCalledFunction()->getName());
              p_succ.popFrame();
              HANDLER_LOG("Call Stack back to only {} entries.\n",
                          p_succ.getStackSize());
            }
          }
          auto t_ret_end = std::chrono::high_resolution_clock::now();
          total_edge_ret_ns +=
              std::chrono::duration_cast<std::chrono::nanoseconds>(t_ret_end -
                                                                   t_ret_start)
                  .count();

          auto t_wl_start = std::chrono::high_resolution_clock::now();
          if (ok_continue) {
            p_succ.setNode(succNode);
            worklist.push_back(p_succ);
          }
          auto t_wl_end = std::chrono::high_resolution_clock::now();
          total_edge_worklist_ns +=
              std::chrono::duration_cast<std::chrono::nanoseconds>(t_wl_end -
                                                                   t_wl_start)
                  .count();
        } // end out edge processing
        auto t_end = std::chrono::high_resolution_clock::now();
        total_out_edges_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_end -
                                                                 t_start)
                .count();
      } // if (hasOutgoingEdges)
      // else {
      //     outs() << "I HAVE NOT OUT EDGES!\n";
      // }
    } // end if (visited.find(...))
    else {
      HANDLER_LOG("Node already visited: {}\n", vNode->toString());
    }
  }

  // outs() << "I visited these functions:\n";
  // for (auto x: visitedFunctions) {
  //     outs() << x << "\n";
  // }

  if (!mdata.isArray()) {
    mdata.setIsArray(is_array);
  }

  auto loop_end_time = std::chrono::high_resolution_clock::now();
  uint64_t total_loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               loop_end_time - loop_start_time)
                               .count();
#if defined(PROFILING)
  if (total_loop_ns > 0) {
    outs() << "\n[PROFILING] `extractParameterMetadata` Time Breakdown:\n";
    outs() << "  Total Loop Time: " << (total_loop_ns / 1e6) << " ms\n";
    outs() << "  Visited Lookup : " << (total_visited_lookup_ns / 1e6)
           << " ms ("
           << ((double)total_visited_lookup_ns / total_loop_ns) * 100.0
           << "%)\n";
    outs() << "  Visited Insert : " << (total_visited_insert_ns / 1e6)
           << " ms ("
           << ((double)total_visited_insert_ns / total_loop_ns) * 100.0
           << "%)\n";
    outs() << "  Node Switch    : " << (total_switch_ns / 1e6) << " ms ("
           << ((double)total_switch_ns / total_loop_ns) * 100.0 << "%)\n";
    outs() << "  Edge Traversal : " << (total_out_edges_ns / 1e6) << " ms ("
           << ((double)total_out_edges_ns / total_loop_ns) * 100.0 << "%)\n";
    outs() << "    - Path Copy  : " << (total_edge_path_copy_ns / 1e6)
           << "ms\n";
    outs() << "    - Call Check : " << (total_edge_call_ns / 1e6) << "ms\n";
    outs() << "    - Ret Check  : " << (total_edge_ret_ns / 1e6) << "ms\n";
    outs() << "    - Worklist   : " << (total_edge_worklist_ns / 1e6) << "ms\n";
    outs() << "    - Unmeasured Overhead: "
           << ((total_out_edges_ns - total_edge_path_copy_ns -
                total_edge_call_ns - total_edge_ret_ns -
                total_edge_worklist_ns) /
               1e6)
           << " ms\n";
    outs() << "  Other (Wait)   : "
           << ((total_loop_ns - total_visited_lookup_ns -
                total_visited_insert_ns - total_switch_ns -
                total_out_edges_ns) /
               1e6)
           << " ms\n\n";
  }
#endif

  return mdata;
}
} // namespace liberator
