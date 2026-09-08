#include "AccessType.h"
#include "AccessTypeHandler.h"
#include "AccessTypeIO.h"
#include "Config.h"
#include "DebugInfoParser.hpp"
#include "ValueMetadata.hpp"

#include <cstddef>
#include <llvm/Analysis/ValueTracking.h>

#include "Graphs/ICFGNode.h"
#include "Graphs/IRGraph.h"
#include "Graphs/SVFG.h"
#include "PhiFunction.h"
#include "SVF-LLVM/BasicTypes.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/ObjTypeInference.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/SVFVariables.h"
#include "TypeMatcher.h"
#include "Util/Casting.h"
#include "Util/SVFUtil.h"
#include "llvm/Demangle/Demangle.h"
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
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/TypedPointerType.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/TargetParser/Triple.h>
#include <queue>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <variant>
#include <vector>

#define MAX_STACKSIZE 20
// how many times the same (aggregate type, field) may be followed along a
// single path before we stop, to bound unrolling of recursive data structures.
#define MAX_GEP_RECURSION_DEPTH 2

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
/**
 * @retrun the function corresponding to a formal parameter
 */
const FunObjVar *get_function(const SVFG &svfg, SVF::NodeID param) {
  SVF::NodeID fid = param;
  auto *pag = SVFIR::getPAG();
  if (auto *pNode = pag->getGNode(param)) {
    if (auto *valVar = SVFUtil::dyn_cast<SVF::ValVar>(pNode)) {
      if (svfg.hasDefSVFGNode(valVar)) {
        fid = svfg.getDefSVFGNode(valVar)->getId();
      }
    }
  }
  const VFGNode *n = svfg.getGNode(fid);
  if (auto fn = dyn_cast<FormalParmVFGNode>(n)) {
    return fn->getFun();
  }
  return nullptr;
}

static bool is_addr_of(const SVF::ValVar *param) {
  if (!param)
    return false;

  const llvm::Value *v = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(param);

  if (!v)
    return false;
  if (llvm::isa<llvm::GetElementPtrInst>(v))
    return true;
  if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v))
    return ce->getOpcode() == llvm::Instruction::GetElementPtr;
  return false;
}

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
template <class T> inline void hash_combine(std::size_t &seed, const T &v) {
  seed ^= std::hash<T>{}(v) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}
} // namespace liberator

namespace std {
template <> struct hash<liberator::memo_state_t> {
  std::size_t operator()(const liberator::memo_state_t &k) const noexcept {
    std::size_t h = std::hash<SVF::NodeID>{}(k.node);
    liberator::hash_combine(h, static_cast<int>(k.access));
    liberator::hash_combine(h, k.type);
    for (int f : k.fields)
      liberator::hash_combine(h, f);
    return h;
  }
};
} // namespace std
  //
namespace liberator {

bool AccessType::equals(std::string s) const {
  return s == to_string(*this, false);
}

bool handlerDispatcher(liberator::ValueMetadata &, const std::string &,
                       const ICFGNode *, const CallICFGNode *, int, AccessType,
                       H_SCOPE h_scope, liberator::Path *path);
bool hasHandlerDispatcher(liberator::ValueMetadata *, const string &,
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

/**
 * This function walks the SVFG graph from a starting node
 * to the first FormalReturnVFGNode.
 */
bool reaches_formal_return(const VFGNode *node, const FunObjVar *fun) {
  set<const VFGNode *> visited;
  vector<const VFGNode *> worklist{node};
  while (!worklist.empty()) {
    auto curr = worklist.back();
    worklist.pop_back();
    if (visited.find(curr) == visited.end()) {
      continue;
    }
    visited.insert(curr);

    // a field is accessed -> just skip
    if (SVFUtil::isa<GepVFGNode>(curr))
      continue;
    // stay intraprocedural
    if (SVFUtil::isa<InterPHIVFGNode>(curr))
      continue;
    // make sure we stay the function we started
    if (curr->getFun() != fun)
      continue;
    if (SVFUtil::isa<FormalRetVFGNode>(curr))
      return true;
    for (auto succ : curr->getOutEdges()) {
      worklist.push_back(succ->getDstNode());
    }
  }
  return false;
}

static std::string describe_callee(const SVF::FunObjVar *f) {
  if (!f)
    return "<null>";
  std::string name = f->getName();

  const llvm::Function *llvm_fun = dyn_cast_or_null<llvm::Function>(
      LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(f));

  if (!llvm_fun)
    return name + " : no llvm fun";

  if (const DISubprogram *sub = llvm_fun->getSubprogram()) {
    if (llvm::DISubroutineType *di_sub = sub->getType()) {
      return name + " : " + liberator::to_string(di_sub);
    }
  }
  return name + " : no dwarf info";
}

/*ValueMetadata my_extract_return_metadata(const SVFG &vfg,
                                         const Value *llvmval) {
  auto *llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
  auto nodeid = llvm_module_set->getValueNode(llvmval);

  SVFIR *pag = SVFIR::getPAG();
  PAGNode *curr_node = pag->getGNode(nodeid);
  ICFG *icfg = pag->getICFG();

  ValueMetadata res;
  res.setValue(llvmval);

  const auto curr_fun = curr_node->getFunction();
  if (curr_fun->isDeclaration())
    return res;

  auto fun_exit = icfg->getFunExitICFGNode(curr_fun);
  const Function *llvm_fun =
      dyn_cast<Function>(llvm_module_set->getLLVMValue(curr_fun));
  // FIXME: replace with type debug information
  Type *ret_type = llvm_fun->getReturnType();

  if (doesReturnGlobalVarConst(fun_exit)) {
    // FIXME: add di type for ac_node
    AccessType ac_node(ret_type, );
    addWrteToAllFields(res, ac_node, fun_exit);
    return res;
  }

  set<ICFGNode *> visited;
  stack<ICFGEdge *> empty_stack;
  FunEntryICFGNode *entry_node = icfg->getFunEntryICFGNode(curr_fun);
  stack<pair<ICFGNode *, stack<ICFGEdge *>>> working;
  working.push(std::make_pair(entry_node, empty_stack));

  AccessTypeSet &ats = res.get_access_type_set();

  set<const Instruction *> allocainst_set;

  while (!working.empty()) {
    auto el = working.top();
    working.pop();
    ICFGNode *curr = el.first;
    stack<ICFGEdge *> curr_stack = el.second;

    if (auto intra_node = SVFUtil::dyn_cast<IntraICFGNode>(curr)) {
      handleIntraICFGNodes(intra_node, ret_type, allocainst_set);
    } else if (auto callsite_node = SVFUtil::dyn_cast<CallICFGNode>(curr)) {
      if (!config_t::instance()->consider_indirect_calls &&
          callsite_node->isIndirectCall())
        continue;

      auto stmt = callsite_node->getSVFStmts().front();
      const auto val = llvm_module_set->getLLVMValue(stmt->getValue());
      auto call_inst = dyn_cast<CallBase>(val);

      if (call_inst) {
        // FIXME: use dwarf debug information to get signature
        // of callee.
        bool ret_type_is_ok = false;
        if (di_callee_ret_type == di_ret_type) {
          allocainst_set.insert(call_inst);
          ret_type_is_ok = true;
        } else if (ValueMetadata::myCallEdgeMap_inst.find(callsite_node) !=
                   ValueMetadata::myCallEdgeMap_inst.end()) {
          auto &targets = ValueMetadata::myCallEdgeMap_inst[callsite_node];
          for (auto t : targets) {
            const string &fun = t->getName();
            if (hasHandlerDispatcher(&res, fun, curr, callsite_node, -1,
                                     C_RETURN)) {
              ret_type_is_ok = true;
            }
          }
        }

        if (ret_type_is_ok &&
            ValueMetadata::myCallEdgeMap_inst.find(callsite_node) !=
                ValueMetadata::myCallEdgeMap_inst.end()) {
          auto targets = ValueMetadata::myCallEdgeMap_inst[callsite_node];

          for (auto fun_ptr : targets) {
            const string &fun_name = fun_ptr->getName();
            // FIXME: add DWARF Debug information
            AccessType ac_node(ret_type);
            ValueMetadata mdata_tmp;
            handlerDispatcher(mdata_tmp, fun_name, curr, callsite_node, -1,
                              ac_node, C_RETURN, nullptr);

            bool added_create = false;
            for (auto &at : mdata_tmp.get_access_type_set()) {
              if (at.get_kind() == AccessType::kind_e::create) {
                added_create = true;
                break;
              }
            }

            if (added_create) {
              auto ret_node = callsite_node->getRetICFGNode();

              // this is the node in the caller,
              // after finishing executing call_node
              const SVFVar *ret_node_val = ret_node->getActualRet();

              // can be null if the function does not return anything (void)
              if (ret_node_val) {
                const Value *llvm_ret_val =
                    llvm_module_set->getLLVMValue(ret_node_val);
                PAGNode *pag_ret_node = pag->getGNode(ret_node_val->getId());
                if (vfg.hasDefSVFGNode(dyn_cast<ValVar>(pag_ret_node))) {
                  auto ret_node =
                      vfg.getDefSVFGNode(dyn_cast<ValVar>(pag_ret_node));
                  if (reaches_formal_return(ret_node, fun_ptr)) {
                    AccessType ac_node(ret_type, di_type);
                    handlerDispatcher(res, fun_name, curr, callsite_node, -1,
                                      ac_node, C_RETURN, nullptr);
                  }
                }
              }
            }
          }
        }
      }
    }

    for (auto edge : curr->getOutEdges()) {
      ICFGNode *dst = edge->getDstNode();
      if (visited.find(dst) != visited.end()) {
        if (auto call_edge : SVFUtil::dyn_cast<CallCFGEdge>(edge)) {
          ICFGEdge *next_ret = phi[call_edge];
        }
      }
    }
  }
}
*/
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

/**
It checks if the target function is handled by our dispatchers.

@param: ats: the access type set to be updated by the handler
@param: fun: the name of the function
@param: node: the node currently analyzed

@return: boolean value indicating if the function is handled by our
dispatchers
*/
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

  auto ats = mdata.get_access_type_set();
  auto ats_it = ats.begin();
  auto ats_end = ats.end();
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
      // constant array index
      auto fs = el.first;
      // path to that constant array index
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

        // can not be a length parameter -> skip
        if (SVFUtil::isa<PointerType>(llvm_p_val->getType())) {
          p_idx++;
          continue;
        }

        PAGNode *pP = pag->getGNode(p->getId());
        const VFGNode *vP = svfg.getDefSVFGNode(SVFUtil::cast<SVF::ValVar>(pP));
        // const_cast<llvm::Value*>(p->getValue());
        // outs() << "P: " << pP->toString() << "\n";
        // vS -> array index node
        // vP -> the length parameter in the signature
        // find if they have the same origin nodes
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
  case Type::StructTyID: {
    // Literal (anonymous) structs match StructTyID too, and getStructName()
    // asserts on them.
    const auto *st = llvm::cast<StructType>(t);
    std::string name = st->isLiteral() ? "<literal>" : st->getName().str();
    return "Struct " + name + " with size " + size_bytes;
  }
  case Type::ArrayTyID:
    return "Array " + size_bytes;
  }

  return "";
}

namespace {
// the result after applying the local function
// to a function. This determines the access_type
struct local_result_t {
  AccessType ac_node;
  const llvm::Value *prev_value;
  bool skip; // don't expand successors/prune search
};
} // namespace

// --- TEMPORARY INSTRUMENTATION: where does the DWARF type chain die? ---
//
// next_di_field() opens with `decay_di_type(di); if (!res) return nullptr;`
// and AccessType.cpp writes its result unconditionally, so once a step yields
// null every LATER step on that path starts from null and returns null again.
// Counting those two situations apart tells us whether the 45% undecidable
// compositions come from ONE upstream failure that poisons long paths (cheap
// to fix: keep the last known DIType) or from MANY independent local failures
// (not cheap: the trade-off has to be accepted instead).
struct di_chain_instr_t {
  size_t ok = 0;          // next_di_field returned a type
  size_t fresh_break = 0; // had a DIType on entry, lost it at this step
  size_t poisoned = 0;    // already null on entry - break was earlier
  // Where does the null actually enter the path?
  size_t formal_no_di = 0; // summarize_formal seeded a path with no DIType
  size_t formal_with_di = 0;
  size_t opaque_wildcard = 0; // handleGep:1476 accepted an arbitrary struct
                              // because the path type was an opaque pointer
  size_t compose_null_di = 0; // merge_access_type copied a null DI from suffix
  // Which phase feeds handleGep a path that already lost its DIType?
  size_t gep_bu_null = 0, gep_bu_ok = 0; // bottom-up  (compute_local_effect)
  size_t gep_td_null = 0,
         gep_td_ok = 0; // top-down (extractParameterMetadata)
};
static di_chain_instr_t di_instr;
static size_t di_break_samples = 0;

compose_instr_t &compose_instr_t::instance() {
  static compose_instr_t instance;
  return instance;
}

void compose_instr_t::dump(llvm::raw_ostream &os) const {
  os << "[COMPOSE] attempts=" << attempts << " di_match=" << di_match
     << " di_reject=" << di_reject << " llvm_match=" << llvm_match
     << " llvm_reject=" << llvm_reject << " undecidable=" << undecidable
     << " (absent=" << di_absent << " not_composite=" << di_not_composite
     << " unnamed=" << di_unnamed << ")\n";
}
struct addr_of_instr_t {
  size_t cs_addr_of = 0, cs_plain = 0;
  size_t suf_empty = 0, suf_wildcard = 0, suf_zero = 0, suf_other = 0;
  size_t composed = 0, rejected = 0;

  void dump(llvm::raw_ostream &os) const;
};
addr_of_instr_t addr_instr;
static size_t addr_of_samples = 0;
void addr_of_instr_t::dump(llvm::raw_ostream &os) const {
  os << "[ADDROF] callsites: addr of=" << cs_addr_of << " plain=" << cs_plain
     << "\n"
     << "[ADDROF] callee suffix leading element: empty=" << suf_empty
     << " wildcard(-1)=" << suf_wildcard << " zero(0)=" << suf_zero
     << " other=" << suf_other << "\n"
     << "[ADDROF] addr_of compositions: composed=" << composed
     << " rejected=" << rejected << "\n";
}

void dump_metrics(llvm::raw_ostream &os) {
  if (!config_t::instance()->print_metrics)
    return;

  // parts of the analysis log through std::cout, so flush it first to keep
  // the report at the end of the output and not somewhere in the middle.
  std::cout.flush();
  os << "=== metrics ===\n";
  compose_instr_t::instance().dump(os);
  addr_instr.dump(os);
  os.flush();
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

      handlerDispatcher(mdata, t->getName(), vNode->getICFGNode(), cs, n_param,
                        acNode, C_PARAM, &p);
    }
  }
}

bool handleGep(const VFGNode *vNode, AccessType &acNode, AccessTypeSet &ats,
               ValueMetadata &mdata) {
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

      auto printType = [](llvm::Type *t, std::string_view msg) {
        if (t->isStructTy()) {
          GEP_LOG("{} {}\n", msg, getType(t));
        } else if (t->isArrayTy()) {
          GEP_LOG("{} {}[]", msg, getType(t));
        }
      };

      printType(sType, "Source type:");
      printType(dType, "Result type:");

      // TODO: check that get_llvm_type is retrieved from the signature of the
      // function correctly.
      auto type = acNode.get_di_type();
      auto ditype = peel_di_qualifiers(type);
      // Both operands need guarding: peel_di_qualifiers returns null when the
      // path carries no DWARF type, and Type::getStructName() is a hard
      // cast<StructType> that asserts for e.g. `getelementptr i32, ptr %p,
      // i64 %i` - the shape an array parameter such as int *p produces.
      // isStructTy() alone is not enough: literal (anonymous) structs are
      // struct-typed but getName() asserts on them.
      // Note these arguments are evaluated even when the GEPHandler log tag
      // is disabled, so the guard matters regardless of -log.
      GEP_LOG("ditype: {} source type: {}\n",
              ditype ? ditype->getName().str() : std::string("<no di type>"),
              !sType->isStructTy() ? std::string("<non-struct>")
              : llvm::cast<StructType>(sType)->isLiteral()
                  ? std::string("<literal struct>")
                  : sType->getStructName().str());
      const llvm::DataLayout &dl = gep_inst->getDataLayout();
      const llvm::Type *path_type = acNode.get_llvm_type();
      llvm::DIType *path_di = acNode.get_di_type();
      bool types_matching = false;

      if (path_di) {
        types_matching = compare_types(path_di, sType, dl);
        GEP_LOG("di: {} matches gep source type: {}\n",
                path_di->getName().str(), types_matching);
      } else if (path_type) {
        types_matching = TypeMatcher::compare_types(sType, path_type);
        if (!types_matching && path_type->isPointerTy()) {
          // Must be an opaque pointer...
          types_matching = true;
          di_instr.opaque_wildcard++;
          acNode.set_llvm_type(sType, nullptr);
        }
      }

      if (types_matching) {
        // SVFUtil::isa<PointerType>(dType)) {
        // Handle struct and array accesses
        // has all Constant Indices requries
        if (gep_inst->hasAllConstantIndices() &&
            gep_inst->getNumIndices() > 1) {
          // Note: getNumIndices returns the number of indices after the
          // base pointer. EXAMPLE: ... i32 0, i32 1, i32 2 will return 3
          int pos = 1;
          // this will point to the first index operand in the GEP instruction
          auto gep_type_it = llvm::gep_type_begin(gep_inst);
          for (; pos <= gep_inst->getNumIndices(); pos++, ++gep_type_it) {
            // pos == 1 will return i32 0
            // dereference of pointer
            if (pos == 1) {
              // TODO: What happens when pos == 1 but getOperand(pos) is not
              // zero?
              // -> then we have an array of pointers
              // If == 0 this just says that it gets dereferenced.
              AccessType tmpAcNode = acNode;
              GEP_LOG("Adding a temporary AccessType to the AccessTypeSet.\n");
              GEP_LOG("The AccessTypeSet now has {} entries.\n",
                      ats.size() + 1);
              tmpAcNode.addField(-1);
              tmpAcNode.set_kind(AccessType::kind_e::read);
              ats.insert(tmpAcNode, vNode->getICFGNode());
            } else {
              // This will be useful for nested structs and recursive structs.
              // We can just iterate over each type in the struct that gets
              // accessed by the GEP.
              // Example: struct A {int f1; }; struct B { A* f1; }; B b;
              // b->f1->f1. GetElementPtr %struct.B, ptr %base, i64 0, i32 0,
              // i32 0 ---> this will return {%struct.B, %struct.A},
              // {%struct.A, i32} Or for arrays in structs struct Inner { int
              // f1; }; struct Outer { struct Inner f1[5]; }; so if you want
              // to access Outer o; o->f1[2]->f1; llvm would create
              // GetElementPtr %struct.Outer, ptr %base, i64 0, i32 0, i32 2,
              // i32 0
              llvm::Type *container_ty = gep_type_it.getStructTypeOrNull();
              llvm::Type *step_result_type = gep_type_it.getIndexedType();

              uint64_t idx = dyn_cast<ConstantInt>(gep_inst->getOperand(pos))
                                 ->getZExtValue();

              const llvm::Type *visit_ty =
                  container_ty ? container_ty : step_result_type;
              const int visit_idx = container_ty ? static_cast<int>(idx) : -1;

              if (visit_ty && acNode.visit_count(visit_ty, visit_idx) >=
                                  MAX_GEP_RECURSION_DEPTH)
                continue;

              // Use DWARF to get the next type that we need to track in the
              // path.
              llvm::DIType *prev_di = acNode.get_di_type();
              llvm::DIType *next_di = next_di_field(prev_di, container_ty, idx,
                                                    step_result_type, dl);
              // --- TEMPORARY INSTRUMENTATION, see di_chain_instr_t ---
              if (next_di) {
                di_instr.ok++;
              } else if (!prev_di) {
                di_instr.poisoned++;
              } else {
                di_instr.fresh_break++;
                if (di_break_samples < 30) {
                  di_break_samples++;
                  llvm::DIType *decayed = decay_di_type(prev_di);
                  auto *st =
                      dyn_cast_if_present<llvm::StructType>(container_ty);
                  llvm::outs()
                      << "[DIBREAK] on='"
                      << (decayed ? decayed->getName() : "<decay-null>")
                      << "' di_tag=" << (decayed ? decayed->getTag() : 0)
                      << " idx=" << idx << " llvm_container="
                      << (st ? (st->isLiteral() ? "<literal>" : st->getName())
                             : "<null/array>")
                      << " n_elems=" << (st ? st->getNumElements() : 0) << "\n";
                  llvm::outs().flush();
                }
              }
              GEP_LOG("Adding field to AccessType {}\n", idx);
              acNode.addField(idx);
              // note that next_di can be nullptr
              // keep the old di type if we could not deduce the new one
              acNode.set_llvm_type(step_result_type, next_di);
              if (visit_ty)
                acNode.add_visited_type(visit_ty, visit_idx);
            }
          }
        } else if (acNode.get_num_fields() == 0) {
          auto d = gep_inst->getOperand(1);
          bool is_array =
              !SVFUtil::isa<ConstantInt>(d) || gep_inst->getNumIndices() == 1;
          if (is_array) {
            GEP_LOG("Setting is_array to true for {}\n", vNode->toString());
            mdata.setIsArray(true);
          }
        } else {
          // if the gep is somekind of other pointer arithmetic we skip.
          return true;
        }
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

local_result_t compute_local_effect(const VFGNode *vNode, AccessType acNode,
                                    ValueMetadata &mdata, Path &p) {
  auto *llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
  AccessTypeSet &ats = mdata.get_access_type_set();
  bool skip_node = false;
  // Processing of the node
  switch (vNode->getNodeKind()) {
  case VFGNode::VFGNodeK::Load: {
    acNode.set_kind(AccessType::kind_e::read);
    ats.insert(acNode, vNode->getICFGNode());
  } break;
  case VFGNode::VFGNodeK::Store: {
    auto *prevValue = p.getPrevValue();

    auto llvm_val = llvm_module_set->getLLVMValue(vNode->getValue());

    if (prevValue != nullptr && SVFUtil::isa<StoreInst>(llvm_val)) {
      auto inst = SVFUtil::cast<StoreInst>(llvm_val);

      // the whole reason of prevValue is to distinguish between if a
      // parameter is used in a store to write to it or to read from it.
      if (inst->getPointerOperand() == prevValue)
        acNode.set_kind(AccessType::kind_e::write);
      else if (inst->getValueOperand() == prevValue)
        acNode.set_kind(AccessType::kind_e::read);

      ats.insert(acNode, vNode->getICFGNode());

      if (vNode->hasIncomingEdge()) {
        for (auto it : vNode->getInEdges()) {
          if (auto node = SVFUtil::dyn_cast<AddrVFGNode>(it->getSrcNode())) {
            if (auto call = SVFUtil::dyn_cast<llvm::CallInst>(
                    llvm_module_set->getLLVMValue(node->getValue()))) {
              llvm::Function *callee = call->getCalledFunction();
              if (callee && callee->getName() == "malloc") {
                // no need to set field, empty field set is what I need
                acNode.set_kind(AccessType::kind_e::create);
                ats.insert(acNode, vNode->getICFGNode());
                LOCAL_LOG("Function {} has a malloc return value",
                          vNode->getICFGNode()->getFun()->getName());
              }
            }
          }
        }
      }
    }
  } break;
  case SVF::VFGNode::VFGNodeK::Gep:
    (acNode.get_di_type() ? di_instr.gep_bu_ok : di_instr.gep_bu_null)++;
    skip_node = handleGep(vNode, acNode, ats, mdata);
    break;
  case VFGNode::VFGNodeK::Copy: {
    if (auto stmt_vfg_node =
            SVFUtil::dyn_cast<StmtVFGNode>(vNode->getValue())) {
      // this is for ptrtoint instructions.
      LOCAL_LOG("{}\n", vNode->toString());
      auto inst = llvm_module_set->getLLVMValue(stmt_vfg_node);
      // auto inst = SVFUtil::dyn_cast<GetElementPtrInst>(lllvm_inst);

      // auto inst = SVFUtil::dyn_cast<Instruction>(vNode->getValue());

      acNode.set_kind(AccessType::kind_e::read);
      ats.insert(acNode, vNode->getICFGNode());

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

  const llvm::Value *prev_value =
      vNode->getValue() == nullptr
          ? nullptr
          : llvm_module_set->getLLVMValue(vNode->getValue());

  return local_result_t{std::move(acNode), prev_value, skip_node};
}
struct exit_state_t {
  SVF::NodeID formal_ret;
  AccessType at;

  // sort the exit states by NodeID.
  // When the NodeID should be equal then sort by AccessType.
  bool operator<(const exit_state_t &o) const {
    return formal_ret != o.formal_ret ? formal_ret < o.formal_ret : at < o.at;
  }
};

inline bool operator==(const exit_state_t &e1, const exit_state_t &e2) {
  return e1.formal_ret == e2.formal_ret && e1.at == e2.at;
}
inline bool operator!=(const exit_state_t &e1, const exit_state_t &e2) {
  return !(e1 == e2);
}

struct func_summary_t {
  /**
   * The effects the summary function has on the local function.
   */
  ValueMetadata effects;
  /**
   * The possible exits of the function.
   */
  std::set<exit_state_t> exits;
};

/**
 * The implementation will decide if new state was reached in the SCC fixpoint
 * iteration.
 */
bool operator==(const liberator::func_summary_t &s1,
                const liberator::func_summary_t &s2) {
  return s1.effects == s2.effects && s1.exits == s2.exits;
}

inline bool operator!=(const func_summary_t &s1, const func_summary_t &s2) {
  return !(s1 == s2);
}

size_t rss_mib() {
  std::ifstream f("/proc/self/statm");
  std::size_t total = 0, resident = 0;
  if (!f)
    return 0;
  f >> total >> resident;
  return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE)) / (1024 * 1024);
}

// TODO: get other possible representations of keys.
std::string make_key(const Function *F, NodeID param_id) {
  std::string key = F->getName().str() + "#" + std::to_string(param_id);
  return key;
}

class param_access_tracker_t {
  // program assignment graph
  SVFIR *pag;
  // points to analysis using andersen
  PointerAnalysis *pta;
  // SVFG graph
  SVFG *svfg;
  // call graph is data structure where each function is a node and
  // a function call is a directed edge between caller and callee.
  CallGraph *cg;
  // detecting scc using tarjan algorithm
  // TODO: use PTACallGraph
  SCCDetection<CallGraph *> *cg_scc;

  // formal_id -> summary
  std::unordered_map<SVF::NodeID, func_summary_t> summaries;
  std::unordered_set<SVF::NodeID> scc_visited;

  typedef stack<NodeID> worklist_t;
  // the inverted ssc's for bottom up analysis
  worklist_t inverted_scc;

  // progress accounting for the bottom-up walk. Number of callgraph nodes,
  // i.e. functions, that already went through process_scc, and the totals we
  // measure that against.
  unsigned num_analyzed_functions = 0;
  unsigned num_analyzed_sccs = 0;
  unsigned total_cg_functions = 0;
  unsigned total_sccs = 0;

public:
  param_access_tracker_t(SVFIR *p, PointerAnalysis *a, SVFG *s)
      : pag(p), pta(a), svfg(s), cg(a->getCallGraph()) {
    // first add our custom indirect calls to the callgraph.
    /*for (const auto &kv : ValueMetadata::myCallEdgeMap_inst) {
      for (const auto &f : kv.second) {
        cg->addIndirectCallGraphEdge(kv.first, kv.first->getCaller(), f);
      }
    }*/

    // evaluate SCC using Tarjan.
    cg_scc = new SCCDetection<CallGraph *>(cg);
    // execute scc
    cg_scc->find();

    // Reverse the SCC order for real bottom up traversal.
    // Important: Do not use topoNodeStack after this again, because it will
    // be empty. Maybe better to copy it here.
    worklist_t st = cg_scc->topoNodeStack();
    while (!st.empty()) {
      auto node_id = st.top();
      st.pop();
      inverted_scc.push(node_id);
    }

    // Every callgraph node belongs to exactly one SCC, so the node count is
    // the number of functions the bottom-up walk will eventually visit.
    total_cg_functions = cg->getTotalNodeNum();
    total_sccs = inverted_scc.size();
    llvm::outs() << "[INFO] Callgraph: " << total_cg_functions
                 << " functions in " << total_sccs << " SCCs\n";
    llvm::outs().flush();

    // Structural, so it is reported here and not by dump_metrics(): it
    // describes the input of the bottom-up walk, and is worth having even
    // when the walk itself never finishes.
    if (config_t::instance()->print_metrics)
      dump_scc_stats();

    /*while (!inverted.empty()) {
      auto el = inverted.top();
      inverted.pop();
      CallGraphNode *cg_node = cg->getCallGraphNode(el);
      cout << cg_node->getFunction()->getName() << " in scc with:" << endl;
      const NodeBS &sub_nodes = cg_scc->subNodes(el);
      for (NodeBS::iterator it = sub_nodes.begin(), eit = sub_nodes.end();
           it != eit; ++it) {
        auto tmp_cg_node = cg->getCallGraphNode(*it);
        cout << "\t" << tmp_cg_node->getFunction()->getName() << endl;
      }
    }*/

    /*auto st1 = cg_scc->revTopoNodeStack();
    while (!st1.empty()) {
      NodeID call_graph_id = st1.front();
      st1.pop();
      CallGraphNode *cg_node = cg->getCallGraphNode(call_graph_id);
      cout << cg_node->getFunction()->getName() << " in scc with:" << endl;
      const NodeBS &sub_nodes = cg_scc->subNodes(call_graph_id);
      for (NodeBS::iterator it = sub_nodes.begin(), eit = sub_nodes.end();
           it != eit; ++it) {
        auto tmp_cg_node = cg->getCallGraphNode(*it);
        cout << "\t" << tmp_cg_node->getFunction()->getName() << endl;
      }
    }*/
  }

  /**
   * Can an indirect call at cs plausibly land in callee?
   *
   * Andersen points-to analysis resolved to many more functions that actually
   * don't make sense because they have a differing signature from the
   * callsite. Therefore this function checks if the callsite and function
   * signature match.
   * @return false if the callsite actual params match the formal params of
   * the callee.
   */
  static bool compatible_signature(const CallICFGNode *cs,
                                   const FunObjVar *callee) {
    if (!cs || !callee)
      return true;
    if (callee->isVarArg())
      return true;

    const auto &parms = cs->getActualParms();
    if (parms.size() != callee->arg_size())
      return false;

    for (size_t i = 0; i < parms.size(); ++i) {
      const SVFType *actual = parms[i] ? parms[i]->getType() : nullptr;
      const auto *formal_var = callee->getArg(i);
      const SVFType *formal = formal_var ? formal_var->getType() : nullptr;
      if (!actual || !formal || actual == formal)
        continue;
      if (actual->isPointerTy() && formal->isPointerTy())
        continue;
      return false;
    }
    return true;
  }

  /**
   * @param the actual param PAG Node
   * @return DIType of the actual parameter
   */
  static llvm::DIType *actual_param_di_type(const SVF::ValVar *param) {
    if (!param)
      return nullptr;

    const Value *v = LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(param);
    if (llvm::isa<Argument>(v)) {
      return liberator::restore_param_di_type(v);
    }

    const llvm::Value *base = v;
    if (const auto *ld = llvm::dyn_cast<llvm::LoadInst>(v))
      base = llvm::getUnderlyingObject(ld->getPointerOperand());

    const auto *alloc = dyn_cast<AllocaInst>(base);

    if (!alloc)
      return nullptr;

    for (auto *rec : llvm::findDVRDeclares(const_cast<AllocaInst *>(alloc))) {
      if (const llvm::DILocalVariable *lv = rec->getVariable())
        return lv->getType();
    }

    return nullptr;
  }

  /**
   * Tarjan over the callgraph restricted to the edges keep() accepts, used to
   * ask what the component structure would look like without a class of
   * edges. Iterative, because the callgraphs that produce a 256-function
   * component are exactly the ones that would blow a recursive
   * implementation's stack.
   *
   * @return size of the largest component, and its members in @p members.
   */
  template <typename KeepFn>
  size_t largest_component(KeepFn keep, std::vector<SVF::NodeID> &members,
                           size_t &cyclic_components) {
    std::vector<SVF::NodeID> ids;
    std::unordered_map<SVF::NodeID, size_t> idx;
    // (NodeID, CallGraphNode*)
    for (const auto &kv : *cg) {
      idx[kv.first] = ids.size();
      ids.push_back(kv.first);
    }

    std::vector<std::vector<size_t>> adj(ids.size());
    // (NodeID, CallGraphNode*)
    for (const auto &kv : *cg) {
      const size_t u = idx[kv.first];
      for (auto it = kv.second->OutEdgeBegin(), eit = kv.second->OutEdgeEnd();
           it != eit; ++it) {
        CallGraphEdge *edge = *it;
        if (!keep(edge))
          continue;
        auto dst = idx.find(edge->getDstID());
        if (dst != idx.end())
          adj[u].push_back(dst->second);
      }
    }

    const size_t n = ids.size();
    std::vector<int> index(n, -1), low(n, 0);
    std::vector<char> on_stack(n, 0);
    std::vector<size_t> stk;
    // (node, index of the next successor to visit)
    std::vector<std::pair<size_t, size_t>> dfs;
    int next_index = 0;
    size_t largest = 0;
    cyclic_components = 0;

    for (size_t root = 0; root < n; ++root) {
      if (index[root] != -1)
        continue;
      index[root] = low[root] = next_index++;
      stk.push_back(root);
      on_stack[root] = 1;
      dfs.emplace_back(root, 0);

      while (!dfs.empty()) {
        const size_t v = dfs.back().first;
        if (dfs.back().second < adj[v].size()) {
          const size_t w = adj[v][dfs.back().second++];
          if (index[w] == -1) {
            index[w] = low[w] = next_index++;
            stk.push_back(w);
            on_stack[w] = 1;
            dfs.emplace_back(w, 0);
          } else if (on_stack[w]) {
            low[v] = std::min(low[v], index[w]);
          }
          continue;
        }

        if (low[v] == index[v]) {
          std::vector<size_t> component;
          size_t w;
          do {
            w = stk.back();
            stk.pop_back();
            on_stack[w] = 0;
            component.push_back(w);
          } while (w != v);

          if (component.size() > 1)
            cyclic_components++;
          if (component.size() > largest) {
            largest = component.size();
            members.clear();
            for (size_t m : component)
              members.push_back(ids[m]);
          }
        }

        dfs.pop_back();
        if (!dfs.empty())
          low[dfs.back().first] = std::min(low[dfs.back().first], low[v]);
      }
    }

    return largest;
  }

  /**
   * Counts the formals of a set of callgraph nodes, i.e. how many summaries a
   * component would have to fixpoint.
   */
  size_t formals_of(const std::vector<SVF::NodeID> &nodes) {
    size_t n = 0;
    for (SVF::NodeID id : nodes)
      n += formal_ids(cg->getCallGraphNode(id)->getFunction()).size();
    return n;
  }

  enum class walk_e { ok, unknown, out_of_range };

  static walk_e di_step(llvm::DIType *cur, int field, llvm::DIType *&out) {
    if (field < 0) {
      out = cur;
      return walk_e::ok;
    }

    auto *comp = llvm::dyn_cast_or_null<DICompositeType>(decay_di_type(cur));

    if (!comp)
      return walk_e::unknown;

    // conjunction: a tag can never equal two of them at once, so `||` would
    // always be true and reject every record.
    auto tag = comp->getTag();
    if (tag != llvm::dwarf::DW_TAG_class_type &&
        tag != llvm::dwarf::DW_TAG_structure_type &&
        tag != llvm::dwarf::DW_TAG_union_type)
      return walk_e::unknown;

    int n_members = 0;
    llvm::DIType *hit = nullptr;
    for (auto el : comp->getElements()) {
      auto mem = dyn_cast<DIDerivedType>(el);
      if (!mem || mem->getTag() != llvm::dwarf::DW_TAG_member) {
        continue;
      }
      if (mem->getName().empty())
        return walk_e::unknown;
      if (n_members == field)
        hit = mem->getBaseType();
      ++n_members;
    }

    if (n_members == 0)
      return walk_e::unknown;
    if (!hit)
      return walk_e::out_of_range;

    out = hit;
    return out ? walk_e::ok : walk_e::unknown;
  }

  unordered_set<SVF::NodeID> validated;
  size_t bad_at_reports = 0;
  static constexpr size_t MAX_BAD_AT_REPORTS = 200;
  static constexpr unsigned MAX_REPOTS_PER_FORMAL = 5;

  std::string fmt_path(const vector<int> fields) {
    if (fields.empty())
      return ".";
    std::string s;
    for (auto f : fields) {
      s += ".";
      s += (f == -1) ? std::string("*") : std::to_string(f);
    }
    return s;
  }

  void validate_summary(SVF::NodeID formal_id, const func_summary_t &summ) {
    if (!config_t::instance()->print_metrics) {
      return;
    }
    if (bad_at_reports > MAX_BAD_AT_REPORTS)
      return;
    if (!validated.insert(formal_id).second)
      return;

    llvm::DIType *base_di = formal_di_type(formal_id);

    if (!base_di) {
      return;
    }
    const auto *fp = dyn_cast<FormalParmVFGNode>(svfg->getGNode(formal_id));
    std::string fname =
        (fp && fp->getFun()) ? fp->getFun()->getName() : "<unknown>";

    unsigned reported = 0;
    for (const AccessType &at : summ.effects.get_access_type_set()) {
      if (at.get_num_fields() == 0)
        continue;

      llvm::DIType *cur = base_di;
      walk_e st = walk_e::ok;
      for (int f : at.get_fields()) {
        llvm::DIType *next = nullptr;
        st = di_step(cur, f, next);
        if (st != walk_e::ok)
          break;
        cur = next;
      }

      if (st == walk_e::unknown) {
        continue;
      }

      auto want = di_record_id(cur);
      auto got = di_record_id(at.get_di_type());

      const char *why = nullptr;
      if (st == walk_e::out_of_range)
        why = "field index does not exists on this record";
      else if ((!want.first.empty() && !got.first.empty()) &&
               (want.first != got.first || want.second != got.second))
        why = "endpoint type disagrees with DWARF walk";

      if (!why)
        continue;

      llvm::outs() << "[BADAT] " << fname << " formal=" << formal_id
                   << " path=" << fmt_path(at.get_fields())
                   << " access=" << to_string(at.get_kind()) << " want="
                   << (want.first.empty() ? llvm::StringRef("<unnamed>")
                                          : want.first)
                   << "/" << want.second << " got="
                   << (got.first.empty() ? llvm::StringRef("<unnamed>")
                                         : got.first)
                   << "/" << got.second << " (" << why << ")\n";

      if (++bad_at_reports >= MAX_BAD_AT_REPORTS) {
        llvm::outs() << "[BADAT] too many bad ats\n";
        break;
      }
      if (++reported >= MAX_REPOTS_PER_FORMAL)
        break;
    }
    llvm::outs().flush();
  }

  /**
   * Reports the biggest SCCs and, more importantly, what makes them cyclic.
   *
   * An SCC held together by DIRECT call edges is genuine recursion and has to
   * be fixpointed. One that is cyclic only through INDIRECT edges is a
   * points-to precision artifact: every address-taken function gets an edge
   * from every callsite whose pointer may reach it, which fuses unrelated
   * layers of a library into a single component and turns the fixpoint into a
   * cross product over all of them. The two need opposite fixes, so count
   * them apart before touching the summary domain.
   */
  /**
   * Which indirect callsites are load-bearing for the giant component?
   *
   * A signature filter only removes edges that are type-impossible; it cannot
   * separate callbacks that genuinely share a shape (destructors taking a
   * void*, comparators, hash functions). If a handful of megamorphic
   * callsites is what fuses the library, suppressing those is worth far more
   * than any further type reasoning - so rank the callsites by how many
   * targets they resolve to, then measure what the component collapses to
   * when each one is suppressed.
   */
  void dump_indirect_hubs(size_t top_n = 10) {
    std::unordered_map<const CallICFGNode *, std::set<const FunObjVar *>>
        targets;
    for (const auto &kv : *cg) {
      for (auto it = kv.second->OutEdgeBegin(), eit = kv.second->OutEdgeEnd();
           it != eit; ++it) {
        CallGraphEdge *edge = *it;
        const FunObjVar *callee = edge->getDstNode()->getFunction();
        for (const CallICFGNode *cs : edge->getIndirectCalls())
          targets[cs].insert(callee);
      }
    }

    // Sort the callsites descending by the number of indirect calls.
    std::vector<const CallICFGNode *> ranked;
    ranked.reserve(targets.size());
    for (const auto &kv : targets)
      ranked.push_back(kv.first);
    std::sort(ranked.begin(), ranked.end(),
              [&](const CallICFGNode *a, const CallICFGNode *b) {
                return targets[a].size() > targets[b].size();
              });

    llvm::outs() << "[HUB] " << targets.size()
                 << " indirect callsites; most megamorphic first, with the "
                    "largest component that remains when it is suppressed:\n";

    std::set<const CallICFGNode *> suppressed;
    std::vector<SVF::NodeID> members;
    for (size_t i = 0; i < ranked.size() && i < top_n; ++i) {
      const CallICFGNode *cs = ranked[i];
      size_t cyclic = 0;
      members.clear();
      const size_t largest = largest_component(
          [cs](CallGraphEdge *e) {
            // keep direct edges
            if (!e->getDirectCalls().empty())
              return true;
            // if edges is indirect
            // keep the edges only if it is not the same as
            for (const CallICFGNode *c : e->getIndirectCalls())
              if (c != cs)
                return true;
            return false;
          },
          members, cyclic);

      llvm::outs() << "[HUB]   " << cs->getCaller()->getName()
                   << " (cs=" << cs->getId()
                   << ") targets=" << targets[cs].size()
                   << " -> largest=" << largest << " functions ("
                   << formals_of(members) << " formals)\n";
      suppressed.insert(cs);
    }

    // And what remains when all of the above go at once.
    size_t cyclic = 0;
    members.clear();
    const size_t largest = largest_component(
        [&suppressed](CallGraphEdge *e) {
          if (!e->getDirectCalls().empty())
            return true;
          for (const CallICFGNode *c : e->getIndirectCalls())
            if (!suppressed.count(c))
              return true;
          return false;
        },
        members, cyclic);
    llvm::outs() << "[HUB] suppressing all " << suppressed.size()
                 << " -> largest=" << largest << " functions ("
                 << formals_of(members) << " formals), " << cyclic
                 << " cyclic components\n";
    llvm::outs().flush();
  }

  void dump_scc_stats(size_t top_n = 10) {
    struct scc_info_t {
      SVF::NodeID rep = 0;
      size_t functions = 0;
      size_t formals = 0;
      size_t intra_direct = 0;   // intra-SCC edges carrying a direct call
      size_t intra_indirect = 0; // intra-SCC edges carrying an indirect call
    };

    std::unordered_map<SVF::NodeID, scc_info_t> per_scc;
    for (const auto &kv : *cg) {
      CallGraphNode *node = kv.second;
      const SVF::NodeID rep = cg_scc->repNode(node->getId());
      scc_info_t &info = per_scc[rep];
      info.rep = rep;
      info.functions++;
      info.formals += formal_ids(node->getFunction()).size();

      for (auto it = node->OutEdgeBegin(), eit = node->OutEdgeEnd(); it != eit;
           ++it) {
        CallGraphEdge *edge = *it;
        // only edges that stay inside the component can close a cycle
        if (cg_scc->repNode(edge->getDstID()) != rep)
          continue;
        if (!edge->getDirectCalls().empty())
          info.intra_direct++;
        if (!edge->getIndirectCalls().empty())
          info.intra_indirect++;
      }
    }

    std::vector<scc_info_t> ranked;
    ranked.reserve(per_scc.size());
    for (const auto &kv : per_scc)
      ranked.push_back(kv.second);
    std::sort(ranked.begin(), ranked.end(),
              [](const scc_info_t &a, const scc_info_t &b) {
                return a.formals > b.formals;
              });

    llvm::outs() << "[SCC] largest components (rep, functions, formals, "
                    "intra-SCC edges direct/indirect):\n";
    for (size_t i = 0; i < ranked.size() && i < top_n; ++i) {
      const scc_info_t &s = ranked[i];
      llvm::outs() << "[SCC]   scc=" << s.rep << " functions=" << s.functions
                   << " formals=" << s.formals << " direct=" << s.intra_direct
                   << " indirect=" << s.intra_indirect << "\n";
    }

    // Name the indirect edges of the worst component: those are the callsites
    // to make more precise if the component turns out not to be real
    // recursion.
    if (!ranked.empty() && ranked.front().intra_indirect > 0) {
      const SVF::NodeID rep = ranked.front().rep;
      size_t shown = 0;
      llvm::outs() << "[SCC] indirect edges inside scc=" << rep << ":\n";
      for (SVF::NodeID member : cg_scc->subNodes(rep)) {
        CallGraphNode *node = cg->getCallGraphNode(member);
        for (auto it = node->OutEdgeBegin(), eit = node->OutEdgeEnd();
             it != eit && shown < top_n; ++it) {
          CallGraphEdge *edge = *it;
          if (cg_scc->repNode(edge->getDstID()) != rep ||
              edge->getIndirectCalls().empty())
            continue;
          llvm::outs() << "[SCC]   " << node->getFunction()->getName() << " -> "
                       << cg->getCallGraphNode(edge->getDstID())
                              ->getFunction()
                              ->getName()
                       << " (" << edge->getIndirectCalls().size()
                       << " callsites)\n";
          shown++;
        }
      }
    }

    // Two counterfactuals. The first asks whether the component is real
    // recursion at all; the second asks how much of it a signature filter on
    // indirect callees would dissolve, before we commit to implementing one.
    std::vector<SVF::NodeID> members;
    size_t cyclic = 0;
    size_t direct_only = largest_component(
        [](CallGraphEdge *e) { return !e->getDirectCalls().empty(); }, members,
        cyclic);
    llvm::outs() << "[SCC] direct edges only: largest=" << direct_only
                 << " functions (" << formals_of(members) << " formals), "
                 << cyclic << " cyclic components\n";

    members.clear();
    cyclic = 0;
    size_t filtered = largest_component(
        [](CallGraphEdge *e) {
          if (!e->getDirectCalls().empty())
            return true;
          const FunObjVar *callee = e->getDstNode()->getFunction();
          for (const CallICFGNode *cs : e->getIndirectCalls())
            if (compatible_signature(cs, callee))
              return true;
          return false;
        },
        members, cyclic);
    llvm::outs() << "[SCC] signature-filtered: largest=" << filtered
                 << " functions (" << formals_of(members) << " formals), "
                 << cyclic << " cyclic components\n";
    llvm::outs().flush();

    dump_indirect_hubs(top_n);
  }

  func_summary_t &get_summary(SVF::NodeID param_id) {
    SVF::NodeID fid = param_id;
    auto *pNode = pag->getGNode(param_id);
    if (pNode && SVFUtil::isa<SVF::ValVar>(pNode)) {
      auto *valVar = SVFUtil::cast<SVF::ValVar>(pNode);
      if (svfg->hasDefSVFGNode(valVar)) {
        fid = svfg->getDefSVFGNode(valVar)->getId();
      }
    }
    return summaries[fid];
  }

  /**
   * @param cs the callsite for which to find the targets functions.
   * @return all possible target function belonging to a call. For indirect
   * calls there can be multiple target functions.
   */
  std::vector<const FunObjVar *> callee_targets(const CallICFGNode *cs) {
    std::vector<const FunObjVar *> res;

    if (!cs->isIndirectCall()) {
      if (const FunObjVar *t = cs->getCalledFunction())
        res.push_back(t);
      return res;
    }

    if (!config_t::instance()->consider_indirect_calls)
      return res;

    std::set<const FunObjVar *> targets;
    if (cg->hasIndCSCallees(cs)) {
      const auto &resolved = cg->getIndCSCallees(cs);
      targets.insert(resolved.begin(), resolved.end());
    }

    auto it = ValueMetadata::myCallEdgeMap_inst.find(cs);
    if (it != ValueMetadata::myCallEdgeMap_inst.end())
      targets.insert(it->second.begin(), it->second.end());

    res.assign(targets.begin(), targets.end());
    return res;
  }

  // Counters are process-wide (see compose_instr_t::instance()) and reported
  // at the end of the run by dump_metrics().
  compose_instr_t &cinstr = compose_instr_t::instance();

  static bool is_indirection_tag(llvm::dwarf::Tag tag) {
    switch (tag) {
    case dwarf::DW_TAG_pointer_type:
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type:
    case dwarf::DW_TAG_ptr_to_member_type:
      return true;
    }
    return false;
  }

  static pair<llvm::StringRef, unsigned> di_record_id(llvm::DIType *t) {
    unsigned depth = 0;
    llvm::DIType *type = peel_di_qualifiers(t);
    while (auto *tmp = llvm::dyn_cast_or_null<llvm::DIDerivedType>(type)) {
      if (!is_indirection_tag(tmp->getTag()))
        break;
      ++depth;
      type = peel_di_qualifiers(tmp->getBaseType());
    }
    if (const auto *comp = llvm::dyn_cast_or_null<llvm::DICompositeType>(type))
      return {comp->getName(), depth};
    return {{}, depth};
  }

  /**
   * type matcher: First tries to match di types of callee and tracked type.
   * If thats not possible fall back to llvm types instead.
   * @param prefix_extra_depth the depth of the additional parameter
   */
  static bool have_compatible_types(const llvm::Type *prefix_ty,
                                    llvm::DIType *prefix_di,
                                    const llvm::Type *callee_base,
                                    llvm::DIType *callee_base_di,
                                    compose_instr_t &ci,
                                    unsigned prefix_extra_depth = 0) {
    auto pre_di = di_record_id(prefix_di);
    auto callee_di = di_record_id(callee_base_di);

    // without this line a call to foo(&t->s)
    // where foo(int *i) and t is of type struct S {int s;}
    // would not match, because t->s has depth 0 and callee expects depth 1.
    pre_di.second += prefix_extra_depth;

    auto pn = pre_di.first;
    auto bn = callee_di.first;

    if (pn.empty() || bn.empty()) {
      // prefix is empty -> use callee_base_di
      llvm::DIType *bad = pn.empty() ? prefix_di : callee_base_di;
      llvm::DIType *d = decay_di_type(bad);
      if (!d)
        ci.di_absent++;
      else if (!llvm::isa<llvm::DICompositeType>(d))
        ci.di_not_composite++;
      else
        ci.di_unnamed++;
    }
    if (!pn.empty() && !bn.empty()) {
      if (pn == bn && pre_di.second == callee_di.second) {
        ci.di_match++;
        return true;
      }
      ci.di_reject++;
      return false;
    }

    // use LLMV type as a fallback.
    if (prefix_ty && callee_base) {
      if (prefix_ty == callee_base) {
        ci.llvm_match++;
        return true;
      }
      if (llvm::isa<llvm::StructType>(prefix_ty) &&
          llvm::isa<llvm::StructType>(callee_base)) {
        ci.llvm_reject++;
        return false;
      }
    }

    ci.undecidable++;
    // TODO: important
    return false;
  }

  /**
   * @param prefix AT from the path
   * @param suffix AT from the callee (already computed summary)
   * @param callee_base LLVM type of callee
   * @param calle_base_di DIType from callee
   * @param addr_of true if parameter is passed with apmersand. foo(&p)
   * @returns the merged AccessType. Returns nullopt if the types mismatch,
   * MAX_GEP_RECURSION_DEPTH or MAX_FIELD_DEPTH is reached.
   */
  std::optional<AccessType>
  merge_access_type(const AccessType &prefix, const AccessType &suffix,
                    const llvm::Type *callee_base, llvm::DIType *callee_base_di,
                    bool addr_of, llvm::DIType *actual_di) {

    // only a non empty suffix path
    cinstr.attempts++;
    auto passed_di = actual_di;
    if (!passed_di)
      passed_di = prefix.get_di_type();

    if (!have_compatible_types(prefix.get_llvm_type(), passed_di, callee_base,
                               callee_base_di, cinstr, addr_of ? 1 : 0))
      return std::nullopt;

    // we stop if we have a self referencing data structure like a linked list
    // after MAX_GEP_RECURSION_DEPTH rounds.
    for (auto &kv : suffix.get_visited_types()) {
      int count =
          prefix.visit_count(kv.first.first, kv.first.second) + kv.second;
      if (count > MAX_GEP_RECURSION_DEPTH)
        return std::nullopt;
    }
    AccessType out = prefix;

    // we stop when the path gets too deep, because then we likely have some
    // recursive definition.
    if (prefix.get_num_fields() + suffix.get_num_fields() > MAX_FIELD_DEPTH)
      return std::nullopt;

    if (suffix.get_num_fields() == 1) {
      out.append_path(suffix.get_fields(), prefix.get_kind(),
                      prefix.get_llvm_type(), prefix.get_di_type());
    }
    if (suffix.get_num_fields() > 1) {
      out.append_path(suffix.get_fields(), suffix.get_parent_kind(),
                      suffix.get_parent_llvm_type(), suffix.get_p_di_type());
    }

    out.set_kind(suffix.get_kind());

    // we only change the type if sfields is not empty.
    // as otherwise we track a wrong field type.
    if (suffix.get_num_fields() > 0) {
      if (!suffix.get_di_type())
        di_instr.compose_null_di++;
      out.set_llvm_type(suffix.get_llvm_type(), suffix.get_di_type());
    }

    for (const auto &kv : suffix.get_visited_types()) {
      out.add_visited_count(kv.first.first, kv.first.second, kv.second);
    }

    return out;
  }

  /**
   * @param ret - the FormalReturn for which we want the ActualReturn
   * Example two calls to xoo:
   * bar -> xoo
   * foo -> xoo
   * two actual returns to bar and foo:
   *      -> bar
   * xoo |
   *      -> foo
   * @return the ActualRet node, from where the callee was called
   */
  const VFGNode *get_resume_node(SVF::NodeID ret, const CallICFGNode *cs) {
    const VFGNode *formal_ret = svfg->getGNode(ret);

    for (auto it = formal_ret->OutEdgeBegin(), eit = formal_ret->OutEdgeEnd();
         it != eit; ++it) {
      const VFGNode *d = (*it)->getDstNode();

      if (auto actual_ret = SVFUtil::dyn_cast<ActualRetVFGNode>(d))
        if (actual_ret->getCallSite() == cs)
          return d;
      if (auto outs_ret = SVFUtil::dyn_cast<ActualOUTSVFGNode>(d))
        if (outs_ret->getCallSite() == cs)
          return d;
    }

    return nullptr;
  }

  /**
   * @param n Node pointing to ActualParmVFGNode
   * @return id of corresponding FormalParmVFGNode or FormalINSVFGNode
   */
  SVF::NodeID callee_entry_id(const VFGNode *n, const FunObjVar *callee) {
    for (auto it = n->OutEdgeBegin(), eit = n->OutEdgeEnd(); it != eit; ++it) {
      const VFGNode *dst = (*it)->getDstNode();
      // passed by value
      if (auto formal_parm = SVFUtil::dyn_cast<FormalParmVFGNode>(dst))
        if (formal_parm->getFun() == callee)
          return formal_parm->getId();
      // passed by reference (memory region read into function)
      if (auto formal_in = SVFUtil::dyn_cast<FormalINSVFGNode>(dst)) {
        if (formal_in->getFun() == callee) {
          return formal_in->getId();
        }
      }
    }

    return 0;
  }

  /**
   * Merge the prefix summary with the suffix summaries.
   * @param in succ - Node pointing to ActualParmVFGNode
   * @param in cs the callsite from which summaries are merged.
   * @param in p the path that was taken so far.
   * @param out res_summ the resulting merged summary
   * @param out worklist appended with the new paths from which execution will
   * resume.
   */
  void merge_summary(VFGNode *succ, const CallICFGNode *cs, Path &p,
                     func_summary_t &summ, vector<Path> &worklist) {
    if (cs->isIndirectCall() && !config_t::instance()->consider_indirect_calls)
      return;

    PAGNode *param = nullptr;
    llvm::DIType *actual_param = nullptr;
    bool addr_of = false;
    if (auto a = SVFUtil::dyn_cast<ActualParmVFGNode>(succ)) {
      param = const_cast<ValVar *>(a->getParam());
      addr_of = is_addr_of(a->getParam());
      actual_param = actual_param_di_type(a->getParam());

      if (addr_of)
        addr_instr.cs_addr_of++;
      else
        addr_instr.cs_plain++;
    }

    if (!param)
      return;

    int n = 0;
    for (auto q : cs->getActualParms()) {
      if (q == param)
        break;
      ++n;
    }

    for (const FunObjVar *callee : callee_targets(cs)) {
      SVF::NodeID callee_formal = formal_id_of(callee, n);
      SUMM_LOG("Possible Called function: {} for id: {}\n", callee->getName(),
               callee_formal);

      if (cs->isIndirectCall())
        ind_collected_functions.insert(callee);

      std::string callee_name = callee->getName();
      if (hasHandlerDispatcher(&summ.effects, callee_name, cs, cs, n,
                               C_PARAM)) {
        SUMM_LOG("Applying external API model for {} on parameter {}\n",
                 callee_name, n);
        handlerDispatcher(summ.effects, callee_name, cs, cs, n,
                          p.get_access_type(), C_PARAM, &p);
      }

      if (callee_formal == 0)
        continue;
      auto sum_it = summaries.find(callee_formal);
      if (sum_it == summaries.end())
        continue;
      const func_summary_t &callee_sum = sum_it->second;
      // The base type the callee's summary field paths are relative to.
      const llvm::Type *callee_base = formal_entry_type(callee_formal);
      llvm::DIType *callee_base_di = formal_di_type(callee_formal);
      // FIXME: this can get very big: 122.000 entries that get copied
      // we don't really need the expanded version of all nodes...
      // so we can compute this lazily using a chain
      for (const AccessType &sum_at :
           callee_sum.effects.get_access_type_set()) {
        if (addr_of) {
          const auto &sf = sum_at.get_fields();
          if (sf.empty())
            addr_instr.suf_empty++;
          else if (sf.front() == -1)
            addr_instr.suf_wildcard++;
          else if (sf.front() == 0)
            addr_instr.suf_zero++;
          else
            addr_instr.suf_other++;
          addr_instr.suf_other++;
          addr_instr.suf_other++;
        }
        // merge path access type copy with callee access type and merge it
        // into summary
        auto composed_opt =
            merge_access_type(p.get_access_type(), sum_at, callee_base,
                              callee_base_di, addr_of, actual_param);
        if (addr_of) {
          if (composed_opt)
            addr_instr.composed++;
          else
            addr_instr.rejected++;

          if (addr_of_samples < 25) {
            addr_of_samples++;
            auto pid = di_record_id(p.get_access_type().get_di_type());
            auto cid = di_record_id(callee_base_di);
            llvm::outs() << "[ADDROF-SAMPLE] " << (composed_opt ? "OK" : "REJ ")
                         << " callee=" << callee->getName() << " prefix="
                         << fmt_path(p.get_access_type().get_fields()) << "("
                         << (pid.first.empty() ? "<unnamed>" : pid.first) << "/"
                         << pid.second << ")"
                         << " callee_base=("
                         << (cid.first.empty() ? "<unnamed>" : cid.first) << "/"
                         << cid.second << ")"
                         << " suffix=" << fmt_path(sum_at.get_fields())
                         << " kind=" << to_string(sum_at.get_kind()) << "\n";
          }
        }
        if (composed_opt) {
          summ.effects.get_access_type_set().insert_nodes(
              *composed_opt, sum_at.getICFGNodes());
        }
      }
      if (callee_sum.effects.isArray())
        summ.effects.setIsArray(true);
      if (callee_sum.effects.isFilePath())
        summ.effects.setIsFilePath(true);
      if (callee_sum.effects.isMallocSize())
        summ.effects.setMallocSize(true);

      for (const exit_state_t &exit : callee_sum.exits) {
        const SVFGNode *poss_ret = get_resume_node(exit.formal_ret, cs);
        if (!poss_ret)
          continue;
        auto composed_opt =
            merge_access_type(p.get_access_type(), exit.at, callee_base,
                              callee_base_di, addr_of, actual_param);
        if (!composed_opt)
          continue;
        Path r(poss_ret, nullptr, exit.at.get_llvm_type());
        r.set_access_type(*composed_opt);
        worklist.push_back(r);
      }
      SUMM_LOG("{}\n", print_summary(callee_sum.effects, true));
    }
  }

  /**
   * @param f function to retrieve for FormalParmVFGNode from.
   * @param n the index of the parameter in the function f.
   * @return the FormalParmVFGNode of parameter n in function f.
   */
  SVF::NodeID formal_id_of(const FunObjVar *f, int n) {
    if (!f || n < 0 || (size_t)n >= f->arg_size())
      return 0;
    auto *arg_val = f->getArg(n);
    if (!arg_val)
      return 0;
    auto *vv = SVFUtil::dyn_cast<ValVar>(arg_val);
    if (!vv || !svfg->hasDefSVFGNode(vv))
      return 0;
    return svfg->getDefSVFGNode(vv)->getId();
  }

  /**
   * TODO: maybe we can leave this all together, when we don't carry it in the
   * MetadataValue param.
   * @param formal_id - the id of the formal
   */
  const llvm::Type *formal_entry_type(SVF::NodeID formal_id) {
    const VFGNode *entry = svfg->getGNode(formal_id);
    if (auto formal_param = SVFUtil::dyn_cast<FormalParmVFGNode>(entry)) {
      auto *llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
      const llvm::Value *val =
          llvm_module_set->getLLVMValue(formal_param->getParam());
      if (!val)
        return nullptr;
      // for base types like i32, i8, float, half, etc...
      if (!val->getType()->isPointerTy())
        return val->getType();
      if (auto *seek_type = restore_llvm_type(val))
        return seek_type;
      return val->getType();
    }
    return nullptr;
  }

  llvm::DIType *formal_di_type(SVF::NodeID formal_id) {
    const VFGNode *entry = svfg->getGNode(formal_id);
    if (auto param = dyn_cast<FormalParmVFGNode>(entry)) {
      auto *llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
      if (auto val = llvm_module_set->getLLVMValue(param->getParam()))
        return restore_param_di_type(val);
    }
    GEP_LOG("Could not find di type for formal_id {}\n", formal_id);
    return nullptr;
  }

  struct visit_key_t {
    const VFGNode *node;
    const llvm::Type *type;
    AccessType::kind_e kind;
    vector<int> fields;
    bool operator<(const visit_key_t &o) const {
      return std::tie(node, fields, kind, type) <
             std::tie(o.node, o.fields, o.kind, o.type);
    }
  };

  /**
   * Evaluates a summary for accesses to a function f that.
   *  @param formal_id - id of the formal parameter node of a function
   * representing the summary.
   *  @return function summary of the read and write accesses to that
   * function.
   */
  bool summarize_formal(SVF::NodeID formal_id, func_summary_t &summ) {
    // We can use this to compare for changes, because effects will grow in
    // size for access type. And exits will grow in size for finding more
    // ActualParmVFGNodes. This is because
    const auto org_effects = summ.effects.get_access_type_set().size();
    const size_t org_exists = summ.exits.size();
    const bool org_is_array = summ.effects.isArray();
    const bool org_is_malloc_size = summ.effects.isMallocSize();
    const bool org_is_file_path = summ.effects.isFilePath();

    ValueMetadata &work = summ.effects;

    const VFGNode *entry = svfg->getGNode(formal_id);
    // TODO: this should not be a set but an unordered_map and the key should
    // just be a plain struct containing the kind, fields, type, current node.
    // And using a good hash.
    std::vector<Path> worklist;
    std::set<visit_key_t> visited;

    auto di_type = formal_di_type(formal_id);
    (di_type ? di_instr.formal_with_di : di_instr.formal_no_di)++;
    if (!di_type)
      GEP_LOG("Could not resolve di_type for function");
    else
      GEP_LOG("Resolved di_type");
    worklist.push_back(
        Path(entry, nullptr, formal_entry_type(formal_id), di_type));

    while (!worklist.empty()) {
      Path p = worklist.back();
      worklist.pop_back();
      // --- TEMPORARY INSTRUMENTATION ---
      instr.pops++;
      instr.max_worklist = std::max(instr.max_worklist, worklist.size());
      if ((instr.pops & 0xFFFFF) == 0) { // every ~1M pops
        llvm::outs()
            << "[PATHS] formal=" << formal_id << " Function: "
            << PAG::getPAG()->getGNode(formal_id)->getFunction()->getName()
            << " pops=" << instr.pops << " dedup=" << instr.dedup_hits
            << " worklist=" << worklist.size()
            << " (max wl=" << instr.max_worklist << ")"
            << " visited=" << visited.size()
            << " effects ATs=" << summ.effects.get_access_type_set().size()
            << " exits=" << summ.exits.size() << " rss=" << rss_mib()
            << "MiB\n";
        llvm::outs().flush();
      }
      // --- end instrumentation ---
      const AccessType &path_at = p.get_access_type();
      if (!visited
               .insert(visit_key_t{p.getNode(), path_at.get_llvm_type(),
                                   path_at.get_kind(), path_at.get_fields()})
               .second) {
        instr.dedup_hits++;
        continue;
      }
      instr.max_visited = std::max(instr.max_visited, visited.size());
      const VFGNode *curr = p.getNode();
      /*if (auto type = p.get_access_type().get_llvm_type()) {
        std::string s;
        raw_string_ostream os(s);
        type->print(os);
        SUMM_LOG("{}", s);
        for (auto f : p.get_access_type().get_fields()) {
          cout << f << ", ";
        }
      }*/

      // Note that p.get_access_type will create a copy of the access type
      // here.
      auto lr = compute_local_effect(curr, p.get_access_type(), work, p);
      // Reasons we skip here:
      // 1. GEP found some weird pointer arithmetic, that we can't assign to
      // arrays or struct fields.
      // 2. GEP converts to a constant expression.
      if (lr.skip)
        continue;

      // update path, field indices, isArray, kind, type, previousValue for
      // stores
      p.set_access_type(lr.ac_node);
      p.setPrevValue(lr.prev_value);

      // Leaf nodes in SVF are for example ActualRetVFGNodes that are not
      // consumed by the caller. A store that is never read. (dead store) A
      // terminal node, which is not used anymore further down.
      if (!curr->hasOutgoingEdge())
        continue;

      for (auto it = curr->OutEdgeBegin(), etit = curr->OutEdgeEnd();
           it != etit; ++it) {
        VFGEdge *edge = *it;
        // Skip indirect edges except for store and Memory phi nodes.
        if (curr->getNodeKind() != VFGNode::VFGNodeK::Store &&
            curr->getNodeKind() != VFGNode::VFGNodeK::MIntraPhi &&
            SVFUtil::isa<SVF::IndirectSVFGEdge>(edge))
          continue;

        VFGNode *succ = edge->getDstNode();

        // (A) CALL boundary
        const CallICFGNode *cs = nullptr;
        // if the parameter is put into an ActualParmVFGNode, we have a
        // callsite. Instead of descending, we merge the function summaries
        // of the callee with the caller.
        if (auto a = SVFUtil::dyn_cast<ActualParmVFGNode>(succ))
          cs = a->getCallSite();
        else if (auto a = SVFUtil::dyn_cast<ActualINSVFGNode>(succ))
          cs = a->getCallSite();

        if (cs) {
          merge_summary(succ, cs, p, summ, worklist);
          continue;
        }

        // (B) return how the path exits
        if (SVFUtil::isa<ActualRetVFGNode>(succ) ||
            SVFUtil::isa<ActualOUTSVFGNode>(succ)) {
          summ.exits.insert(exit_state_t{curr->getId(), p.get_access_type()});
          continue;
        }

        // (C) normal intra edge
        // TODO: do we need to create a copy here or are there better
        // alternatives?
        Path ps = p;
        // add node to histroy
        // TODO: is this even needed or can we just add the next node to the
        // worklist?
        // Because callstack in ps is not needed anymore, because recursive
        // functions are now handled by the SCC algorithm.
        // ps.addStep(curr->getICFGNode());
        //
        ps.setNode(succ);
        worklist.push_back(ps);
      }
    }

    return summ.effects.get_access_type_set().size() != org_effects ||
           summ.exits.size() != org_exists ||
           summ.effects.isArray() != org_is_array ||
           summ.effects.isMallocSize() != org_is_malloc_size ||
           summ.effects.isFilePath() != org_is_file_path;
  }

  /**
   * @return all functions that are in the same SCC.
   */
  std::vector<const FunObjVar *> functions_in_scc(SVF::NodeID rep) {
    std::vector<const FunObjVar *> results;

    for (SVF::NodeID id : cg_scc->subNodes(rep)) {
      CallGraphNode *n = cg->getGNode(id);
      results.push_back(n->getFunction());
    }

    return results;
  }

  /**
   * @return all direct callees of a function.
   */
  std::vector<const FunObjVar *> direct_callees(const FunObjVar *f) {
    std::vector<const FunObjVar *> res;
    if (!f)
      return res;
    auto cg_node = cg->getCallGraphNode(f);
    if (!cg_node)
      return res;
    for (auto edge : cg_node->getOutEdges()) {
      res.push_back(edge->getDstNode()->getFunction());
    }
    return res;
  }

  /**
   * @return all ids of FormalParmVFGNode of Function G that
   * have a definition (no unused params) in the SVFG.
   */
  vector<SVF::NodeID> formal_ids(const FunObjVar *G) {
    std::vector<SVF::NodeID> ids;

    // there can be cases where we have inline assembly that we have to skip
    if (!G || G->isDeclaration() || !pag->hasFunArgsList(G))
      return ids;

    for (const SVFVar *arg : pag->getFunArgsList(G)) {
      if (!arg->getType() /*|| !arg->getType()->isPointerTy() */)
        continue;

      auto *vv = SVFUtil::cast<ValVar>(arg);

      if (!svfg->hasDefSVFGNode(vv))
        continue;

      ids.push_back(svfg->getDefSVFGNode(vv)->getId());
    }

    return ids;
  }

  /**
   * Reports how far the bottom-up callgraph walk has come. Uses outs()
   * directly instead of one of the *_LOG macros, because those expand to a
   * no-op under NDEBUG and the progress has to stay visible in release
   * builds.
   *
   * @param rep representative node of the SCC that is about to be processed.
   */
  void report_progress(NodeID rep) {
    const unsigned scc_size = cg_scc->subNodes(rep).count();
    num_analyzed_functions += scc_size;
    num_analyzed_sccs++;

    llvm::outs() << "[INFO " << num_analyzed_functions << "/"
                 << total_cg_functions << " funcs, " << num_analyzed_sccs << "/"
                 << total_sccs << " SCCs] callgraph: ";

    auto *rep_node = cg->getCallGraphNode(rep);
    if (rep_node && rep_node->getFunction())
      llvm::outs() << rep_node->getFunction()->getName();
    else
      llvm::outs() << "<scc " << rep << ">";

    // recursive functions:
    if (scc_size > 1)
      llvm::outs() << " (SCC of " << scc_size << ")";

    llvm::outs() << "\n";
    llvm::outs().flush();
  }

  /**
   * For multi/cycles SCCs compute fixpoint for all other just compute
   * summmary for all formal params.
   * @param id of the function to be queried.
   */
  // ---------------------------------------------------------------------
  // TEMPORARY INSTRUMENTATION (memory/time diagnosis) - remove when done.
  // Answers two questions:
  //  1. is a slow summarize_formal a path-COUNT problem or a path-SIZE
  //     problem? -> [PATHS] lines, emitted from inside the search loop so
  //     they appear even when a single call never returns.
  //  2. what does the retained state actually consist of? -> [RETAIN] lines.
  // ---------------------------------------------------------------------
  struct instr_t {
    uint64_t pops = 0;       // paths taken off the worklist
    uint64_t dedup_hits = 0; // pops rejected by `visited`
    size_t max_worklist = 0;
    size_t max_visited = 0;
  };
  instr_t instr;

  /// Totals over everything currently retained in `summaries`.
  void dump_retention_stats(const char *tag) const {
    size_t n_at = 0, n_icfg = 0, n_vis = 0, n_fields = 0, n_pfields = 0,
           n_exits = 0, max_set = 0;
    for (const auto &kv : summaries) {
      n_exits += kv.second.exits.size();
      const auto &ats = kv.second.effects.get_access_type_set();
      max_set = std::max(max_set, ats.size());
      for (const AccessType &at : ats) {
        n_at++;
        n_icfg += at.getICFGNodes().size();
        n_vis += at.get_visited_types().size();
        n_fields += at.get_fields().size();
        n_pfields += at.get_parent_fields().size();
      }
    }
    // std::set node header is 32 bytes on libstdc++ (colour + 3 pointers);
    // std::map node for visited_types is 32 + 24 bytes of key/value.
    const size_t b_flat = n_at * sizeof(AccessType);
    const size_t b_setnode = n_at * 32;
    const size_t b_icfg = n_icfg * 40;
    const size_t b_vis = n_vis * 56;
    const size_t b_fields = (n_fields + n_pfields) * 4;
    llvm::outs() << "[RETAIN " << tag << "] summaries=" << summaries.size()
                 << " accesstypes=" << n_at << " (max/summary=" << max_set
                 << ") exits=" << n_exits << " | icfg_nodes=" << n_icfg
                 << " visited_types=" << n_vis << " fields=" << n_fields
                 << " p_fields=" << n_pfields << "\n[RETAIN " << tag
                 << "] approx MB: flat=" << (b_flat >> 20)
                 << " setnodes=" << (b_setnode >> 20)
                 << " icfg=" << (b_icfg >> 20)
                 << " visited_types=" << (b_vis >> 20)
                 << " fieldvecs=" << (b_fields >> 20) << " TOTAL="
                 << ((b_flat + b_setnode + b_icfg + b_vis + b_fields) >> 20)
                 << "\n";
    llvm::outs().flush();
  }

  void process_scc(NodeID id) {
    report_progress(id);
    // periodic retention snapshot; cheap enough at this interval
    if (num_analyzed_sccs % 500 == 0) {
      dump_retention_stats("periodic");
      llvm::outs() << "[DICHAIN periodic] ok=" << di_instr.ok
                   << " fresh_break=" << di_instr.fresh_break
                   << " poisoned=" << di_instr.poisoned
                   << " | formal_with_di=" << di_instr.formal_with_di
                   << " formal_no_di=" << di_instr.formal_no_di
                   << " opaque_wildcard=" << di_instr.opaque_wildcard
                   << " compose_null_di=" << di_instr.compose_null_di
                   << " | gep_bu(ok/null)=" << di_instr.gep_bu_ok << "/"
                   << di_instr.gep_bu_null
                   << " gep_td(ok/null)=" << di_instr.gep_td_ok << "/"
                   << di_instr.gep_td_null << "\n";
      llvm::outs().flush();
    }

    llvm::SmallVector<SVF::NodeID, 8> formals;
    // get all formal ids of all functions in an SCC.
    for (SVF::NodeID tid : cg_scc->subNodes(id)) {
      const FunObjVar *f = cg->getGNode(tid)->getFunction();
      for (SVF::NodeID fid : formal_ids(f)) {
        formals.push_back(fid);
      }
    }

    if (formals.empty())
      return;

    // Single SCC (non-recursive function)
    if (!cg_scc->isInCycle(id)) {
      for (auto fid : formals) {
        summarize_formal(fid, summaries[fid]);
      }
      for (auto fid : formals) {
        validate_summary(fid, summaries[fid]);
      }
      /*
      for (auto fid : formals) {
        auto fp = dyn_cast<FormalParmVFGNode>(svfg->getGNode(fid));
        llvm::outs() << "[process_scc] "
                     << " | Evaluated Function: " << fp->getFun()->getName()
                     << " (Formal Param ID: " << fid << ")\n"
                     << " -> Effects: "
                     << liberator::print_summary(summaries[fid].effects)
                     << " -> Exit Count: " << summaries[fid].exits.size()
                     << "\n\n";
      }*/
      return;
    }

    // Fixpoint iteration
    bool changed = true;

    // print scc
    auto nodes = cg_scc->subNodes(id);
    int num = 1;
    for (auto n : nodes) {
      auto tmp = cg->getCallGraphNode(n);
      llvm::outs() << tmp->getName() << " -> ";

      if (num == 5) {
        llvm::outs() << "\n";
        num = 0;
      }
      num++;
    }

    llvm::outs() << "\n";

    // TEMPORARY INSTRUMENTATION: is a slow SCC diverging, or just expensive?
    //   total_ats grows ~linearly per round, unbounded -> divergent
    //   total_ats plateaus but rounds continue      -> `changed` oscillates
    //   on
    //                                                  exits/flags, other bug
    //   total_ats converges slowly                  -> pure cost problem
    int fixpoint_iter = 0;
    while (changed) {
      if (fixpoint_iter >= MAX_FIXPOINT_ITERATIONS) {
        llvm::outs() << "[WARN] SCC " << id << " (" << formals.size()
                     << " formals) did not reach a fixpoint within "
                     << MAX_FIXPOINT_ITERATIONS
                     << " iterations; widening. Summaries for this SCC are "
                        "under-approximate.\n";
        llvm::outs().flush();
        break;
      }
      fixpoint_iter++;
      changed = false;
      for (auto fid : formals) {
        changed |= summarize_formal(fid, summaries[fid]);
      }

      size_t total_ats = 0, total_exits = 0, max_fields = 0;
      for (auto fid : formals) {
        const auto &ats = summaries[fid].effects.get_access_type_set();
        total_ats += ats.size();
        total_exits += summaries[fid].exits.size();
        for (const AccessType &at : ats)
          max_fields = std::max<size_t>(max_fields, at.get_num_fields());
      }
      llvm::outs() << "[FIX] scc=" << id << " iter=" << fixpoint_iter
                   << " formals=" << formals.size()
                   << " total_ats=" << total_ats << " exits=" << total_exits
                   << " max_field_depth=" << max_fields << "\n";
      llvm::outs() << "[DICHAIN] ok=" << di_instr.ok
                   << " fresh_break=" << di_instr.fresh_break
                   << " poisoned=" << di_instr.poisoned
                   << " | formal_with_di=" << di_instr.formal_with_di
                   << " formal_no_di=" << di_instr.formal_no_di
                   << " opaque_wildcard=" << di_instr.opaque_wildcard
                   << " compose_null_di=" << di_instr.compose_null_di
                   << " | gep_bu(ok/null)=" << di_instr.gep_bu_ok << "/"
                   << di_instr.gep_bu_null
                   << " gep_td(ok/null)=" << di_instr.gep_td_ok << "/"
                   << di_instr.gep_td_null << "\n";
      llvm::outs().flush();
    }

    for (auto fid : formals) {
      validate_summary(fid, summaries[fid]);
    }

    for (auto fid : formals) {
      auto fp = dyn_cast<FormalParmVFGNode>(svfg->getGNode(fid));
      llvm::outs() << "[process_scc] "
                   << " | Evaluated Function: " << fp->getFun()->getName()
                   << " (Formal Param ID: " << fid << ")\n"
                   << " -> Effects: "
                   << liberator::print_summary(summaries[fid].effects)
                   << " -> Exit Count: " << summaries[fid].exits.size()
                   << "\n\n";
    }
  }

  void walk_scc(const FunObjVar *f) {
    if (!f)
      return;

    auto cg_node = cg->getCallGraphNode(f);
    if (!cg_node)
      return;
    SVF::NodeID rep = cg_scc->repNode(cg_node->getId());
    if (!scc_visited.insert(rep).second)
      return;

    // the callgraph is now a DAG. So we will first walk the DAG to the leaf
    // nodes using DFS and then evaluate the summaries of each function
    // bottom-up.
    for (const FunObjVar *G : functions_in_scc(rep)) {
      for (const FunObjVar *C : direct_callees(G)) {
        if (!C)
          continue;
        auto c_node = cg->getCallGraphNode(C);
        if (!c_node)
          continue;
        if (cg_scc->repNode(c_node->getId()) != rep) {
          walk_scc(C);
        }
      }
    }

    // fixpoint each SCC. A SCC node can contain one Function Object for
    // non-recursive and multiple for recursive functions. For recursive
    // function calls we need to evaluate the fixpoint.
    bool changed = true;
    int fixpoint_iter = 0;
    while (changed && fixpoint_iter < MAX_FIXPOINT_ITERATIONS) {
      fixpoint_iter++;
      changed = false;
      for (const FunObjVar *G : functions_in_scc(rep)) {
        for (SVF::NodeID fid : formal_ids(G)) {
          changed |= summarize_formal(fid, summaries[fid]);
        }
      }
    }
    if (changed) {
      llvm::outs() << "[WARN] SCC " << rep
                   << " did not reach a fixpoint within "
                   << MAX_FIXPOINT_ITERATIONS
                   << " iterations; widening. Summaries for this SCC are "
                      "under-approximate.\n";
      llvm::outs().flush();
    }
  }

  void print_bottom_up_order(raw_ostream &os) {
    auto cpy_scc = inverted_scc;
    int count = 1;
    while (!cpy_scc.empty()) {
      if ((count % 5) == 0)
        os << "\n";
      auto el = cpy_scc.top();
      cpy_scc.pop();
      auto rep = cg_scc->repNode(el);
      auto sub_nodes = cg_scc->subNodes(el);
      auto cgn = cg->getCallGraphNode(rep);
      os << count++ << ". " << cgn->getFunction()->getName();
      if (cpy_scc.size() != 1 && sub_nodes.empty()) {
        os << " -> ";
      } else {
        for (auto sub : sub_nodes) {
          if (sub == rep)
            continue;
          os << ", " << cg->getCallGraphNode(sub)->getFunction()->getName();
        }
        os << " -> ";
      }
    }
    os << "\n";
  }

  /**
   * Walks the inverted SCCs retrieved from executing modified "tarjan
   * algorithm".
   *
   * @param f function for
   */
  void walk_inverted_scc(const FunObjVar *f) {
    if (!f)
      return;

    auto cg_node = cg->getCallGraphNode(f);
    if (!cg_node)
      return;
    SVF::NodeID rep = cg_scc->repNode(cg_node->getId());
    // print_bottom_up_order(llvm::outs());

    while (!scc_visited.count(rep) && !inverted_scc.empty()) {
      auto el = inverted_scc.top();
      inverted_scc.pop();
      process_scc(el);
      scc_visited.insert(el);
    }
  }
};

/*
 * TODO: Can Probably be removed:
 static vector<Path> expand_successors(const VFGNode *vNode, Path &p,
                                      ValueMetadata &mdata) {
  std::vector<Path> next;
  if (!vNode->hasOutgoingEdge())
    return next;

  auto llvm_module_set = LLVMModuleSet::getLLVMModuleSet();
  auto ac_node = p.get_access_type();

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
      if (SVFUtil::isa<SVF::IndirectSVFGEdge>(edge))
        continue;
    }

    VFGNode *succ_node = edge->getDstNode();
    // Add the current ICFGNode to the history of the path
    Path p_succ = p;
    p_succ.addStep(vNode->getICFGNode());

    bool ok_continue = true;

    const CallICFGNode *cs = nullptr;
    // is a direct call
    bool isACall = false;

    if (auto call_node = SVFUtil::dyn_cast<ActualParmVFGNode>(succ_node)) {
      cs = call_node->getCallSite();
      HANDLER_LOG("ActualParmVFGNode {}\n", succ_node->toString());
      isACall = true;
    } else if (auto call_node =
                   SVFUtil::dyn_cast<ActualINSVFGNode>(succ_node)) {
      cs = call_node->getCallSite();
      // Call MU
      HANDLER_LOG("ActualINSVFGNode {}\n", succ_node->toString());
      isACall = true;
    } else if (auto ret_node = SVFUtil::dyn_cast<ActualRetVFGNode>(succ_node))
{ cs = ret_node->getCallSite(); isACall = false; } else if (auto ret_node =
                   SVFUtil::dyn_cast<ActualOUTSVFGNode>(succ_node)) {
      // Call CHI
      cs = ret_node->getCallSite();
      isACall = false;
    } else if (auto addr_node = SVFUtil::dyn_cast<AddrVFGNode>(succ_node)) {
      if (auto *inst = llvm::dyn_cast<llvm::Instruction>(
              llvm_module_set->getLLVMValue(addr_node->getValue()))) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
          llvm::Function *callee = call->getCalledFunction();
          if (callee && callee->getName() == "malloc") {
            HANDLER_LOG("Found a malloc in an AddrVFGNode\n");
          }
        }
      }
    }

    if (cs && isACall) {
      p_succ.pushFrame(cs);
      HANDLER_LOG("The callstack now has {} entries.", p_succ.getStackSize());
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
                  SVFUtil::dyn_cast<ActualParmVFGNode>(succ_node)) {
            param = const_cast<SVF::ValVar *>(call_node->getParam());
            can_handle_parameter = true;
          } else if (auto call_node =
                         SVFUtil::dyn_cast<FormalParmVFGNode>(succ_node)) {
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

            ok_continue = handlerDispatcher(&mdata, fun, vNode->getICFGNode(),
                                            cs, n_param, ac_node, C_PARAM,
&p);
          }
        }
      }
    }

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

    if (ok_continue) {
      p_succ.setNode(succ_node);
      next.push_back(p_succ);
    }
  }
  return next;
}*/

ValueMetadata my_extract_parameter_metadata(const SVFG &vfg, const Value *val,
                                            unsigned param_id) {
  // static parameter tracker
  // for each SVFG graph one tracker.
  static std::unordered_map<const SVFG *,
                            std::unique_ptr<param_access_tracker_t>>
      trackers;
  auto &tracker = trackers[&vfg];
  if (!tracker) {
    tracker = std::make_unique<param_access_tracker_t>(
        SVFIR::getPAG(), vfg.getPTA(), const_cast<SVFG *>(&vfg));
  }
  const FunObjVar *f = get_function(vfg, param_id);
  tracker->walk_inverted_scc(f);
  /*for (auto callee : ind_collected_functions)
    llvm::outs() << "[CALLEE] " << describe_callee(callee) << "\n";
  llvm::outs() << "number of callees: " << ind_collected_functions.size()
               << "\n";*/

  func_summary_t &s = tracker->get_summary(param_id);
  ValueMetadata mdata = s.effects;
  mdata.setValue(val);
  return mdata;
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
          (acNode.get_di_type() ? di_instr.gep_td_ok : di_instr.gep_td_null)++;
          skipNode = handleGep(vNode, acNode, ats, mdata);
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
