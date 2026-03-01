#pragma once

#include "json/json.h"

namespace llvm {
class Type;
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
Json::Value to_json(const ValueMetadata &v, bool verbose);
std::string to_string(const ValueMetadata &v, bool verbose);
std::string print_summary(const ValueMetadata &v);

} // namespace liberator
