#include "ValueMetadata.hpp"

#include "AccessTypeIO.h"
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

} // namespace liberator
