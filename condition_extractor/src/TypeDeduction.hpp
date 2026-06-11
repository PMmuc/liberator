#pragma once

#include "llvm/IR/Type.h"

class transparent_type_t {
public:
  enum class kind_e { e_primitive, e_pointer, e_array, e_struct };

  llvm::Type *get_llvm_type() const { return llvm_type_; }

  bool is_pointer_type() const { return get_kind() == kind_e::e_pointer; }
  bool is_struct_type() const { return get_kind() == kind_e::e_struct; }
  bool is_primitive_type() const { return get_kind() == kind_e::e_primitive; }
  bool is_array_type() const { return get_kind() == kind_e::e_array; }

  virtual bool is_opaque_pointer() const { return false; }
  virtual kind_e get_kind() const { return kind_e::e_primitive; }

  bool is_compatible_with(const transparent_type_t *rhs) const;

  std::unique_ptr<transparent_type_t> merge_with(const transparent_type_t *rhs);
  bool is_placeholder() const { return is_primitive_type() && !llvm_type_; }

  virtual std::unique_ptr<transparent_type_t> clone() const;

protected:
  llvm::Type *llvm_type_ = nullptr;
  bool is_a_union_ = false;
};

std::unique_ptr<transparent_type_t> clone() const {}

bool transparent_type_t::is_compatible_with(
    const transparent_type_t *rhs) const {
  if (!rhs)
    return true;

  if (!rhs->is_pointer_type())
    return false;

  if (is_opaque_pointer())

    // primtives are compatible
    return get_kind() == rhs->get_kind();
}

class transparent_pointer_type_t : public transparent_type_t {
public:
  virtual bool is_opaque_pointer() const override { return !pointed_type_; }

  kind_e get_kind() const override { return kind_e::e_pointer; }

  virtual std::unique_ptr<transparent_type_t> clone() const override;

protected:
  std::unique_ptr<transparent_pointer_type_t> pointed_type_;
};

class transparent_array_type_t : public transparent_type_t {
public:
  virtual kind_e get_kind() const override { return kind_e::e_array; }
};

class transparent_struct_type_t : public transparent_type_t {
public:
  virtual kind_e get_kind() const override { return kind_e::e_struct; }
};

std::unique_ptr<transparent_type_t>
transparent_type_t::merge_with(const transparent_type_t *rhs) {
  if (!rhs)
    return rhs->clone();

  is_compatible_with(rhs);
}
