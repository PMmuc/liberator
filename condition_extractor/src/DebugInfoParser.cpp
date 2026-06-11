#include "DebugInfoParser.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>
#include <unordered_map>

using namespace llvm;
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
    if (!md) continue;
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
