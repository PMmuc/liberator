#include "ValueMetadata.hpp"

#include "AccessTypeIO.h"

namespace liberator {

void ValueMetadata::add_len_source(const llvm::Value *fp, const Path *pp) {
  auto fp_v = const_cast<llvm::Value *>(fp);
  Path p = pp == nullptr ? Path(nullptr, nullptr, nullptr) : *pp;

  // The bottom-up analysis can reach the same call boundary once per fixpoint
  // iteration of an SCC, so drop entries we already recorded instead of
  // letting the vector grow with every round, preventing unnecessary
  // duplicates.
  for (const auto &el : length_sources)
    if (el.first == fp_v && !(el.second < p) && !(p < el.second) &&
        p.get_cs_stack() == el.second.get_cs_stack())
      return;

  // Value and path
  length_sources.push_back(std::make_pair(fp_v, p));
}

const std::vector<std::pair<llvm::Value *, Path>> &
ValueMetadata::get_len_source() const {
  return length_sources;
}

} // namespace liberator
