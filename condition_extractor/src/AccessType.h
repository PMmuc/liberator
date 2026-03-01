#ifndef INCLUDE_DOM_ACCESSTYPE_H_
#define INCLUDE_DOM_ACCESSTYPE_H_

#include "Graphs/ICFG.h"
#include "Graphs/SVFG.h"
#include "SVFIR/SVFVariables.h"
#include "WPA/Andersen.h"
#include <Graphs/GenericGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/raw_ostream.h>

#include "PhiFunction.h"
#include "TypeMatcher.h"
#include <fstream>
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
  std::set<const ICFGNode *> icfg_set;
  std::vector<int> fields;
  kind_e access;
  const llvm::Type *type;

  // fake parent
  bool has_parent_;
  std::vector<int> p_fields;
  kind_e p_access;
  const llvm::Type *p_type;

  // original casted type
  const llvm::Type *c_type;

  // remember the types extracted from previous GEP
  std::set<const llvm::Type *> visited_types;

public:
  AccessType(const llvm::Type *t) {
    access = kind_e::none;
    p_access = kind_e::none;
    has_parent_ = false;
    type = t;
    p_type = nullptr;
    c_type = nullptr;
  }
  ~AccessType() { fields.clear(); }

  void add_visited_type(llvm::Type *a_type) { visited_types.insert(a_type); }

  bool is_visited(llvm::Type *a_type) {
    return visited_types.find(a_type) != visited_types.end();
  }
  const std::vector<int> &get_parent_fields() const { return p_fields; }

  kind_e get_parent_kind() const { return p_access; }
  const llvm::Type *get_parent_llvm_type() const { return p_type; }

  // copy assignment operator
  AccessType &operator=(const AccessType &rhs) {
    this->fields = rhs.fields;
    this->access = rhs.access;
    this->type = rhs.type;

    // hope this does not make a mess!
    this->icfg_set = rhs.icfg_set;

    // parent
    this->has_parent_ = rhs.has_parent_;
    this->p_fields = rhs.p_fields;
    this->p_access = rhs.p_access;
    this->p_type = rhs.p_type;

    // visited types for GEP recursion
    this->visited_types = rhs.visited_types;

    return *this;
  };

  void addICFGNode(const ICFGNode *icfg_node) { icfg_set.insert(icfg_node); }

  std::set<const ICFGNode *> getICFGNodes() const { return icfg_set; }

  const llvm::Type *getOriginalCastType() { return c_type; }

  void setOriginalCastType(const llvm::Type *t) { c_type = t; }

  void addField(int a_field) {
    // fake father? find better way to do so
    p_fields = fields;
    p_access = access;
    p_type = type;
    has_parent_ = true;
    fields.push_back(a_field);
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

  void set_llvm_type(const llvm::Type *typ) { type = typ; }

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
    return true;
  }

  std::string dumpICFGNodes() const {

    std::string str;
    raw_string_ostream rawstr(str);

    for (auto inst : getICFGNodes())
      rawstr << inst->toString() << "; \n";
    rawstr << "\n";

    return rawstr.str();
  }

  // for std:set
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
  // std::map<AccessType, std::set<const ICFGNode*>> ats;
  std::set<AccessType> ats_set;

public:
  void insert(AccessType at, const ICFGNode *inst) {
    // outs() << "[DEBUG] insert: " << at.toString() << "\n";

    // at is already in the set
    auto at_iter = ats_set.find(at);
    if (at_iter != ats_set.end()) {
      AccessType at_prev = *at_iter;
      at_prev.addICFGNode(inst);
      ats_set.erase(at_iter);
      // ats_set.erase(at_prev);
      ats_set.insert(at_prev);
    } else {
      at.addICFGNode(inst);
      ats_set.insert(at);
    }
  }

  void remove(AccessType at) {
    auto at_iter = ats_set.find(at);
    if (at_iter != ats_set.end()) {
      ats_set.erase(*at_iter);
    }
  }

  size_t size() const { return ats_set.size(); }

  std::set<const ICFGNode *> getAllICFGNodes() const {
    std::set<const ICFGNode *> allNodes;

    for (auto at : ats_set) {
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
};

class Path {
private:
  const VFGNode *node;
  AccessType access_type;
  const Value *prevValue;
  std::stack<const CallICFGNode *> stack;
  std::vector<std::pair<const ICFGNode *, AccessType>> history;
  friend std::string to_string(const Path &);

public:
  // Path(const VFGNode* p_node) {
  //     node = p_node;
  //     prevValue = nullptr;
  // }

  // BUG: this is probably a bug where val is not used in the constructor.
  Path(const VFGNode *p_node, const llvm::Value *val, const llvm::Type *type)
      : access_type(type) {
    node = p_node;
    prevValue = nullptr;
    // access_type.setType(val->getType());
  }

  void addStep(const ICFGNode *node) {
    history.push_back(std::make_pair(node, getAccessType()));
  }

  const std::vector<std::pair<const ICFGNode *, AccessType>> getSteps() {
    return history;
  }

  const Value *getPrevValue() { return prevValue; }

  void setPrevValue(const Value *a_prevValue) { prevValue = a_prevValue; }

  const VFGNode *getNode() { return node; }

  void setNode(const VFGNode *a_node) { node = a_node; }

  AccessType getAccessType() { return access_type; }

  void setAccessType(AccessType a_access_type) { access_type = a_access_type; }

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

  // copy assignment operator
  Path &operator=(const Path &rhs) {
    this->node = rhs.node;
    this->access_type = rhs.access_type;
    this->stack = rhs.stack;

    this->history = rhs.history;

    return *this;
  };
};

} // namespace liberator

#endif /* INCLUDE_DOM_ACCESSTYPE_H_ */
