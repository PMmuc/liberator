#include "DebugInfoParser.hpp"
#include "Config.h"
#include "FileLogger.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/TypedPointerType.h>
#include <unordered_map>

using namespace llvm;

namespace liberator {
struct_padding_info_t::struct_padding_info_t(
    llvm::ArrayRef<byte_range_t> &ranges)
    : padding_byte_ranges_(ranges) {}

struct_padding_info_t::struct_padding_info_t(
    const llvm::DICompositeType *type) {
  auto size = type->getSizeInBits() / 8;

  struct field_info_t {
    unsigned offset;
    unsigned size;
  };

  SmallVector<field_info_t, 8> field_info;

  for (const Metadata *md : type->getElements()) {
    if (!md)
      continue;
    const auto *derived = dyn_cast<DIDerivedType>(md);
    if (!derived || derived->getTag() != dwarf::DW_TAG_member)
      continue;
    unsigned offset = derived->getOffsetInBits() / 8;
    unsigned size = derived->getSizeInBits() / 8;
    field_info.emplace_back(offset, size);
  }

  if (field_info.empty())
    return;

  sort(field_info, [](const field_info_t &f1, const field_info_t &f2) -> bool {
    return f1.offset < f2.offset;
  });

  unsigned curr_offset = 0;
  for (const auto &field : field_info) {
    if (field.offset > curr_offset)
      padding_byte_ranges_.emplace_back(curr_offset, field.offset);
    curr_offset = field.offset + field.size;
  }
  if (curr_offset < size)
    padding_byte_ranges_.emplace_back(curr_offset, size);
}

std::string struct_padding_info_t::to_string() const { return ""; }

std::unordered_map<llvm::StructType *, struct_padding_info_t>
debug_info_parser_t::get_struct_info_padding(const llvm::Module &M) {
  std::unordered_map<llvm::StructType *, struct_padding_info_t> res;
  SmallVector<DICompositeType *, 32> di_composite_type;

  DebugInfoFinder finder;

  finder.processModule(M);

  for (auto scope : finder.types()) {
    if (auto *composite_type = dyn_cast<DICompositeType>(scope)) {
      di_composite_type.push_back(composite_type);
    }
  }

  SmallDenseSet<DICompositeType *> visited;
  LLVMContext &context = M.getContext();
  const DataLayout &layout = M.getDataLayout();
  for (auto composite : di_composite_type) {
    insert_padding_info(composite, res, visited, context, layout);
  }

  return res;
}
void debug_info_parser_t::insert_padding_info(
    llvm::DICompositeType *type,
    std::unordered_map<llvm::StructType *, struct_padding_info_t>
        &padding_info_map,
    llvm::SmallDenseSet<llvm::DICompositeType *> &visited,
    llvm::LLVMContext &ctx, const llvm::DataLayout &dl,
    llvm::StructType *struct_type) {
  if (!visited.insert(type).second)
    return;

  if (!struct_type) {
    StringRef name = type->getName();
    if (!name.empty()) {
      std::string prefix;
      switch (type->getTag()) {
      case dwarf::DW_TAG_class_type:
      case dwarf::DW_TAG_structure_type:
      case dwarf::DW_TAG_union_type:
      case dwarf::DW_TAG_enumeration_type:
      default:
        llvm_unreachable("Not known DWARF type");
      }
      struct_type = StructType::getTypeByName(ctx, name);
    }
  }

  if (!struct_type)
    return;

  struct_padding_info_t struct_padding_info(type);
  padding_info_map.insert({struct_type, struct_padding_info});
  auto padding_ranges = struct_padding_info.get_padding_ranges();
  const StructLayout *struct_layout = dl.getStructLayout(struct_type);
  unsigned field_idx = 0;
  auto is_padding = [&padding_ranges,
                     &struct_layout](unsigned field_idx) -> bool {
    return std::ranges::any_of(
        padding_ranges,
        [&struct_layout,
         field_idx](const struct_padding_info_t::byte_range_t &range) -> bool {
          return struct_layout->getElementOffset(field_idx) == range.first;
        });
  };

  for (DINode *node : type->getElements()) {
    if (!node || (!isa<DICompositeType>(node) && !isa<DIDerivedType>(node)))
      continue;
    StructType *child_struct_type = nullptr;
    while (is_padding(field_idx))
      field_idx++;

    if (struct_type)
      child_struct_type =
          dyn_cast<StructType>(struct_type->getElementType(field_idx));

    if (auto *nested_di_composite_type = dyn_cast<DICompositeType>(node)) {
      insert_padding_info(nested_di_composite_type, padding_info_map, visited,
                          ctx, dl, child_struct_type);
    }
    if (auto *derived_di_type = dyn_cast<DIDerivedType>(node)) {
      if (auto *base_type = derived_di_type->getBaseType()) {
        if (auto *composite_di_type = dyn_cast<DICompositeType>(base_type))
          insert_padding_info(composite_di_type, padding_info_map, visited, ctx,
                              dl, child_struct_type);
      }
      field_idx++;
    }
  }
}

llvm::Type *infer_type_from_arg_attrs(const llvm::Argument *arg) {
  if (!arg->getType()->isPointerTy())
    return nullptr;
  // When the struct type is passed by value but is to big
  if (auto *T = arg->getParamByValType())
    return T;
  return nullptr;
}

llvm::DIType *peel_di_qualifiers(llvm::DIType *t) {
  using namespace llvm::dwarf;
  if (!t) {
    TYPE_LOG("peel_di_qualifiers: null input\n");
    return nullptr;
  }
  std::string s;
  raw_string_ostream os(s);
  t->printTree(os);
  TYPE_LOG("{}", s);
  while (auto *d = llvm::dyn_cast_or_null<llvm::DIDerivedType>(t)) {
    auto tag = d->getTag();
    if (tag != DW_TAG_typedef && tag != DW_TAG_const_type &&
        tag != DW_TAG_volatile_type && tag != DW_TAG_restrict_type &&
        tag != DW_TAG_atomic_type)
      break;
    t = d->getBaseType();
  }
  return t;
}

llvm::Type *resolve_di_type_to_llvm(llvm::DIType *di, llvm::Module &mod) {
  using namespace llvm::dwarf;
  auto &ctx = mod.getContext();

  if (!di)
    return llvm::Type::getVoidTy(ctx);

  std::string s;
  raw_string_ostream os(s);
  di->printTree(os);
  TYPE_LOG("{}", s);
  di = peel_di_qualifiers(di);
  if (!di)
    return llvm::Type::getVoidTy(ctx);

  // function pointers
  if (auto *sr = llvm::dyn_cast<llvm::DISubroutineType>(di)) {
    TYPE_LOG("Found a function pointer\n");
    auto types = sr->getTypeArray();
    if (types.size() == 0)
      return nullptr;
    // resolve return type
    llvm::Type *ret = resolve_di_type_to_llvm(types[0], mod);
    if (!ret)
      ret = llvm::Type::getVoidTy(ctx);
    // resolve param types
    std::vector<llvm::Type *> params;
    for (unsigned i = 1; i < types.size(); ++i) {
      llvm::Type *p = resolve_di_type_to_llvm(types[i], mod);
      if (!p)
        return nullptr;
      params.push_back(p);
    }
    // create the function type signature
    return llvm::FunctionType::get(ret, params, /*isVarArg=*/false);
  }

  // TODO: maybe use something better then linear search for finding the correct
  // struct
  // for struct types and union and classes
  if (auto *comp = llvm::dyn_cast<llvm::DICompositeType>(di)) {
    auto tag = comp->getTag();
    if (tag == DW_TAG_structure_type || tag == DW_TAG_class_type ||
        tag == DW_TAG_union_type) {
      auto name = comp->getName().str();
      if (name.empty())
        return nullptr;
      // iterate each struct type defined in the module.
      for (auto *ST : mod.getIdentifiedStructTypes()) {
        if (ST->getName() == name || ST->getName() == "struct." + name ||
            ST->getName() == "union." + name ||
            ST->getName() == "class." + name)
          return ST;
      }
      return nullptr;
    }
  }

  if (auto *d = llvm::dyn_cast<llvm::DIDerivedType>(di)) {
    auto tag = d->getTag();
    if (tag == DW_TAG_pointer_type || tag == DW_TAG_reference_type ||
        tag == DW_TAG_rvalue_reference_type) {
      llvm::Type *pointee = resolve_di_type_to_llvm(d->getBaseType(), mod);
      // void* (DW_TAG_pointer_type with null base) and unresolvable pointees
      // both become i8* — matches the old C-style convention and keeps the
      // pointer wrap intact rather than degenerating to plain i8.
      if (!pointee || pointee->isVoidTy())
        pointee = llvm::Type::getInt8Ty(ctx);
      return llvm::TypedPointerType::get(pointee, 0);
    }
  }

  // get the correct basic type (int, float, short, double, long, char)
  if (auto *b = llvm::dyn_cast<llvm::DIBasicType>(di)) {
    auto bits = b->getSizeInBits();
    if (bits == 0)
      return llvm::Type::getVoidTy(ctx);
    switch (b->getEncoding()) {
    case DW_ATE_boolean:
      return llvm::Type::getInt1Ty(ctx);
    case DW_ATE_signed:
    case DW_ATE_unsigned:
    case DW_ATE_signed_char:
    case DW_ATE_unsigned_char:
    case DW_ATE_UTF:
      return llvm::IntegerType::get(ctx, bits);
    case DW_ATE_float:
      if (bits == 16)
        return llvm::Type::getHalfTy(ctx);
      if (bits == 32)
        return llvm::Type::getFloatTy(ctx);
      if (bits == 64)
        return llvm::Type::getDoubleTy(ctx);
      if (bits == 80)
        return llvm::Type::getX86_FP80Ty(ctx);
      if (bits == 128)
        return llvm::Type::getFP128Ty(ctx);
      return nullptr;
    default:
      return nullptr;
    }
  }

  return nullptr;
}

llvm::Type *infer_type_from_forward_uses(const llvm::Value *param) {
  llvm::Type *best_struct = nullptr;
  llvm::Type *fallback = nullptr;
  for (const llvm::User *U : param->users()) {
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(U)) {
      if (gep->getPointerOperand() != param)
        continue;
      llvm::Type *src = gep->getSourceElementType();
      std::string str;
      raw_string_ostream os(str);
      src->print(os);
      TYPE_LOG("Found source element type: {}\n", str);
      if (src && src->isStructTy()) {
        if (!best_struct)
          best_struct = src;
      } else if (!fallback) {
        fallback = src;
      }
    } else if (auto *ld = llvm::dyn_cast<llvm::LoadInst>(U)) {
      if (ld->getPointerOperand() == param && !fallback)
        fallback = ld->getType();
    } else if (auto *st = llvm::dyn_cast<llvm::StoreInst>(U)) {
      if (st->getPointerOperand() == param && !fallback)
        fallback = st->getValueOperand()->getType();
    }
  }
  return best_struct ? best_struct : fallback;
}
} // namespace liberator
