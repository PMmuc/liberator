#ifndef INCLUDE_DOM_ACCESSTYPE_HANDLER_H_
#define INCLUDE_DOM_ACCESSTYPE_HANDLER_H_

#include "AccessType.h"
#include "SVF-LLVM/LLVMModule.h"
#include "ValueMetadata.hpp"
#include <Util/Casting.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <set>
namespace liberator {
/**
 * Adds an Access write type to the mdata, given the
 * @param mdata - metadatavalue
 * @param atNode - return type
 * @param icfgNode - ICFGNode callsite
 */
void addWrteToAllFields(ValueMetadata *mdata, AccessType atNode,
                        const ICFGNode *icfgNode);

// H_SCOPE is a masked with C_RETURN and C_PARAM  asdf
// C_RETURN -> the handler is invoked by extractReturnMetadata
// C_PARAM -> the handler is invoked by extractParameterMetadata
#define C_RETURN 1 // 01
#define C_PARAM 2  // 10
typedef unsigned short H_SCOPE;

typedef bool (*Handler)(ValueMetadata *, std::string, const ICFGNode *,
                        const CallICFGNode *, int, AccessType, H_SCOPE, Path *);
typedef std::map<std::string, Handler> AccessTypeHandlerMap;
extern AccessTypeHandlerMap accessTypeHandlers;
} // namespace liberator
#endif /* INCLUDE_DOM_ACCESSTYPE_HANDLER_H_ */
