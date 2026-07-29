#include "AccessTypeIO.h"
#include "AccessType.h"
#include "DebugInfoParser.hpp" // peel_di_qualifiers
#include "TypeMatcher.h"
#include "ValueMetadata.hpp"
#include <llvm/IR/DebugInfoMetadata.h>

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
  rawstr << ", " << to_string(at.get_llvm_type());
  rawstr << ", " << TypeMatcher::compute_hash(at.get_llvm_type()) << ")";

  return rawstr.str();
}

std::string to_string(const llvm::Type *typ) {
  std::string str;
  llvm::raw_string_ostream(str) << *typ;
  return str;
}

namespace {

/**
 * @return returns for example "%struct.vpx_image". Anonymous composites get the
 * "anon" name.
 */
std::string composite_name(const llvm::DICompositeType *comp) {
  std::string prefix;
  switch (comp->getTag()) {
  case llvm::dwarf::DW_TAG_union_type:
    prefix = "%union.";
    break;
  case llvm::dwarf::DW_TAG_class_type:
    prefix = "%class.";
    break;
  default:
    prefix = "%struct.";
    break;
  }
  auto name = comp->getName();
  return prefix + (name.empty() ? "anon" : name.str());
}

// Guards against cycles in malformed debug info. Struct bodies are only
// expanded at the top level and pointers never expand their pointee, so a
// well-formed type graph nests far below this.
constexpr unsigned MAX_DI_PRINT_DEPTH = 16;

std::string print_di_type(const llvm::DIType *di, bool expand_composite,
                          unsigned depth);

/**
 * Print function type in format:
 * "i32 (%struct.foo*, i8**)".
 */
std::string print_subroutine(const llvm::DISubroutineType *sr, unsigned depth) {
  // 1. Get array types in [ret, param1, ..., param_n]
  auto types = sr->getTypeArray();

  // 2. Get the return type
  std::string out;
  if (types.size() == 0)
    out = "void";
  else
    out = print_di_type(types[0], false, depth + 1);

  // 3. Get the parameters
  out += " (";
  for (unsigned i = 1; i < types.size(); ++i) {
    if (i > 1)
      out += ", ";
    // A trailing null entry in the type array marks a varargs signature.
    if (types[i] == 0 && i + 1 == types.size())
      out += "...";
    else
      out += print_di_type(types[i], false, depth + 1);
  }
  out += ")";
  return out;
}

/**
 * DWARF array like int[3][3] is converted to "[3 x [3 x i32]]"
 */
std::string print_array(const llvm::DICompositeType *comp, unsigned depth) {
  std::string element = print_di_type(comp->getBaseType(), false, depth + 1);

  std::vector<uint64_t> counts;
  for (auto *e : comp->getElements()) {
    // an array like int[3][3] has two subranges
    auto *sub = llvm::dyn_cast_or_null<llvm::DISubrange>(e);
    if (!sub)
      continue;
    // Non-constant bounds (VLAs, flexible array members) have no count; LLVM
    // spells those "[0 x T]".
    auto *ci = llvm::dyn_cast_if_present<llvm::ConstantInt *>(sub->getCount());
    counts.push_back(ci ? ci->getZExtValue() : 0);
  }

  if (counts.empty())
    counts.push_back(0);

  // Innermost dimension first, so wrap back to front.
  std::string out = element;
  for (auto it = counts.rbegin(); it != counts.rend(); ++it)
    out = "[" + std::to_string(*it) + " x " + out + "]";

  return out;
}

/**
 * Converts a DICompositeType such as union/class/struct to a output like
 * "%struct.A = type { i32, i8* }". Nested composites appear by name only,
 * exactly as LLVM prints them.
 */
std::string print_composite_body(const llvm::DICompositeType *comp,
                                 unsigned depth) {
  std::string out = composite_name(comp) + " = type { ";

  bool first = true;
  for (auto *e : comp->getElements()) {
    auto *member = llvm::dyn_cast_or_null<llvm::DIDerivedType>(e);
    // Skip anything that is not a data member (methods, inheritance, ...).
    if (!member || member->getTag() != llvm::dwarf::DW_TAG_member)
      continue;
    if (!first)
      out += ", ";
    first = false;
    out += print_di_type(member->getBaseType(), false, depth + 1);
  }

  out += first ? "}" : " }";
  return out;
}

/**
 * Converts DIBasicType to string.
 */
std::string to_string(const llvm::DIBasicType *b) {
  using namespace llvm::dwarf;
  auto bits = b->getSizeInBits();
  if (bits == 0)
    return "void";

  if (b->getEncoding() == DW_ATE_float) {
    switch (bits) {
    case 16:
      return "half";
    case 32:
      return "float";
    case 64:
      return "double";
    case 80:
      return "x86_fp80";
    case 128:
      return "fp128";
    default:
      break;
    }
  }

  if (b->getEncoding() == DW_ATE_boolean)
    return "i1";

  return "i" + std::to_string(bits);
}

/**
 * @param di the DWARF type to print.
 * @param expand_composite print also "= type { ... }" for structs but only
 * if struct is passed by value.
 * @param depth recursion guard, see MAX_DI_PRINT_DEPTH.
 */
std::string print_di_type(const llvm::DIType *di, bool expand_composite,
                          unsigned depth) {
  using namespace llvm::dwarf;

  if (!di)
    return "void";
  if (depth > MAX_DI_PRINT_DEPTH)
    return "...";

  di = peel_di_qualifiers(const_cast<llvm::DIType *>(di));
  if (!di)
    return "void";

  // Output basic types like i32, i8, float, half, double
  if (auto *b = llvm::dyn_cast<llvm::DIBasicType>(di))
    return to_string(b);

  // Output Subroutine types like function pointers
  if (auto *sr = llvm::dyn_cast<llvm::DISubroutineType>(di))
    return print_subroutine(sr, depth);

  if (auto *d = llvm::dyn_cast<llvm::DIDerivedType>(di)) {
    switch (d->getTag()) {
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
    case DW_TAG_ptr_to_member_type: {
      // void* is spelled "i8*" like in typed-pointer IR.
      std::string pointee = print_di_type(d->getBaseType(), false, depth + 1);
      if (pointee == "void")
        pointee = "i8";
      return pointee + "*";
    }
    case DW_TAG_member:
    case DW_TAG_inheritance:
      return print_di_type(d->getBaseType(), expand_composite, depth + 1);
    default:
      break;
    }
  }

  if (auto *comp = llvm::dyn_cast<llvm::DICompositeType>(di)) {
    switch (comp->getTag()) {
    case DW_TAG_array_type:
      return print_array(comp, depth);
    case DW_TAG_enumeration_type: {
      auto bits = comp->getSizeInBits();
      return "i" + std::to_string(bits ? bits : 32);
    }
    case DW_TAG_structure_type:
    case DW_TAG_class_type:
    case DW_TAG_union_type:
      return expand_composite ? print_composite_body(comp, depth)
                              : composite_name(comp);
    default:
      break;
    }
  }

  return di->getName().empty() ? "<unknown>" : di->getName().str();
}

} // namespace

/**
 * Prints a DWARF type in the old (pre-opaque-pointer) LLVM IR type syntax:
 *   "i32", "i8*", "i8**"
 *   "%struct.vpx_image*"                       (pointer to a struct)
 *   "%struct.vpx_codec_cx_pkt = type { i32, %union.anon }"  (by value)
 *   "[4 x i8*]"                                (array)
 *   "%struct.vpx_image* (%struct.vpx_codec_alg_priv*, i8**)*"
 * The DWARF type graph is walked directly instead of being lowered through
 * resolve_di_type_to_llvm, whose llvm::TypedPointerType prints as
 * "typedptr(i8, 0)" and whose struct lookup fails for types that are not
 * identified in the module.
 */
std::string to_string(const llvm::DIType *di) {
  return print_di_type(di, /*expand_composite=*/true, /*depth=*/0);
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
  rawstr << ", " << TypeMatcher::compute_hash(at.get_llvm_type()) << ")";

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
  accessTypeJson["type"] = TypeMatcher::compute_hash(at.get_llvm_type());
  accessTypeJson["type_string"] = to_string(at.get_llvm_type());

  return accessTypeJson;
}

Json::Value to_json(const AccessType &at, bool verbose) {
  Json::Value accessTypeJson;

  if (at.has_parent()) {
    AccessType p(at.p_type);
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
  accessTypeJson["type"] = TypeMatcher::compute_hash(at.get_llvm_type());
  accessTypeJson["type_string"] = to_string(at.get_llvm_type());

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
