#include "AccessTypeIO.h"
#include "AccessType.h"
#include "DebugInfoParser.hpp" // peel_di_qualifiers
#include "TypeMatcher.h"
#include "ValueMetadata.hpp"
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfoMetadata.h>

#include <llvm/Support/Casting.h>
#include <unordered_map>
#include <unordered_set>

namespace liberator {
std::string to_string(AccessType::kind_e k) {
  if (k == AccessType::kind_e::read)
    return "read";
  else if (k == AccessType::kind_e::write)
    return "write";
  else if (k == AccessType::kind_e::ret)
    return "return";
  else if (k == AccessType::kind_e::none)
    return "none";
  else if (k == AccessType::kind_e::del)
    return "delete";
  else if (k == AccessType::kind_e::create)
    return "create";
  else {
    outs() << "[ERROR] Access:: " << (int)k << " unknown!!\n";
    exit(1);
  }
}

std::string to_string_parent(const AccessType &at) {
  std::string str;
  raw_string_ostream rawstr(str);

  // example of output:
  // (., write) -> write the whole pointer (all the fields)
  // (.1, read) -> read field in position 1
  // (.0.1, write) -> write subfield 1 of the field 0

  rawstr << "(.";
  int max_fields = at.get_num_fields();
  int i = 0;
  for (int f : at.get_fields()) {
    if (f == -1)
      rawstr << "*";
    else
      rawstr << f;
    if (i < max_fields - 1)
      rawstr << ".";
    i++;
  }

  rawstr << ", ";
  rawstr << to_string(at.get_kind());
  rawstr << ", " << to_string(at.get_di_type());

  return rawstr.str();
}

std::string to_string(const llvm::Type *typ) {
  if (!typ)
    return "no llvm type";
  std::string str;
  llvm::raw_string_ostream(str) << *typ;
  return str;
}

const std::string *di_key(const llvm::DIType *di) {
  if (!di)
    return nullptr;

  static std::unordered_map<const llvm::DIType *, const std::string *> di2str;
  static std::unordered_set<string> di_types;

  auto it = di2str.find(di);
  if (it != di2str.end())
    return it->second;

  const std::string *key = &*di_types.insert(build_di_key(di)).first;
  di2str.emplace(di, key);
  return key;
}

std::string to_string(const llvm::DIType *di) {
  return print_di_type(di, true, 0);
}

/**
 * @param at AccessType to print.
 * @param verbose if true prints all ICFGNodes that accessed that type.
 */
std::string to_string(const AccessType &at, bool verbose) {

  std::string str;
  raw_string_ostream rawstr(str);

  // example of output:
  // (., write) -> write the whole pointer (all the fields)
  // (.1, read) -> read field in position 1
  // (.0.1, write) -> write subfield 1 of the field 0

  rawstr << "(";

  if (at.has_parent()) {
    AccessType p(at.get_parent_llvm_type());
    // AccessType p;
    // p.setType(p_type);
    p.set_kind(at.get_parent_kind());
    for (auto f : at.get_parent_fields())
      p.addField(f);
    rawstr << to_string_parent(p) << ",";
  } else
    rawstr << "(0),";

  rawstr << ".";
  int max_fields = at.get_num_fields();
  int i = 0;
  for (int f : at.get_fields()) {
    if (f == -1)
      rawstr << "*";
    else
      rawstr << f;
    if (i < max_fields - 1)
      rawstr << ".";
    i++;
  }

  rawstr << ", ";
  rawstr << to_string(at.get_kind());
  rawstr << ", " << to_string(at.get_llvm_type());
  rawstr << ", " << to_string(at.get_di_type());
  rawstr << ", " << TypeMatcher::compute_hash(at.get_di_type()) << ")";

  if (verbose) {
    rawstr << "\n";
    rawstr << at.dumpICFGNodes();
  }

  return rawstr.str();
}

Json::Value dumpICFGNodesJson() {

  Json::Value debugInfo(Json::arrayValue);

  // for (auto inst: getICFGNodes())
  //     debugInfo.append(inst->toString());

  return debugInfo;
}

Json::Value to_json_parent(const AccessType &at) {
  Json::Value accessTypeJson;

  accessTypeJson["access"] = to_string(at.get_kind());

  Json::Value fieldsJson(Json::arrayValue);

  for (auto field : at.get_fields())
    fieldsJson.append(field);

  accessTypeJson["fields"] = fieldsJson;
  accessTypeJson["type"] = TypeMatcher::compute_hash(at.get_di_type());
  accessTypeJson["type_string"] = to_string(at.get_di_type());

  return accessTypeJson;
}

Json::Value to_json(const AccessType &at, bool verbose) {
  Json::Value accessTypeJson;

  if (at.has_parent()) {
    AccessType p(at.p_type, at.get_p_di_type());
    // AccessType p;
    // p.setType(p_type);
    p.set_kind(at.p_access);
    for (auto f : at.p_fields)
      p.addField(f);
    accessTypeJson["parent"] = to_json_parent(p);
  } else
    accessTypeJson["parent"] = 0;

  accessTypeJson["access"] = to_string(at.get_kind());

  Json::Value fieldsJson(Json::arrayValue);

  for (auto field : at.fields)
    fieldsJson.append(field);

  accessTypeJson["fields"] = fieldsJson;
  accessTypeJson["llvm_type"] = to_string(at.get_llvm_type());
  accessTypeJson["type"] = TypeMatcher::compute_hash(at.get_di_type());
  accessTypeJson["type_string"] = to_string(at.get_di_type());

  if (verbose)
    accessTypeJson["debug"] = dumpICFGNodesJson();

  return accessTypeJson;
}
std::string to_string(const Path &p) {
  std::string str;
  raw_string_ostream rawstr(str);

  rawstr << "<" << to_string(p.access_type) << ", ";
  rawstr << p.node->toString() << ">";

  return rawstr.str();
}

Json::Value to_json(const AccessTypeSet &ats, bool verbose) {
  Json::Value result(Json::arrayValue);

  for (auto at : ats)
    result.append(to_json(at, verbose));

  return result;
}

std::string to_string(const AccessTypeSet &ats, bool verbose) {
  std::stringstream sstream;

  for (auto at : ats)
    sstream << to_string(at, verbose) << std::endl;

  return sstream.str();
}

Json::Value to_json(const ValueMetadata &v, bool verbose) {

  Json::Value res;

  res["access_type_set"] = to_json(v.get_access_type_set(), verbose);
  res["is_array"] = v.is_array;
  res["is_malloc_size"] = v.is_malloc_size;
  res["is_file_path"] = v.is_file_path;
  res["len_depends_on"] = v.len_depends_on;

  Json::Value setByJson(Json::arrayValue);

  for (auto d : v.set_by)
    setByJson.append(d);

  res["set_by"] = setByJson;

  return res;
}

std::string print_summary(const ValueMetadata &v, bool verbose) {
  std::stringstream sstream;

  sstream << "ATS " << v.ats.size() << "\n";
  if (verbose) {
    int i = 1;
    for (auto &at : v.ats) {
      sstream << i++ << ". " << to_string(at, false) << "\n";
    }
  }
  sstream << "array " << std::to_string(v.is_array) << ", ";
  sstream << "malloc " << std::to_string(v.is_malloc_size) << ", ";
  sstream << "path " << std::to_string(v.is_file_path) << ", ";
  sstream << "depends '" << v.len_depends_on << "'\n";

  return sstream.str();
}

std::string to_string(const ValueMetadata &v, bool verbose) {

  std::stringstream sstream;

  sstream << "is_array: " << std::to_string(v.is_array) << "\n";
  sstream << "is_malloc_size: " << std::to_string(v.is_malloc_size) << "\n";
  sstream << "is_file_path: " << std::to_string(v.is_file_path) << "\n";
  sstream << "len_depends_on: " << v.len_depends_on << "\n";

  std::string set_by_str = "";
  for (auto d : v.set_by)
    set_by_str += d + " ";

  sstream << "set_by: " << set_by_str << "\n";
  sstream << "access_type_set:\n" << to_string(v.ats, verbose) << "\n";

  return sstream.str();
}
} // namespace liberator
