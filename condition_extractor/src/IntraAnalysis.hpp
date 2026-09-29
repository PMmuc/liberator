#pragma once

#include "AccessType.h"
#include <llvm/IR/Value.h>

#define MAX_GEP_RECURSION_DEPTH 1

namespace SVF {
class VFGNode;
}

namespace liberator {

class AccessTypeSet;
class ValueMetadata;
class Path;

// the result after intraprocedural analysis. This determines the access_type
struct local_result_t {
  AccessType ac_node;
  const llvm::Value *prev_value;
  bool skip; // don't expand successors/prune search
};

/**
 * This function handles getelementptr instructions for struct fields, arrays
 * and pointer arithmetic.
 * @param vNode - should be a GepVFGNode
 * @param acNode - current access_type
 * @param ats - current access_type_set
 * @param mdata - update the mdata
 */
bool handleGep(const SVF::VFGNode *vNode, AccessType &acNode,
               AccessTypeSet &ats, ValueMetadata &mdata, Path &p);
void handleActualParam(const SVF::VFGNode *vNode, AccessType &acNode,
                       ValueMetadata &mdata, Path &p);
local_result_t transfer_function(const SVF::VFGNode *vNode, AccessType acNode,
                                 ValueMetadata &mdata, Path &p);

} // namespace liberator
