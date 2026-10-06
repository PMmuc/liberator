
#include "TypeMatcher.h"
#include "DebugInfoParser.hpp"
#include "md5/md5.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/TypedPointerType.h>
#include <llvm/Support/Casting.h>

TypeMatcher::TypeStringMap TypeMatcher::type_hash_map;
TypeMatcher::TypeStringMap TypeMatcher::type_id_map;
TypeMatcher::DITypeStringMap TypeMatcher::di_type_hash_map;

std::string TypeMatcher::compute_id(const llvm::StructType *t) {

  if (type_id_map.find(t) != type_id_map.end())
    return type_id_map[t];

  std::string name;
  if (t->hasName()) {
    name = remove_trail_num(t->getName().str());
  } else {
    name = "none";
  }

  type_id_map[t] = name;

  return name;
}

/**
 * -
 */
std::string di_struct_id(const llvm::DICompositeType *comp,
                         StringRef typedef_name) {
  return liberator::unique_composite_name(comp, typedef_name);
}

/**
 * -
 */
bool has_data_members(const llvm::DICompositeType *comp) {
  for (auto *e : comp->getElements()) {
    auto m = dyn_cast_or_null<llvm::DIDerivedType>(e);
    if (m && !m->isStaticMember() && m->getTag() == llvm::dwarf::DW_TAG_member)
      return true;
  }

  return false;
}

std::string di_subroutine_string(const llvm::DISubroutineType *sr,
                                 unsigned depth);

/**
 * -
 * Returns a string representation of the struct and if it is a pointer to a
 * struct the depth.
 */
std::string di_array_string(const llvm::DICompositeType *comp, unsigned depth);

/**
 * -
 */
string unique_string(const llvm::DIType *type, unsigned depth) {
  llvm::StringRef typedef_name;
  type = liberator::peel_di_type(type, typedef_name);

  if (!type)
    return "VO";

  if (auto *b = llvm::dyn_cast<llvm::DIBasicType>(type)) {
    const uint64_t bits = b->getSizeInBits();
    if (bits == 0)
      return "VO";
    switch (b->getEncoding()) {
    case dwarf::DW_ATE_float:
      if (b->getName() == "long double")
        return "F8";
      switch (bits) {
      case 16:
        return "HA";
      case 32:
        return "FL";
      case 64:
        return "DO";
      case 80:
        return "F8";
      case 128:
        return "FP";
      default:
        return "UN";
      }
    case dwarf::DW_ATE_complex_float:
      return "ST[none]";
    default:
      return "IN";
    }
  }

  if (auto sr = dyn_cast<DISubroutineType>(type))
    return di_subroutine_string(sr, depth);

  if (auto *d = llvm::dyn_cast<DIDerivedType>(type)) {
    switch (d->getTag()) {
    case dwarf::DW_TAG_pointer_type:
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type: {
      string pointee = unique_string(d->getBaseType(), depth + 1);
      if (pointee == "VO")
        pointee = "IN";
      return "TP[" + pointee + "]";
    }
    case dwarf::DW_TAG_member:
      return unique_string(d->getBaseType(), depth + 1);
    default:
      break;
    }
  }

  if (auto comp = dyn_cast<DICompositeType>(type)) {
    switch (comp->getTag()) {
    case dwarf::DW_TAG_array_type:
      return di_array_string(comp, depth);
    case dwarf::DW_TAG_enumeration_type:
      return "IN";
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
    case dwarf::DW_TAG_union_type:
      if (!comp->isForwardDecl() && !has_data_members(comp))
        return "FN";
      return "ST[" + di_struct_id(comp, typedef_name) + "]";
    default:
      break;
    }
  }
  return "UN";
}

std::string di_subroutine_string(const llvm::DISubroutineType *sr,
                                 unsigned depth) {
  auto types = sr->getTypeArray();

  std::string ret =
      types.size() == 0 ? "VO" : unique_string(types[0], depth + 1);

  std::vector<string> params;
  for (unsigned i = 1; i < types.size(); ++i) {
    if (types[i])
      params.push_back(unique_string(types[i], depth + 1));
  }

  std::string hash = "FN[" + to_string(params.size() + 1) + "," + ret;
  for (const string &p : params)
    hash += "," + p;
  return hash + "]";
}

/**
 * -
 */
std::string di_array_string(const llvm::DICompositeType *comp, unsigned depth) {
  string hash = unique_string(comp->getBaseType(), depth + 1);

  vector<uint64_t> counts;
  for (auto e : comp->getElements()) {
    auto *sub = llvm::dyn_cast_or_null<llvm::DISubrange>(e);
    if (!sub)
      continue;
    auto ci = llvm::dyn_cast_if_present<ConstantInt *>(sub->getCount());
    counts.push_back(ci ? ci->getZExtValue() : 0);
  }
  if (counts.empty())
    counts.push_back(0);

  const std::string tag = comp->isVector() ? "VF[" : "AR[";
  for (auto it = counts.rbegin(); it != counts.rend(); ++it) {
    hash = tag + std::to_string(*it) + "," + hash + "]";
  }
  return hash;
}

std::string TypeMatcher::compute_hash(const llvm::DIType *t) {
  string hash = TypeMatcher::compute_unique_string(t);
  md5::MD5 md5stream;
  md5stream.add(hash.c_str(), hash.length());
  hash = md5stream.getHash();

  return hash;
}

std::string TypeMatcher::compute_unique_string(const llvm::DIType *t) {
  auto it = di_type_hash_map.find(t);
  if (it != di_type_hash_map.end())
    return it->second;

  std::string hash = unique_string(t, 0);
  di_type_hash_map[t] = hash;
  return hash;
}

std::string TypeMatcher::compute_unique_string(const llvm::Type *t,
                                               std::set<std::string> ids_done) {

  if (type_hash_map.find(t) != type_hash_map.end())
    return type_hash_map[t];

  std::string hash = "";

  switch (t->getTypeID()) {
  case llvm::Type::TypeID::HalfTyID:
    hash += "HA";
    break;
  case llvm::Type::TypeID::BFloatTyID:
    hash += "BF";
    break;
  case llvm::Type::TypeID::FloatTyID:
    hash += "FL";
    break;
  case llvm::Type::TypeID::DoubleTyID:
    hash += "DO";
    break;
  case llvm::Type::TypeID::X86_FP80TyID:
    hash += "F8";
    break;
  case llvm::Type::TypeID::FP128TyID:
    hash += "FP";
    break;
  case llvm::Type::TypeID::PPC_FP128TyID:
    hash += "PP";
    break;
  case llvm::Type::TypeID::VoidTyID:
    hash += "VO";
    break;
  case llvm::Type::TypeID::LabelTyID:
    hash += "LA";
    break;
  case llvm::Type::TypeID::MetadataTyID:
    hash += "ME";
    break;
  case llvm::Type::TypeID::X86_AMXTyID:
    hash += "AM";
    break;
  case llvm::Type::TypeID::TokenTyID:
    hash += "TO";
    break;
  case llvm::Type::TypeID::IntegerTyID:
    hash += "IN";
    break;
  case llvm::Type::TypeID::FunctionTyID: {
    hash += "FN["; // compute hash function

    const llvm::FunctionType *ft = SVFUtil::dyn_cast<FunctionType>(t);

    // std::string t_id = compute_id(st);

    auto ret_type = ft->getReturnType();
    unsigned int n_arg = ft->getNumParams();
    hash += std::to_string(n_arg + 1) + ",";

    hash += compute_unique_string(ret_type, ids_done) + ",";
    unsigned int i = 0;
    for (; i < n_arg; i++) {
      auto at = ft->getParamType(i);
      hash += compute_unique_string(at, ids_done) + ",";
    }

    hash = hash.substr(0, hash.size() - 1); // remove last ","
    hash += "]";
  } break;
  case llvm::Type::TypeID::PointerTyID: {
    const llvm::PointerType *pt = SVFUtil::dyn_cast<PointerType>(t);
    // FIXME: here we would add compute_unique_string(pt->getElementType(),
    // ids_done) + "]";
    hash += "PN";
  } break;
  case llvm::Type::TypeID::TypedPointerTyID: {
    // Synthesized by the DWARF fallback to preserve source-level pointer
    // typing in opaque-pointer LLVM. Unlike opaque PointerType, it carries
    // the pointee, so include it so e.g. `i32*` and `i32**` hash distinctly.
    const auto *tp = SVFUtil::dyn_cast<llvm::TypedPointerType>(t);
    hash += "TP[" + compute_unique_string(tp->getElementType(), ids_done) + "]";
  } break;
  case llvm::Type::TypeID::StructTyID: {
    const llvm::StructType *st = SVFUtil::dyn_cast<StructType>(t);

    std::string t_id = compute_id(st);

    if (st->isEmptyTy()) {
      hash += "FN";
    } else {
      hash += "ST[" + t_id + "]";
      // hash += "ST[" + t_id + "," +
      //         std::to_string(st->getNumElements()) + ",";

      // bool to_expand = ids_done.find(t_id) == ids_done.end();
      // ids_done.insert(t_id);
      // for (auto el: st->elements())
      //     if (to_expand)
      //         hash += compute_unique_string(el, ids_done) + ",";
      //     else
      //         hash += "x,";

      // hash = hash.substr(0, hash.size()-1); // remove last ","
      // hash += "]";
    }
  } break;
  case llvm::Type::TypeID::ArrayTyID: {
    const llvm::ArrayType *ar = SVFUtil::dyn_cast<ArrayType>(t);
    hash += "AR[" + std::to_string(ar->getNumElements());
    hash += "," + compute_unique_string(ar->getElementType(), ids_done);
    hash += "]";
  } break;
  case llvm::Type::TypeID::FixedVectorTyID: {
    const llvm::FixedVectorType *fv = SVFUtil::dyn_cast<FixedVectorType>(t);
    hash += "FV[" + std::to_string(fv->getNumElements());
    hash += "," + compute_unique_string(fv->getElementType(), ids_done);
    hash += "]";
  } break;
  case llvm::Type::TypeID::ScalableVectorTyID: {
    const llvm::ScalableVectorType *fv =
        SVFUtil::dyn_cast<ScalableVectorType>(t);
    hash += "SV[" + std::to_string(fv->getMinNumElements());
    hash += "," + compute_unique_string(fv->getElementType(), ids_done);
    hash += "]";
  } break;
  default:
    outs() << "[ERROR] Invalid Type!\n";
    exit(1);
  }

  type_hash_map[t] = hash;

  return hash;
}

/**
 * Compute MD5 hash of the llvm::Type
 */
std::string TypeMatcher::compute_hash(const llvm::Type *t) {
  std::string hash = TypeMatcher::compute_unique_string(t);

  md5::MD5 md5stream;
  md5stream.add(hash.c_str(), hash.length());
  hash = md5stream.getHash();

  return hash;
}

bool TypeMatcher::compare_types(const llvm::Type *t1, const llvm::Type *t2) {
  return compute_hash(t1) == compute_hash(t2);
}

std::string TypeMatcher::remove_trail_num(std::string n) {

  // std::string to_ret = "";

  char trail_chrs[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.'};

  int i = n.size() - 1;

  bool has_bad_char = true;
  while (has_bad_char) {
    has_bad_char = false;
    for (int c = 0; c < sizeof(trail_chrs); c++)
      if (n[i] == trail_chrs[c]) {
        has_bad_char = true;
        break;
      }
    i--;
    if (i < 0)
      break;
  }

  return std::string(&n[0], &n[i + 2]);
}
