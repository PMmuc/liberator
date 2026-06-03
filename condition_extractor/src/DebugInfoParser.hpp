#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <unordered_map>

class struct_padding_info_t final {
public:
  using byte_range_t = std::pair<unsigned, unsigned>;
  struct_padding_info_t() = default;
  struct_padding_info_t(llvm::ArrayRef<byte_range_t> &ranges);
  struct_padding_info_t(const llvm::DICompositeType *type);

  std::string to_string() const;

  llvm::ArrayRef<byte_range_t> get_padding_ranges() const {
    return padding_byte_ranges_;
  }

private:
  llvm::SmallVector<byte_range_t, 4> padding_byte_ranges_;
};

class debug_info_parser_t {
public:
  static std::unordered_map<llvm::StructType *, struct_padding_info_t>
  get_struct_info_padding(const llvm::Module &m);

private:
  static void insert_padding_info(
      llvm::DICompositeType *type,
      std::unordered_map<llvm::StructType *, struct_padding_info_t> &res,
      llvm::SmallDenseSet<llvm::DICompositeType *> &visited,
      llvm::LLVMContext &ctx, const llvm::DataLayout &dl,
      llvm::StructType *struct_type = nullptr);
};
