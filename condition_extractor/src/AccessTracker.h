#pragma once

#include "ValueMetadata.hpp"
#include <Graphs/GraphTraits.h>
#include <Graphs/ICFGNode.h>
#include <Graphs/SCC.h>
#include <MemoryModel/PointerAnalysis.h>
#include <Util/GeneralType.h>
#include <unordered_set>
namespace SVF {
class SVFIR;
class PointerAnalysis;
class SVFG;
class CallGraph;
} // namespace SVF

namespace liberator {

struct exit_state_t {
  SVF::NodeID formal_ret;
  AccessType at;

  bool operator<(const exit_state_t &o) const {
    return formal_ret != o.formal_ret ? formal_ret < o.formal_ret : at < o.at;
  }
};

struct func_summary_t {
  ValueMetadata effects;
  std::set<exit_state_t> exits;
};

struct ret_summary_t {
  ValueMetadata effects;
  /*
   * contains the
   */
  set<unsigned> return_params;
  const llvm::Type *type = nullptr;
  // the return type of the function.
  llvm::DIType *base_di = nullptr;

  bool widened = false;
};

class access_tracker_t {
  SVF::SVFIR *pag;
  SVF::PointerAnalysis *pta;
  SVF::SVFG *svfg;
  SVF::CallGraph *cg;
  SVF::SCCDetection<SVF::CallGraph *> *cg_scc;

  // FormalParamVFGNode ID -> node summary
  std::unordered_map<SVF::NodeID, func_summary_t> summaries;
  std::unordered_set<SVF::NodeID> scc_visited;
  // cache the backward slice for it to be readily available
  // in backward_slice
  std::unordered_map<const FunObjVar *, vector<NodeID>> ret_sources;
  // Function -> retunr summary
  unordered_map<const FunObjVar *, ret_summary_t> ret_summaries;

  typedef std::stack<SVF::NodeID> worklist_t;
  worklist_t inverted_scc;

  unsigned num_analyzed_functions = 0;
  unsigned num_analyzed_sccs = 0;
  unsigned total_cg_functions = 0;
  unsigned total_sccs = 0;
  unsigned bad_at_reports = 0;

  unordered_set<NodeID> validated;

public:
  access_tracker_t(SVF::SVFIR *svfir, SVF::PointerAnalysis *pta_,
                   SVF::SVFG *svfg_);

  static ValueMetadata extract_parameter_metadata(const SVFG &vfg,
                                                  const Value *val,
                                                  unsigned param_id);
  static ValueMetadata extract_return_metadata(const SVFG &vfg,
                                               const Value *ret_val,
                                               const FunObjVar *f);

  static access_tracker_t &get_tracker(const SVFG &vfg);
  /**
   * Walks the callgraph bottom-up for function f.
   */
  void walk_inverted_scc(const FunObjVar *f);

  /**
   * Returns the function summary to a FunObjVar.
   *
   * @param f for which we want the return summary.
   *
   * @return ret_summary_t for function f.
   */
  const ret_summary_t &get_ret_summary(const FunObjVar *f) {
    return ret_summaries[f];
  }

  /**
   * Returns the return summary to a FunObjVar.
   *
   * @param f for which we want the return function_summary.
   *
   * @return func_summary_t for function f.
   */
  func_summary_t &get_summary(SVF::NodeID param_id);

private:
  /**
   * @param cs the callsite for which to find the targets functions.
   * @return all possible target function belonging to a call. For indirect
   * calls there can be multiple target functions.
   */
  std::vector<const FunObjVar *> callee_targets(const CallICFGNode *cs);

  /**
   * @return all ids of FormalParmVFGNode of Function G that
   */
  vector<SVF::NodeID> formal_ids(const FunObjVar *G);

  void dump_scc_stats(int num_top = 10);
  void dump_indirect_block(int num_top = 10);
  std::vector<const FunObjVar *> functions_in_scc(SVF::NodeID rep);
  void validate_summary(SVF::NodeID formal_id, const func_summary_t &summ);

  /**
   *
   * Returns the number and members of the largest component in the call graph.
   *
   * @param keep - predicate for edges to keep in the graph.
   * @param members - the members of the largest component
   * @param cyclic_components - number of sccs with more than 1 component.
   * @return the number of elements in members.
   */
  template <typename Fn>
  size_t largest_component(Fn keep, std::vector<NodeID> &members,
                           size_t &cyclic_components);

  // void walk_scc(const FunObjVar *f);
  void process_scc(NodeID id);
  /**
   * TODO: maybe we can leave this all together, when we don't carry it in the
   * MetadataValue param.
   * @param formal_id - the id of the formal
   * @return the llvm type
   */
  const llvm::Type *formal_entry_type(SVF::NodeID formal_id);

  /**
   * @param formal_id the id of the FormalParmVFGNode
   * @return DIType the return type of the DIType.
   */
  llvm::DIType *formal_di_type(SVF::NodeID formal_id);
  /**
   * Counts the formals of a set of callgraph nodes.
   * That is how many summaries a component would have to fixpoint.
   */
  size_t formals_of(const std::vector<SVF::NodeID> &nodes);

  bool summarize_return(const FunObjVar *f);

  const VFGNode *def_node_of(const SVFVar *var);

  /**
   * Evaluates a summary of accesses starting from entry. Accesses are
   * (fields, type, kind) properties.
   */
  bool summarize_from(const VFGNode *entry, const llvm::Type *t,
                      llvm::DIType *di_type, func_summary_t &summ);

  /**
   * @return the return DIType of the function f.
   */
  llvm::DIType *ret_di_type(const FunObjVar *f);

  /**
   * @param f function to retrieve for FormalParmVFGNode from.
   * @param n the index of the parameter in the function f.
   * @return the FormalParmVFGNode ID of parameter n in function f or 0 on
   * error.
   */
  SVF::NodeID formal_id_of(const FunObjVar *f, int n);

  /**
   * @param f the function
   * @returns the ActualRetVFGNode
   */
  NodeID ret_id_of(const FunObjVar *f);

  /**
   * Dumps the number of elements in summary and it's memory usage.
   */
  void dump_mem_stats();

  /**
   * Prints the progress of the function analysis.
   */
  void report_progress(SVF::NodeID n);

  /**
   * Merges callee summary into current tracked summary and pushes returns from
   * the exit states to the worklist.
   *
   * @param succ - is the ActualVFGParm or ActualINVFGParm
   * @param cs - is the callsite
   * @param p - Path is the current tracked path
   * @param summ - is the summary
   * @param worklist - is the worklist.
   */
  void merge_summary(VFGNode *succ, const CallICFGNode *cs, Path &p,
                     func_summary_t &summ, vector<Path> &worklist);

  /**
   * Merge two access types.
   *
   * @param prefix (fields/kind/type)
   * @param suffix (fields/kind type)
   * @param callee_base_di - di type of the callee summary
   * @param addr_of - if parameter was passed as reference
   * @param actual_di - the actual type of the parameter
   */
  std::optional<AccessType> merge_access_type(const AccessType &prefix,
                                              const AccessType &suffix,
                                              llvm::DIType *callee_base_di,
                                              bool addr_of,
                                              llvm::DIType *actual_di);

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
  const VFGNode *get_resume_node(SVF::NodeID ret, const CallICFGNode *cs);
  /**
   * Returns the function summary for a formal parameter.
   *
   * @param NodeID of the formal parameter.
   * @param summ is resulting function summary.
   *
   * @return true if entry was added to summary, false otherwise.
   */
  bool summarize_formal(SVF::NodeID formal_id, func_summary_t &summ);

  /**
   * Starting from the return node of f, it retreives all allocation sites
   * (heap, stack) or callsites that went into the creation of the return
   * value.
   */
  const std::vector<SVF::NodeID> &backward_slice(const FunObjVar *f);
};

template <typename Fn>
size_t access_tracker_t::largest_component(Fn keep,
                                           std::vector<NodeID> &members,
                                           size_t &cyclic_components) {
  std::vector<SVF::NodeID> ids;
  std::unordered_map<SVF::NodeID, size_t> idx;

  // (NodeID, CallGraphNode*)
  for (auto &kv : *cg) {
    const size_t u = ids.size();
    idx[kv.first] = ids.size();
    ids.push_back(kv.first);
  }

  // ids number of nodes in the graph
  std::vector<vector<size_t>> adj(ids.size());

  for (const auto &kv : *cg) {
    const size_t u = idx[kv.first];
    for (auto &edge : kv.second->getOutEdges()) {
      if (!keep(edge)) {
        continue;
      }
      auto dst = idx.find(edge->getDstID());
      if (dst != idx.end()) {
        adj[u].push_back(dst->second);
      }
    }
  }
  const size_t n = ids.size();
  vector<size_t> dfs(n, -1);
  vector<pair<size_t, size_t>> wl;
  vector<size_t> low(n);
  vector<size_t> stack;
  vector<bool> on_stack(n, false);
  int next_index = 0;
  size_t largest = 0;
  cyclic_components = 0;

  for (size_t root = 0; root < n; ++root) {
    if (dfs[root] != -1)
      continue;

    dfs[root] = low[root] = next_index++;
    stack.push_back(root);
    on_stack[root] = true;
    wl.emplace_back(root, 0);

    while (!wl.empty()) {
      // pre-order
      const size_t v = wl.back().first;
      if (wl.back().second < adj[v].size()) {
        const size_t w = adj[v][wl.back().second++];
        if (dfs[w] == -1) {
          dfs[w] = low[w] = next_index++;
          stack.push_back(w);
          on_stack[w] = true;
          wl.emplace_back(w, 0);
        } else if (on_stack[w]) {
          // explored
          low[v] = min(low[v], dfs[w]);
        }
        continue;
      }
      // postorder
      wl.pop_back();
      if (!wl.empty())
        low[wl.back().first] = min(low[wl.back().first], low[v]);

      if (low[v] == dfs[v]) {
        std::vector<size_t> scc;
        size_t w;
        do {
          w = stack.back();
          stack.pop_back();
          on_stack[w] = false;
          scc.push_back(w);
        } while (v != w);

        if (scc.size() > 1)
          cyclic_components++;

        if (scc.size() > largest) {
          largest = scc.size();
          members.clear();
          for (size_t m : scc)
            members.push_back(ids[m]);
        }
      }
    }
  }
  return largest;
}

} // namespace liberator
