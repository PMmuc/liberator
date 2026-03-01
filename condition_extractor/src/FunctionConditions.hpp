#pragma once

#include "json/json.h"
#include <string>
#include <vector>

#include "Config.h"
#include "ValueMetadata.hpp"

namespace liberator {

class FunctionConditions {
private:
  typedef std::vector<ValueMetadata> params_metadata_t;
  // std::vector<AccessTypeSet> parameter_ats;
  // AccessTypeSet return_ats;
  params_metadata_t parameter_metadata;
  ValueMetadata return_metadata;
  std::string function_name;

public:
  void setFunctionName(std::string f) { function_name = f; }
  std::string getFunctionName() const { return function_name; }

  void addParameterMetadata(ValueMetadata par) {
    parameter_metadata.push_back(par);
  }

  int getParameterNum() { return parameter_metadata.size(); }

  void replaceParameterMetadata(int parm, ValueMetadata new_par) {
    parameter_metadata[parm] = new_par;
  }

  params_metadata_t get_parameters() const { return parameter_metadata; }

  ValueMetadata getParameterMetadata(int idx) {
    if (idx < 0 || idx >= parameter_metadata.size())
      assert("idx out of bounds!");

    return parameter_metadata[idx];
  }

  void setReturnMetadata(ValueMetadata ret) { return_metadata = ret; }
  ValueMetadata getReturnMetadata() const { return return_metadata; }

  // for using it in std::set
  bool operator<(const FunctionConditions &rhs) const {
    return function_name < rhs.function_name;
  }
  std::string getSummary();
  std::string toString(bool verbose);
};

using function_condition_set_t = std::map<std::string, FunctionConditions>;

void store_into_json_file(const function_condition_set_t &, const std::string &,
                          bool);
void store_into_text_file(const function_condition_set_t &, const std::string &,
                          bool);
Json::Value to_json(const function_condition_set_t &, bool verbose);
std::string to_string(const function_condition_set_t &, bool verbose);
std::string get_summary(const function_condition_set_t &);
void save_condition_set(const function_condition_set_t &cs, OutType out);

} // namespace liberator
