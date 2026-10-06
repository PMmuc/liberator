#ifndef INCLUDE_DOM_ACCESSTYPE_HANDLER_H_
#define INCLUDE_DOM_ACCESSTYPE_HANDLER_H_

#include "AccessType.h"
#include "ValueMetadata.hpp"
#include <Graphs/ICFGNode.h>
#include <Util/Casting.h>
#include <functional>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
// H_SCOPE is a masked with C_RETURN and C_PARAM  asdf
// C_RETURN -> the handler is invoked by extractReturnMetadata
// C_PARAM -> the handler is invoked by extractParameterMetadata
#define C_RETURN 1 // 01
#define C_PARAM 2  // 10
typedef unsigned short H_SCOPE;

namespace liberator {
/**
 * Finds the return type of the function that icfgNode belongs to.
 * Adds an Access write type to the mdata, given the type.
 * If the type is a pointer a simple derefrence is added (-1).
 * If the type is a struct a write to all fields is added.
 *
 * @param mdata - metadatavalue
 * @param atNode - return type
 * @param icfgNode - ICFGNode callsite
 */
void addWrteToAllFields(ValueMetadata &mdata, AccessType atNode,
                        const ICFGNode *icfgNode);

/**
If exists, call the predefined handler for function fun.

@param: mdata: the access type set to be updated by the handler
@param: fun: the name of the called function
@param: ifcgNode: the function currently analyzed
@param: cs: the callsite node
@param: param_num: the parameter number

@return: boolean value indicating if the analysis should continue on the
subfield. For example, it might be false for a cast to indicate we do not try
to follow further child of the node. default true.
*/
bool handlerDispatcher(ValueMetadata &mdata, const std::string &fun,
                       const ICFGNode *icfgNode, const CallICFGNode *cs,
                       int param_num, AccessType atNode, H_SCOPE h_scope,
                       liberator::Path *path);

/**
It checks if the target function is handled by our dispatchers.

@param: ats: the access type set to be updated by the handler
@param: fun: the name of the function
@param: node: the node currently analyzed

@return: boolean value indicating if the function is handled by our
dispatchers
*/
bool hasHandlerDispatcher(ValueMetadata *mdata, const std::string &fun,
                          const ICFGNode *icfgNode, const CallICFGNode *cs,
                          int param_num, H_SCOPE h_scope);

typedef function<bool(ValueMetadata &, string, const ICFGNode *,
                      const CallICFGNode *, int, AccessType, H_SCOPE, Path *)>
    handler_t;
typedef std::map<std::string, handler_t> AccessTypeHandlerMap;
extern AccessTypeHandlerMap accessTypeHandlers;
static handler_t handler_from_annotation(const std::string &name);
} // namespace liberator
#endif /* INCLUDE_DOM_ACCESSTYPE_HANDLER_H_ */
