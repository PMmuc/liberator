#include "AccessType.h"
#include "AccessTypeHandler.h"
#include "AccessTypeIO.h"
#include "Config.h"
#include "IntraAnalysis.hpp"
#include "ValueMetadata.hpp"

#include <SVFIR/SVFValue.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>

#include "Graphs/ICFGNode.h"
#include "Graphs/IRGraph.h"
#include "Graphs/SVFG.h"
#include "PhiFunction.h"
#include "SVF-LLVM/BasicTypes.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "Util/Casting.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"
#include <Graphs/ICFGEdge.h>
#include <Graphs/SCC.h>
#include <Graphs/SVFGNode.h>
#include <Graphs/VFGNode.h>
#include <MSSA/SVFGBuilder.h>
#include <Util/GeneralType.h>
#include <Util/WorkList.h>
#include <chrono>
#include <iostream>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/TypedPointerType.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/TargetParser/Triple.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <unordered_map>
#include <unordered_set>

#define MAX_STACKSIZE 20
static constexpr unsigned int MAX_SLICE_NODES = 10000;

static constexpr unsigned int MAX_FIELD_DEPTH = 6;

// Widening bound for the SCC fixpoint in process_scc().
//
// The AccessType lattice IS finite - MAX_FIELD_DEPTH bounds path length - but
// it is astronomically large: with a field alphabet of ~30 and depth 6 it
// admits ~10^10 distinct field vectors, so finiteness gives no practical
// termination guarantee. Large mutually recursive SCCs keep composing new
// paths for many rounds; c-ares SCC 153 (256 functions, 690 formals) grows
// ~4x per round and was still climbing past 7 rounds and hundreds of millions
// of AccessTypes.
//
// The type-agreement guard in merge_access_type() removes the provably
// spurious compositions, but ~45% of compositions on that SCC are
// type-UNDECIDABLE (DWARF lost along the path, LLVM type opaque under LLVM 16
// opaque pointers) and those alone sustain the growth. So a bound is needed
// on top of precision.
//
// Cutting the iteration off keeps the summaries computed so far. That is an
// under-approximation of the SCC's true effects - accesses that only appear
// after deeper mutual recursion are missed - so the truncation is reported
// rather than applied silently.
static constexpr int MAX_FIXPOINT_ITERATIONS = 3;

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
/**
 * @return: the node that has no predecessors.
 */
std::set<const VFGNode *> getDefinitionSetForRet(const VFGNode *,
                                                 std::set<const FunObjVar *> &);
// bool leadsToBitCastOfType(const ICFGNode*,Type*,const Instruction**);
bool leadsToBitCastOfType(const VFGNode *, Type *);
bool areCompatible(FunctionType *, FunctionType *);
// NOT EXPOSED FUNCTIONS -- END!

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

std::set<const FunObjVar *> ind_collected_functions;
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

//
namespace liberator {

bool AccessType::equals(std::string s) const {
  return s == to_string(*this, false);
}

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
      return;
    }
    const SVFVar *var = stmt->getValue();
    const auto llvminst = llvmModuleSet->getLLVMValue(var);

    if (auto alloca = SVFUtil::dyn_cast<AllocaInst>(llvminst)) {
      INTRA_LOG("alloca {}\n", alloca->getName());
      // FIXME: use dwarf debug information
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

  // is the return type a global or a constant expression
  if (doesReturnGlobalVarConst(fun_exit)) {
    AccessType acNodeConst(retType);
    addWrteToAllFields(mdata, acNodeConst, fun_exit);
    return mdata;
  }

  PHIFun phi;
  PHIFunInv phi_inv;
  get_phi_function(svfModule, icfg, phi, phi_inv);

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

  AccessTypeSet &ats = mdata.get_access_type_set();

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
            const std::string &fun = t->getName();
            // malloc handler
            AccessType acNode(retType);
            // Not that mdata_tmp will not be used and just thrown away
            ValueMetadata mdata_tmp;
            handlerDispatcher(mdata_tmp, fun, node, call_node, -1, acNode,
                              C_RETURN, nullptr);
            RETURN_LOG("[INFO] After handlerDispatcher\nmeta_tmp:\n");
            RETURN_LOG("{}\n", to_string(mdata_tmp, false));

            bool added_create = false;
            for (auto at : mdata_tmp.get_access_type_set()) {
              if (at.get_kind() == AccessType::kind_e::create) {
                outs() << "I added a create!!!\n";
                added_create = true;
                break;
              }
            }
            auto ret_node = call_node->getRetICFGNode();
            const SVFVar *ret_node_val = ret_node->getActualRet();

            // basically what this code does, is it
            // finds out if the callnode is a function pointer
            // call. If it is it finds if the called function
            // is a malloc, if it is a malloc it finds out
            // if the function the return value leads to a bitcast.
            // and for that bitcast it checks if the return type
            // is the same as the return type of the caller.
            // can be null if the function does not return anything (void)
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
                handlerDispatcher(mdata, fun, node, call_node, -1, acNode,
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
          handlerDispatcher(mdata, fun, node, call_node, -1, acNode, C_RETURN,
                            nullptr);

          for (unsigned p = 0; p < ftype->getNumParams(); p++) {
            handlerDispatcher(mdata, fun, node, call_node, p, acNode, C_RETURN,
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
    for (auto at : mdata.get_access_type_set()) {
      // for (auto at: *mdata_xx.get_access_type_set()) {
      if (at.get_kind() == AccessType::kind_e::ret) {
        auto l_ats_all_nodes = at.getICFGNodes();
        for (auto inst : l_ats_all_nodes) {
          if (inst == fun_exit) {
            for (auto at2 : mdata.get_access_type_set())
              // for (auto at2: *mdata_xx.get_access_type_set())
              for (auto inst2 : at.getICFGNodes())
                ats.insert(at2, inst2);
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
    for (const AccessType &atx : el.second.get_access_type_set())
      for (auto inst : atx.getICFGNodes())
        ats.insert(atx, inst);
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
  /*
   if (total_func_ns > 0) {
     outs() << "\n[PROFILING] `extractReturnMetadata` Time Breakdown:\n";
     outs() << "  Total Function Time: " << (total_func_ns / 1e6) << " ms\n";
     outs() << "  1. Main ICFG Loop  : " << (total_main_loop_ns / 1e6) << " ms
   ("
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
 */
  return mdata;
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

/**
 * Find the root nodes for every element in sources.
 * @param sources - set of nodes the roots have to be found
 * @param on_root - lambda function called when root was found with the root as
 * parameter.
 */
template <typename T>
static void find_definitions(llvm::ArrayRef<const VFGNode *> sources,
                             T on_root) {
  unordered_set<const VFGNode *> visited;
  vector<const VFGNode *> worklist;
  for (const VFGNode *s : sources) {
    if (visited.insert(s).second)
      worklist.push_back(s);

    while (!worklist.empty()) {
      const VFGNode *curr = worklist.back();
      worklist.pop_back();

      // root found
      if (curr->getInEdges().empty()) {
        if (on_root(curr))
          return;
        continue;
      }
      for (const VFGEdge *in : curr->getInEdges()) {
        const VFGNode *parent = in->getSrcNode();
        if (visited.insert(parent).second)
          worklist.push_back(parent);
      }
    }
  }
}

vector<string>
my_extract_dependency_among_parameters(const SVF::SVFVar *current_param,
                                       ValueMetadata &mdata, SVFG &svfg,
                                       const FunObjVar *fun) {
  SVFIR *pag = SVFIR::getPAG();

  if (!pag->hasFunArgsList(fun))
    return {};

  const auto &fun_params = pag->getFunArgsList(fun);

  // (root_node -> index of parameter it corresponds to)
  unordered_map<const VFGNode *, llvm::SmallVector<unsigned, 2>> root_to_params;

  unsigned num_candidates = 0;
  unsigned p_idx = 0;

  for (const SVFVar *p : fun_params) {
    const unsigned idx = p_idx++;
    if (p == current_param)
      continue;

    const auto p_var = SVFUtil::dyn_cast<ValVar>(p);

    if (!p_var || !svfg.hasDefSVFGNode(p_var)) {
      continue;
    }

    ++num_candidates;
    find_definitions({svfg.getDefSVFGNode(p_var)}, [&](const VFGNode *root) {
      root_to_params[root].push_back(idx);
      return false;
    });
  }
  if (root_to_params.empty())
    return {};

  // Unique stored write accesses.
  std::vector<const VFGNode *> stored_values;
  std::unordered_set<const VFGNode *> visited;

  for (const AccessType &at : mdata.get_access_type_set()) {
    if (at.get_kind() != AccessType::kind_e::write)
      continue;
    for (const ICFGNode *node : at.getICFGNodes()) {
      const auto *intra = SVFUtil::dyn_cast<IntraICFGNode>(node);
      if (!intra)
        continue;
      for (const SVFStmt *s : intra->getSVFStmts()) {
        const auto *st = SVFUtil::dyn_cast<StoreStmt>(s);
        if (!st)
          continue;

        // the operand that gets stored
        const auto *src_var = dyn_cast<ValVar>(st->getRHSVar());
        if (!src_var || !svfg.hasDefSVFGNode(src_var))
          continue;
        // parameter that gets stored
        const VFGNode *src_node = svfg.getDefSVFGNode(src_var);
        if (visited.insert(src_node).second)
          stored_values.push_back(src_node);
      }
    }
  }

  vector<bool> found(p_idx, false);
  unsigned num_found = 0;
  find_definitions(stored_values, [&](const VFGNode *root) {
    auto it = root_to_params.find(root);
    if (it == root_to_params.end())
      return false;

    for (unsigned idx : it->second) {
      if (!found[idx]) {
        found[idx] = true;
        ++num_found;
      }
    }
    return num_found == num_candidates;
  });

  std::set<std::string> set_by;
  for (unsigned i = 0; i < found.size(); ++i) {
    if (found[i])
      set_by.insert("param_" + std::to_string(i));
  }

  return {set_by.begin(), set_by.end()};
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

  AccessTypeSet &ats = mdata.get_access_type_set();
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
    AccessType acNode = p.get_access_type();

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
          ats.insert(acNode, vNode->getICFGNode());
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

            ats.insert(acNode, vNode->getICFGNode());

            if (vNode->hasIncomingEdge()) {
              for (auto it : vNode->getInEdges()) {
                if (auto node =
                        SVFUtil::dyn_cast<AddrVFGNode>(it->getSrcNode())) {
                  if (auto call = SVFUtil::dyn_cast<llvm::CallInst>(
                          llvmModuleSet->getLLVMValue(node->getValue()))) {
                    llvm::Function *callee = call->getCalledFunction();
                    if (callee && callee->getName() == "malloc") {
                      // no need to set field, empty field set is what I
                      // need
                      acNode.set_kind(AccessType::kind_e::create);
                      mdata.get_access_type_set().insert(acNode,
                                                         vNode->getICFGNode());
                    }
                  }
                }
              }
            }
          }
        } break;
        case SVF::VFGNode::VFGNodeK::Gep:
          skipNode = handleGep(vNode, acNode, ats, mdata, p);
          break;
        case VFGNode::VFGNodeK::Copy: {
          if (auto stmt_vfg_node =
                  SVFUtil::dyn_cast<StmtVFGNode>(vNode->getValue())) {
            // this is for ptrtoint instructions.
            COPY_LOG("{}\n", vNode->toString());
            auto inst = llvmModuleSet->getLLVMValue(stmt_vfg_node);
            // auto inst = SVFUtil::dyn_cast<GetElementPtrInst>(lllvm_inst);

            // auto inst =
            // SVFUtil::dyn_cast<Instruction>(vNode->getValue());

            acNode.set_kind(AccessType::kind_e::read);
            ats.insert(acNode, vNode->getICFGNode());

            // XXX: casting operations complitate things a lot. For the time
            // being I just leave it.

            if (auto bitcastinst = SVFUtil::dyn_cast<BitCastInst>(inst)) {
              auto dst_typ = bitcastinst->getDestTy();
              auto src_typ = bitcastinst->getSrcTy();

              // if (acNode.getNumFields() != 0 &&
              //     TypeMatcher::compare_types(src_typ, acNode.getType()))
              //     {

              // outs() << "src_typ " << *src_typ << "\n";
              // outs() << "acNode.getType() " << *acNode.getType() << "\n";

              // if (TypeMatcher::compare_types(src_typ, acNode.getType()))
              // {
              //     // I want the node the original type after the cast
              //     this
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
          ats.insert(acNode, vNode->getICFGNode());
          break;
        case SVF::VFGNode::VFGNodeK::BinaryOp:
          acNode.set_kind(AccessType::kind_e::read);
          ats.insert(acNode, vNode->getICFGNode());
          break;
        case SVF::VFGNode::VFGNodeK::AParm:
          handleActualParam(vNode, acNode, mdata, p);
          break;
        } // end switch statement
        auto t_end = std::chrono::high_resolution_clock::now();
        total_switch_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                               t_end - t_start)
                               .count();
      } // anonymous block

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

      p.set_access_type(acNode);
      if (vNode->getValue() == nullptr)
        p.setPrevValue(nullptr);
      else
        // has corresponding llvm value
        p.setPrevValue(llvmModuleSet->getLLVMValue(vNode->getValue()));

      if (vNode->hasOutgoingEdge()) {
        llvm::TimeTraceScope TimeScope("Outgoing Edges Traversal");
        auto t_start = std::chrono::high_resolution_clock::now();
        // outs() << "Children of: \n";
        // outs() << vNode->toString() << "\n";
        for (VFGNode::const_iterator it = vNode->OutEdgeBegin(),
                                     eit = vNode->OutEdgeEnd();
             it != eit; ++it) { // start out edge processing
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
            isACall = true;
          } else if (auto call_node =
                         SVFUtil::dyn_cast<ActualINSVFGNode>(succNode)) {
            cs = call_node->getCallSite();
            // Call MU
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
                      handlerDispatcher(mdata, fun, vNode->getICFGNode(), cs,
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

          // A Path's identity is (VFGNode, fields, kind, type), so an
          // unbounded GEP chain multiplies the search space per node instead
          // of visiting it once. MAX_GEP_RECURSION_DEPTH only bounds how
          // often a single (aggregate type, field) repeats along a path, not
          // the overall length, so bound the length here with the same limit
          // merge_access_type() applies when composing summaries.
          if (ok_continue && p_succ.get_access_type().get_num_fields() >
                                 static_cast<int>(MAX_FIELD_DEPTH)) {
            HANDLER_LOG("FIELD DEPTH REACHED\n");
            ok_continue = false;
          }

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
      } // end if (hasOutgoingEdges)
    } // not visited
  } // end if (!worklist.empty())

  // outs() << "I visited these functions:\n";
  // for (auto x: visitedFunctions) {
  //     outs() << x << "\n";
  // }

  auto loop_end_time = std::chrono::high_resolution_clock::now();
  uint64_t total_loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               loop_end_time - loop_start_time)
                               .count();
  /*#if defined(PROFILING)
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
             << ((double)total_out_edges_ns / total_loop_ns) * 100.0 <<
  "%)\n"; outs() << "    - Path Copy  : " << (total_edge_path_copy_ns / 1e6)
             << "ms\n";
      outs() << "    - Call Check : " << (total_edge_call_ns / 1e6) << "ms\n";
      outs() << "    - Ret Check  : " << (total_edge_ret_ns / 1e6) << "ms\n";
      outs() << "    - Worklist   : " << (total_edge_worklist_ns / 1e6) <<
  "ms\n"; outs() << "    - Unmeasured Overhead: "
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
  */

  return mdata;
}

} // namespace liberator
