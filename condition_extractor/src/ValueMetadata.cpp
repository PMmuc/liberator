#include "ValueMetadata.hpp"

#include "json/json.h"
#include <sstream>

namespace liberator {

void ValueMetadata::addFunParam(const llvm::Value *fp, Path *pp) {
  auto fp_v = const_cast<llvm::Value *>(fp);
  if (pp == nullptr) {
    Path p(nullptr, nullptr, nullptr);
    auto el = std::make_pair(fp_v, p);
    fun_params.push_back(el);
  } else {
    auto el = std::make_pair(fp_v, *pp);
    fun_params.push_back(el);
  }
}

std::vector<std::pair<llvm::Value *, Path>> ValueMetadata::getFunParams() {
  return fun_params;
}

Json::Value ValueMetadata::toJson(bool verbose) {

  Json::Value medataResult;

  medataResult["access_type_set"] = ats.toJson(verbose);
  medataResult["is_array"] = this->is_array;
  medataResult["is_malloc_size"] = this->is_malloc_size;
  medataResult["is_file_path"] = this->is_file_path;
  medataResult["len_depends_on"] = this->len_depends_on;

  Json::Value setByJson(Json::arrayValue);
  for (auto d : this->set_by)
    setByJson.append(d);

  medataResult["set_by"] = setByJson;

  return medataResult;
}

std::string ValueMetadata::getSummary() {

  std::stringstream sstream;

  sstream << "ATS " << this->ats.size() << ", ";
  sstream << "array " << std::to_string(this->is_array) << ", ";
  sstream << "malloc " << std::to_string(this->is_malloc_size) << ", ";
  sstream << "path " << std::to_string(this->is_file_path) << ", ";
  sstream << "depends '" << len_depends_on << "'\n";

  return sstream.str();
}

std::string ValueMetadata::toString(bool verbose) {

  std::stringstream sstream;

  sstream << "is_array: " << std::to_string(this->is_array) << "\n";
  sstream << "is_malloc_size: " << std::to_string(this->is_malloc_size) << "\n";
  sstream << "is_file_path: " << std::to_string(this->is_file_path) << "\n";
  sstream << "len_depends_on: " << this->len_depends_on << "\n";

  std::string set_by_str = "";
  for (auto d : this->set_by)
    set_by_str += d + " ";

  sstream << "set_by: " << set_by_str << "\n";
  sstream << "access_type_set:\n" << ats.toString(verbose) << "\n";

  return sstream.str();
}
} // namespace liberator
