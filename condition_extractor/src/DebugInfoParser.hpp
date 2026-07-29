#pragma once

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <unordered_map>

namespace liberator {
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

llvm::Type *infer_type_from_arg_attrs(const llvm::Argument *arg);
llvm::DIType *peel_di_qualifiers(llvm::DIType *t);
llvm::Type *resolve_di_type_to_llvm(llvm::DIType *di, llvm::Module &mod);
llvm::Type *infer_type_from_forward_uses(const llvm::Value *param);

/**
 * Returns the llvm::Type of the parameter.
 */
const llvm::Type *restore_llvm_type(const llvm::Value *param);

/**
 *  Returns the type of a value from DWARF type information.
 *  @param param to retrieve the type.
 *  @return the DIType to restore.
 */
llvm::DIType *restore_param_di_type(const llvm::Value *param);

/**
 * This will remove pointers, constness and volatile modifiers from the
 * specified type.
 * @param type
 * @return the modified DIType without pointers and references.
 */
llvm::DIType *decay_di_type(llvm::DIType *type);

/**
 * Compare DIType to llvm type
 * First we have to canoncialize the llvm type name to a name that can be
 * compared to the actual name.
 * @return true if types match, false otherwise
 */
bool compare_types(llvm::DIType *di, const llvm::Type *type,
                   const llvm::DataLayout &dl);

/**
 * Returns the next DI type that we need to handle based on the offset that
 * compare_types returned from a GEP instruction.
 * @param di current DIType
 * @param container
 */
llvm::DIType *next_di_field(llvm::DIType *di, const llvm::Type *container,
                            uint64_t idx, const llvm::Type *result,
                            const llvm::DataLayout &dl);

} // namespace liberator
