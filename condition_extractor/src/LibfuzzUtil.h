//===- HexTypeUtil.h - helper functions and classes for HexType ----*-
// C++-*-===//
////
////                     The LLVM Compiler Infrastructure
////
//// This file is distributed under the University of Illinois Open Source
//// License. See LICENSE.TXT for details.
////
////===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_HEXTYPE_H
#define LLVM_TRANSFORMS_UTILS_HEXTYPE_H

#include "llvm/IR/Argument.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include <format>
#include <llvm/IR/DebugInfoMetadata.h>
#include <string>
#include <vector>

using namespace llvm;

std::string remove_quotes(std::string s);

namespace liberator {
std::string pointer_flag(const llvm::DIType *di);
llvm::DIType *restore_param_di_type(const llvm::Value *param);
std::string typed_pointer_string(llvm::Type *type, const llvm::DIType *di);

struct argument_record {
public:
  std::string name; // arg name or return
  std::string flag; // val, ref, ret
  uint64_t size;
  std::string type;

  void set_from_type(llvm::Type *arg_type, const llvm::DIType *di = nullptr) {

    if (arg_type->isPtrOrPtrVectorTy()) {
      flag = pointer_flag(di);
    } else {
      flag = "val";
    }

    type = typed_pointer_string(arg_type, di);
  }

  void set_from_argument(const llvm::Argument &arg) {

    this->name = arg.getName().str();

    llvm::Type *a_type = arg.getType();
    const llvm::DIType *di = restore_param_di_type(&arg);

    if (arg.hasPointeeInMemoryValueAttr()) {
      flag = "ret";
      llvm::raw_string_ostream ostream(this->type);
      arg.getPointeeInMemoryValueType()->print(ostream);
      type += "*";
      return;
    }

    if (a_type->isPtrOrPtrVectorTy()) {

      llvm::PointerType *a_ptype = llvm::dyn_cast<llvm::PointerType>(a_type);
      flag = pointer_flag(di);
    } else {
      flag = "val";
    }
    this->type = typed_pointer_string(a_type, di);
  }
  std::string to_json() const;
};

struct function_record {
  std::string function_name;
  argument_record return_info;
  std::string is_vararg;
  std::vector<argument_record> arguments_info;
  std::string to_json();
};

// void dumpCoerceMap(std::string func_name, unsigned arg_pos, std::string
// arg_original, std::string arg_coerce);
void dumpCoerceMap(llvm::Function *func, unsigned arg_pos,
                   std::string arg_original_name, std::string arg_original_type,
                   llvm::Argument *arg_coerce);
void dumpApiInfo(function_record a_fun);

uint64_t estimate_size(const llvm::Argument *arg, const llvm::DataLayout *DL);
uint64_t estimate_size(llvm::Type *type, const llvm::DataLayout *DL);

} // namespace liberator
  //
template <> struct std::formatter<liberator::argument_record> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
  auto format(const liberator::argument_record &arg, std::format_context &ctx) {
    auto out = ctx.out();

    /*str_ret += "{";
    str_ret += "\"name\": \"" + this->name + "\", ";
    str_ret += "\"flag\": \"" + this->flag + "\", ";
    str_ret += "\"size\": " + std::to_string(this->size) + ", ";
    str_ret += "\"type\": \"" + remove_quotes(this->type) + "\"";
    str_ret += "}";*/
    out =
        std::format_to(out,
                       "{{\"name\": \"{}\", \"flag\": \"{}\", \"size\": \"{}\""
                       "\"type\": \"{}\"}}",
                       arg.name, arg.flag, arg.size, remove_quotes(arg.type));
  }
};

template <> struct std::formatter<liberator::function_record> {
  constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
  auto format(const liberator::function_record &fun, std::format_context &ctx) {
    auto out = ctx.out();

    out = std::format_to(
        out,
        "{{\"function_name\": \"{}\", \"return_info\": {}, \"is_vararg\": "
        "\"{}\", \"arguments_info\": [",
        fun.function_name, fun.return_info.to_json(), fun.is_vararg);
  }
};
#endif // LLVM_TRANSFORMS_UTILS_HEXTYPE_H
