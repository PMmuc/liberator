#include "ValueMetadata.hpp"

#include "AccessTypeIO.h"
#include "json/json.h"
#include <sstream>

namespace liberator {

void ValueMetadata::addFunParam(const llvm::Value *fp, Path *pp) {
  auto fp_v = const_cast<llvm::Value *>(fp);
  Path p = pp == nullptr ? Path(nullptr, nullptr, nullptr) : *pp;

  // The bottom-up analysis can reach the same call boundary once per fixpoint
  // iteration of an SCC, so drop entries we already recorded instead of
  // letting the vector grow with every round.
  for (const auto &el : fun_params)
    if (el.first == fp_v && !(el.second < p) && !(p < el.second))
      return;

  // Value and path
  fun_params.push_back(std::make_pair(fp_v, p));
}

std::vector<std::pair<llvm::Value *, Path>> ValueMetadata::getFunParams() {
  return fun_params;
}

} // namespace liberator
