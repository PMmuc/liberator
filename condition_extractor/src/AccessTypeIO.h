#pragma once

#include "json/json.h"

namespace llvm {
class Type;
class DIType;
}

namespace liberator {
class AccessTypeSet;
class AccessType;
class Path;
class ValueMetadata;

Json::Value to_json(const AccessTypeSet &ats, bool verbose = false);
std::string to_string(const AccessTypeSet &ats, bool verbose = false);
Json::Value to_json(const AccessType &at, bool verbose = false);
std::string to_string(const AccessType &at, bool verbose = false);
std::string to_string_parent(const AccessType &at);
Json::Value to_json_parent(const AccessType &at);
std::string to_string(const Path &p);
std::string to_string(const llvm::Type *typ);
/**
 * Prints a DWARF type in old (pre-opaque-pointer) LLVM IR type syntax, e.g.
 * "i32", "i8*", "%struct.A*", "%struct.A = type { i32, i8* }", "[4 x i8*]".
 */
std::string to_string(const llvm::DIType *di);
Json::Value to_json(const ValueMetadata &v, bool verbose);
std::string to_string(const ValueMetadata &v, bool verbose);
std::string print_summary(const ValueMetadata &v, bool verbose = false);

} // namespace liberator
