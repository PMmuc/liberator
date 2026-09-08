#ifndef INCLUDE_DOM_ACCESSTYPE_H_
#define INCLUDE_DOM_ACCESSTYPE_H_

#include "Graphs/ICFG.h"
#include "Graphs/SVFG.h"
#include "SVFIR/SVFVariables.h"
#include "WPA/Andersen.h"
#include <Graphs/GenericGraph.h>
#include <Util/GeneralType.h>
#include <cstddef>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <map>
#include <utility>

using namespace SVF;
using namespace llvm;
using namespace std;

namespace Json {
class Value;
}

namespace liberator {

class AccessType {
public:
  enum class kind_e {
    none,
    read,
    write,
    ret,
    del,
    create,
    file,
    input_stream,
    output_stream
  };

private:
  friend Json::Value to_json(const AccessType &, bool);
  // This set contains all the ICFGNodes that made up this AccessType.
  mutable std::set<const ICFGNode *> icfg_set;
  /*
   * contains the struct or array fields that were tracked.
   */
  std::vector<int> fields;
  /*
   * contains the type of access (read, write, create)
   */
  kind_e access;
  /*
   * contains the type
   */
  const llvm::Type *type;
  llvm::DIType *di_type;

  // fake parent
  bool has_parent_;
  std::vector<int> p_fields;
  kind_e p_access;
  const llvm::Type *p_type;
  DIType *p_di_type;

  // original casted type
  const llvm::Type *c_type;

  // counts how often a field of a type got accessed.
  // should prevent type recursion in a path.
  // mutable for the same reason icfg_set is: AccessTypes live inside a
  // std::set whose ordering (operator<) depends only on fields/access/type, so
  // updating the recursion counters through a set iterator cannot break the
  // container invariant.
  mutable std::map<std::pair<const llvm::Type *, int>, int> visited_types;

public:
  AccessType(const llvm::Type *t, llvm::DIType *di = nullptr) {
    access = kind_e::none;
    p_access = kind_e::none;
    has_parent_ = false;
    type = t;
    p_type = nullptr;
    c_type = nullptr;
    di_type = di;
    p_di_type = nullptr;
  }

  // copy assignment operator
  AccessType(const AccessType &rhs) = default;
  AccessType &operator=(const AccessType &rhs) = default;
  // move operator
  AccessType(AccessType &&) = default;
  AccessType &operator=(AccessType &&) = default;

  ~AccessType() = default;

  DIType *get_di_type() const { return di_type; }
  DIType *get_p_di_type() const { return p_di_type; }

  void add_visited_type(const llvm::Type *a_type, int field) {
    visited_types[{a_type, field}]++;
  }

  void add_visited_count(const llvm::Type *a_type, int field, int count) {
    visited_types[{a_type, field}] += count;
  }

  /**
   * Fold another AccessType's recursion counters into this one, keeping the
   * LARGER count per key.
   *
   * Needed because operator</operator== compare only (fields, access, type) -
   * visited_types is deliberately not part of the identity. So two AccessTypes
   * that reached the same field path by different routes, carrying different
   * recursion counts, collapse onto one entry in an AccessTypeSet. Without
   * this merge the first-inserted counters win and every later (higher) count
   * is discarded, leaving the set holding the MINIMUM counts ever seen.
   * merge_access_type() then reads those understated counters and permits
   * compositions that should have exceeded MAX_GEP_RECURSION_DEPTH - the
   * recursion bound silently weakens the more paths converge on a key.
   *
   * max is the sound choice for a budget: it keeps the tightest constraint.
   */
  void merge_visited_max(const AccessType &other) const {
    for (const auto &kv : other.visited_types) {
      auto &slot = visited_types[kv.first];
      if (kv.second > slot)
        slot = kv.second;
    }
  }

  const std::map<std::pair<const llvm::Type *, int>, int> &
  get_visited_types() const {
    return visited_types;
  }

  // number of times (a_type, field) has already been followed on this path
  int visit_count(const llvm::Type *a_type, int field) const {
    auto it = visited_types.find({a_type, field});
    return it == visited_types.end() ? 0 : it->second;
  }
  const std::vector<int> &get_parent_fields() const { return p_fields; }

  kind_e get_parent_kind() const { return p_access; }
  const llvm::Type *get_parent_llvm_type() const { return p_type; }

  void addICFGNode(const ICFGNode *icfg_node) const {
    icfg_set.insert(icfg_node);
  }

  std::set<const ICFGNode *> &getICFGNodes() { return icfg_set; }
  const std::set<const ICFGNode *> &getICFGNodes() const { return icfg_set; }

  const llvm::Type *getOriginalCastType() { return c_type; }

  void setOriginalCastType(const llvm::Type *t) { c_type = t; }

  void addField(int a_field) {
    // fake father? find better way to do so
    p_fields = fields;
    p_access = access;
    p_type = type;
    p_di_type = di_type;
    has_parent_ = true;
    fields.push_back(a_field);
  }

  void append_path(const std::vector<int> &suffix_fields, kind_e parent_access,
                   const llvm::Type *parent_ty, llvm::DIType *parent_di) {
    if (suffix_fields.empty()) {
      return;
    }
    fields.insert(fields.end(), suffix_fields.begin(), suffix_fields.end());
    p_fields.assign(fields.begin(), fields.end() - 1);
    p_access = parent_access;
    p_type = parent_ty;
    p_di_type = parent_di;
    has_parent_ = true;
  }

  std::vector<int> &get_fields() { return fields; }
  const std::vector<int> &get_fields() const { return fields; }

  int get_num_fields() const { return fields.size(); }

  void removeLastField() {
    if (get_num_fields() == 0)
      return;
    fields.pop_back();
  }

  int getLastField() {
    if (get_num_fields() == 0)
      return -1;
    return fields.back();
  }

  void set_kind(kind_e a_access) { access = a_access; }

  kind_e get_kind() const { return access; }

  void set_llvm_type(const llvm::Type *typ, llvm::DIType *di_typ) {
    type = typ;
    di_type = di_typ;
  }

  const llvm::Type *get_llvm_type() const { return type; }

  inline bool has_parent() const { return has_parent_; }

  // void clone() {
  //     unsigned int new_ID = ++global_object_id;
  //     parent_ID = ID;
  //     ID = new_ID;
  // }

  bool equals(std::string s) const;

  bool operator==(const AccessType &other) const {
    if (other.fields != fields)
      return false;
    if (other.access != access)
      return false;
    if (other.type != type)
      return false;
    // TODO: Maybe add di_type?
    return true;
  }

  std::string dumpICFGNodes() const {

    std::string str;
    raw_string_ostream rawstr(str);

    for (auto &inst : getICFGNodes())
      rawstr << inst->toString() << "; \n";
    rawstr << "\n";

    return rawstr.str();
  }

  // for std::set
  bool operator<(const AccessType &rhs) const {
    if (fields == rhs.fields) {
      if (access == rhs.access)
        return type < rhs.type;
      else
        return access < rhs.access;
    }

    return fields < rhs.fields;
  }
};

class AccessTypeSet {
private:
  typedef std::set<AccessType> container_t;
  typedef container_t::iterator iterator;
  typedef container_t::const_iterator const_iterator;
  container_t ats_set;

public:
  /**
   * Add the ICFGNode inst to the AccessType at.
   * If it does not exists yet, create it in the map.
   * Note: This depends on the fact, that AccessType's odering will not
   * use ICFGNode for its less comparison, otherwise this code will be undefined
   * behaviour.
   * @return an iterator to the updated value.
   */
  const_iterator insert(const AccessType &at, const ICFGNode *inst) {
    // outs() << "[DEBUG] insert: " << at.toString() << "\n";
    auto it = ats_set.find(at);
    if (it == ats_set.end())
      it = ats_set.insert(at).first;
    else
      // keep the tightest recursion budget; see merge_visited_max()
      it->merge_visited_max(at);
    return it;
  }

  template <typename T>
  const_iterator insert_nodes(const AccessType &at, const T &nodes) {
    auto it = ats_set.find(at);
    if (it == ats_set.end())
      it = ats_set.insert(at).first;
    else
      // keep the tightest recursion budget; see merge_visited_max()
      it->merge_visited_max(at);
    for (const ICFGNode *n : nodes) {
      it->addICFGNode(n);
    }
    return it;
  }

  void remove(AccessType &at) {
    auto at_iter = ats_set.find(at);
    if (at_iter != ats_set.end()) {
      ats_set.erase(*at_iter);
    }
  }

  size_t size() const { return ats_set.size(); }

  std::set<const ICFGNode *> getAllICFGNodes() const {
    std::set<const ICFGNode *> allNodes;

    for (auto &at : ats_set) {
      for (auto icfg_node : at.getICFGNodes()) {
        allNodes.insert(icfg_node);
      }
    }

    return allNodes;
  }

  std::set<AccessType>::iterator begin() const { return ats_set.begin(); }
  std::set<AccessType>::iterator end() const { return ats_set.end(); }

  bool operator<(const AccessTypeSet &rhs) const {
    return ats_set < rhs.ats_set;
  }
  bool operator==(const AccessTypeSet &rhs) const {
    return ats_set == rhs.ats_set;
  }
  bool operator!=(const AccessTypeSet &rhs) const { return !(*this == rhs); }
};

// first we need to define what the incoming state is.
// The incoming state must uniqly identify the function call as already
// processed. What happens when the function parameter decides the execution
// path taken and the write and read accesses to that? do we not care and
// report that still or do we distinguish that case? In case we distinguish,
// the state must reflect that. The caller does not play a role in the memo.
// Because the caller will just use the results of the saved value to change
// it's locale state. But execution does not change based on passed parameter
// value. But in the caller itself the return values are important. Each
// possible return state must be tracked, when it returns references as this
// will change the accesses to the parameter in the caller.
//

struct memo_state_t {
  // node id - we need to know if the node that calls the callee is the same.
  SVF::NodeID node;
  // fields - Because we want to stay field sensitive, we need to track the
  // different field accesses separately.
  vector<int> fields;
  // we need to differentiate the kind. A path that reads should be different
  // to a path that writes.
  AccessType::kind_e access;
  // The type of the struct currently tracked by this state.
  // This is the same as in the original liberator. Although the type is not
  // as useful anymore as we have opaque pointer, it can still serve as an
  // indicaton of if we are on the right track.
  const llvm::Type *type;
  // The previous value makes it possible to differentiate for a store node,
  // if the access is read or write.
  llvm::Value *prev_value;

  bool operator==(const memo_state_t &o) const {
    return node == o.node && o.access == access && type == o.type &&
           fields == o.fields && prev_value == o.prev_value;
  }
};

class Path {
private:
  const VFGNode *node;
  AccessType access_type;
  const Value *prevValue;
  std::stack<const CallICFGNode *> stack;
  // std::vector<std::pair<const ICFGNode *, AccessType>> history;
  friend std::string to_string(const Path &);

public:
  // Path(const VFGNode* p_node) {
  //     node = p_node;
  //     prevValue = nullptr;
  // }

  // BUG: this is probably a bug where val is not used in the constructor.
  Path(const VFGNode *p_node, const llvm::Value *val, const llvm::Type *type,
       llvm::DIType *di = nullptr)
      : access_type(type, di) {
    node = p_node;
    prevValue = nullptr;
    // access_type.setType(val->getType());
  }

  // copy assignment operator
  Path(const Path &rhs) = default;
  Path(Path &&) = default;
  Path &operator=(const Path &) = default;
  Path &operator=(Path &&) = default;
  ~Path() = default;

  /*void addStep(const ICFGNode *node) {
    history.push_back(std::make_pair(node, get_access_type()));
  }

  size_t history_size() const { return history.size(); }

  const std::vector<std::pair<const ICFGNode *, AccessType>> getSteps() {
    return history;
  }*/

  const Value *getPrevValue() { return prevValue; }

  void setPrevValue(const Value *a_prevValue) { prevValue = a_prevValue; }

  const VFGNode *getNode() const { return node; }

  void setNode(const VFGNode *a_node) { node = a_node; }

  const AccessType &get_access_type() const { return access_type; }

  void set_access_type(const AccessType &a_access_type) {
    access_type = a_access_type;
  }
  void set_access_type(AccessType &&a_access_type) {
    access_type = std::move(a_access_type);
  }

  bool isCorrect(const CallICFGNode *edge) {
    if (getStackSize() == 0)
      return false;
    return stack.top() == edge;
  }

  const CallICFGNode *topFrame() {
    if (getStackSize() == 0)
      return nullptr;
    return stack.top();
  }

  void pushFrame(const CallICFGNode *cs) { stack.push(cs); }

  void popFrame() { stack.pop(); }

  uint getStackSize() { return stack.size(); }

  void dump() {
    // for (auto n: get_full_path()) {
    //     outs() << n->toString() << "\n";
    // }
    outs() << "<TBI>!!\n";
  }

  void dump_stack() {
    auto stk_copy(this->stack);
    while (!stk_copy.empty()) {
      auto f = stk_copy.top();
      outs() << f->toString() << "\n";
      stk_copy.pop();
    }
  }

  // for using it in std::set
  bool operator<(const Path &rhs) const {
    if (node == rhs.node)
      return access_type < rhs.access_type;
    else
      return node < rhs.node;
  }
};

/**
 * Counters describing how merge_access_type() decided whether a callee summary
 * may be composed onto the caller path. The counting itself is always on (a
 * handful of increments), the report is printed once at the end of the run and
 * only when -metrics is given.
 */
struct compose_instr_t {
  size_t attempts = 0;    // compositions with a non-empty suffix path
  size_t di_match = 0;    // DWARF says same record -> allowed
  size_t di_reject = 0;   // DWARF says different records -> rejected
  size_t llvm_match = 0;  // no usable DWARF, LLVM types equal -> allowed
  size_t llvm_reject = 0; // no usable DWARF, distinct structs -> rejected
  size_t undecidable = 0; // neither view discriminates -> allowed
  // Why DWARF declined: no DIType at all, vs. one that
  // di_record_name() rejects because it only accepts NAMED composites.
  size_t di_absent = 0;        // prefix or callee base DIType is null
  size_t di_not_composite = 0; // decays to a basic type (int, char)
  size_t di_unnamed = 0;       // composite but anonymous

  /// Process-wide counters, shared by every param_access_tracker_t.
  static compose_instr_t &instance();

  void dump(llvm::raw_ostream &os) const;
};

/**
 * Writes the instrumentation counters collected during the analysis, if
 * config_t::print_metrics is set. Meant to be called once, after the analysis
 * finished.
 */
void dump_metrics(llvm::raw_ostream &os);

} // namespace liberator

#endif /* INCLUDE_DOM_ACCESSTYPE_H_ */
