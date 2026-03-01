#pragma once

#include "json/json.h"
#include <Graphs/ICFGNode.h>
#include <map>
#include <string>
#include <vector>

#include "AccessType.h"

namespace llvm {
class Value;
}

namespace liberator {
class ValueMetadata {
  friend Json::Value to_json(const ValueMetadata &, bool);
  AccessTypeSet ats;
  bool is_array;
  bool is_malloc_size;
  bool is_file_path;
  std::string len_depends_on;
  std::vector<std::string> set_by;

  const llvm::Value *val;
  std::vector<llvm::Value *> indexes;
  std::vector<std::pair<llvm::Value *, Path>> fun_params;
  friend std::string to_string(const ValueMetadata &, bool);
  friend std::string print_summary(const ValueMetadata &);

public:
  ValueMetadata() {
    val = nullptr;
    is_array = false;
    is_malloc_size = false;
    is_file_path = false;
    len_depends_on = "";
  }

  void setValue(const llvm::Value *p_val) { val = p_val; }
  const llvm::Value *getValue() { return val; }

  AccessTypeSet &get_access_type_set() { return ats; }
  const AccessTypeSet &get_access_type_set() const { return ats; }

  void addIndex(const llvm::Value *idx) {
    indexes.push_back(const_cast<llvm::Value *>(idx));
  }
  std::vector<llvm::Value *> getIndexes() { return indexes; }

  std::vector<std::pair<llvm::Value *, Path>> getFunParams();

  void addFunParam(const llvm::Value *fp, Path *pp);

  void setAccessTypeSet(AccessTypeSet p_ats) { ats = p_ats; }
  AccessTypeSet *getAccessTypeSet() { return &ats; }
  int getAccessNum() { return ats.size(); }

  void setIsArray(bool p_is_array) { is_array = p_is_array; }
  bool isArray() { return is_array; }

  void setMallocSize(bool p_malloc_size) { is_malloc_size = p_malloc_size; }
  bool isMallocSize() { return is_malloc_size; }

  void setIsFilePath(bool p_is_file_path) { is_file_path = p_is_file_path; }
  bool isFilePath() { return is_file_path; }

  void setLenDependency(std::string p_dep) { len_depends_on = p_dep; }
  std::string getLenDependency() { return len_depends_on; }

  void addSetByDependency(std::string p_dep) { set_by.push_back(p_dep); }
  std::vector<std::string> getSetByDependency() { return set_by; }

  // // copy assignment operator
  // ValueMetadata& operator=(const ValueMetadata &rhs) {

  //     this->val = rhs.val;
  //     this->is_array = rhs.is_array;
  //     this->is_malloc_size = rhs.is_malloc_size;
  //     this->is_file_path = rhs.is_file_path;
  //     this->len_depends_on = rhs.len_depends_on;

  //     // this->fields = rhs.fields;
  //     // this->access = rhs.access;
  //     // this->type = rhs.type;

  //     // // hope this does not make a mess!
  //     // this->icfg_set = rhs.icfg_set;

  //     // // parent
  //     // this->has_parent = rhs.has_parent;
  //     // this->p_fields = rhs.p_fields;
  //     // this->p_access = rhs.p_access;
  //     // this->p_type = rhs.p_type;

  //     // // visited types for GEP recursion
  //     // this->visited_types = rhs.visited_types;

  //     return *this;
  // };

public: // static functions/data!
  // handle debug information

  typedef std::map<const SVF::CallICFGNode *, std::set<const SVF::FunObjVar *>>
      MyCallEdgeMap;

  static MyCallEdgeMap myCallEdgeMap_inst;
};

/**
 * Retrieves the return type information of the return value and puts it into
 * ValueMetadata.
 *
 * @param vfg the value flow graph of the current programa
 * @param llvmval - the return value (not the return instruction)
 */
ValueMetadata extractReturnMetadata(const SVFG &vfg, const Value *llvmval);
/**
 * Retrieves the type information of a parameter and puts it into the
 * ValueMetadata struct.
 *
 * @param svfg - the sparse value frow graph
 * @param Value - value (formal) parameter of a function to extract the
 * ValueMetadata from
 * @param Type - the type of the Value
 * @param paramId - the SVFVar Node ID of the parameter
 */
ValueMetadata extractParameterMetadata(const SVFG &, const llvm::Value *,
                                       const llvm::Type *, unsigned);
std::vector<std::string> extractDependencyAmongParameters(const SVF::SVFVar *,
                                                          ValueMetadata &,
                                                          SVF::SVFG &,
                                                          const FunObjVar *);
std::string extractLenDependencyParameter(const SVF::SVFVar *, ValueMetadata &,
                                          SVF::SVFG &, const FunObjVar *);
} // namespace liberator
