#pragma once

#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"

/**
 * All variables and their associated debug information type.
 */
using slot_type_map_t =
    std::unordered_map<const llvm::Value *, const llvm::DIType *>;
/**
 * Builds the slot map from SSA Debug records. Assumes that the target library
 * got build with -g flag. The slot map assigns a debug record to each variables
 * it defines. For example: int a = 3; in SSA: %0 = alloca i32, align 4
 * dbg_declare_value()
 *
 */
slot_type_map_t build_slot_map(const llvm::Module &M);

/**
 * Tries to resolve the debug information for an indirect call instruction.
 * For example:
 *    %2 = int %0(i32 10, ptr %1),
 * will try to track where %0 is resolved.
 * For that it uses intraprocedural static code analysis.
 * If %0 = load ptr, ptr %4, we track where %4 is allocated.
 *  -> %0 = load ptr, ptr @global1, align 8, !dbg !68
 * With the load operation we have debug information associated, that the global
 * got defined.
 * -> Get the type of @global1 -> cb_t -> create signature out of the callback
 * type information.
 * @return returns the debug record for the function pointer type it could
 * resolve.
 */
const llvm::DISubroutineType *resolve_callbase(const llvm::CallBase *cs,
                                               const slot_type_map_t &slots);
/**
 * From a llvm function we create a signature with complete type information.
 * Example: void (*)(int) -> FT[void B[32,signed]]
 * Example: int (*)(ClassA* a) -> FT[B[32,signed] TP[ST[ClassA]]]
 * @return the signature type as a string
 */
std::string signature(const llvm::DISubroutineType *sr);
/**
 * From a llvm function we create a signature with complete type information.
 * Example: FT[void BT[32,signed]]
 * @return the signature type as a string
 */
std::string signature(const llvm::Function *f);
