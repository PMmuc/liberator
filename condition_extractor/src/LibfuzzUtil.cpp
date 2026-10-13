//===- HexTypeUtil.cpp - helper functions and classes for HexType ---------===//
////
////                     The LLVM Compiler Infrastructure
////
//// This file is distributed under the University of Illinois Open Source
//// License. See LICENSE.TXT for details.
////
////===--------------------------------------------------------------------===//

#include "LibfuzzUtil.h"
#include "DebugInfoParser.hpp"
#include "FileLogger.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <filesystem>
#include <fstream>
#include <inttypes.h>
#include <ios>
#include <iostream>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>

#define MAXLEN 1000

using namespace llvm;

namespace liberator {
// cl::opt<bool> ClCreateCastRelatedTypeList(
// "create-cast-related-type-list",
// cl::desc("create casting related object list"),
// cl::Hidden, llvm::cl::init(false));

std::string CoerceFilePath = "";
std::string ApiFilePath = "";

std::string getCoerceFileLog() {
  if (CoerceFilePath == "") {
    if (getenv("LIBFUZZ_LOG_PATH")) {
      char buff[1000];
      strcpy(buff, getenv("LIBFUZZ_LOG_PATH"));
      CoerceFilePath = std::string(buff) + "/coerce.log";
    } else {
      errs() << "LIBFUZZ_LOG_PATH not found, set it!\n";
      exit(1);
    }
  }
  return CoerceFilePath;
}

std::string getApiFileLog() {
  if (ApiFilePath == "") {
    if (getenv("LIBFUZZ_LOG_PATH")) {
      ApiFilePath = std::string(getenv("LIBFUZZ_LOG_PATH")) + "/apis_llvm.json";
    } else {
      errs() << "LIBFUZZ_LOG_PATH not found\n";
    }
  }
  return ApiFilePath;
}
void dumpLine(std::string line, std::string fileName) {
  std::ofstream log(fileName, std::ios_base::app | std::ios_base::out);
  log << line;
  log.close();
}

std::string pointer_flag(const llvm::DIType *di) {
  auto *ptr = dyn_cast_or_null<llvm::DIDerivedType>(
      peel_di_qualifiers(const_cast<llvm::DIType *>(di)));
  if (ptr && ptr->getTag() == llvm::dwarf::DW_TAG_pointer_type &&
      llvm::isa_and_nonnull<llvm::DISubroutineType>(
          peel_di_qualifiers(ptr->getBaseType())))
    return "fun";
  return "ref";
}

std::string typed_pointer_string(llvm::Type *type, const llvm::DIType *di) {
  auto ptr = dyn_cast_or_null<llvm::DIDerivedType>(
      peel_di_qualifiers(const_cast<DIType *>(di)));
  if (type->isPointerTy() && ptr &&
      is_indirection_tag(static_cast<llvm::dwarf::Tag>(ptr->getTag())))
    return build_di_key(ptr);

  std::string str;
  llvm::raw_string_ostream ostream(str);
  type->print(ostream);
  return str;
}

void dumpCoerceMap(llvm::Function *func, unsigned arg_pos,
                   std::string arg_original_name, std::string arg_original_type,
                   llvm::Argument *arg_coerce) {
  std::string fileName = getCoerceFileLog();

  const llvm::DataLayout *DL = &func->getParent()->getDataLayout();

  llvm::Type *arg_coerce_type = arg_coerce->getType();

  std::string arg_coerce_name = arg_coerce->getName().str();
  uint64_t arg_coerce_size = estimate_size(arg_coerce, DL);
  std::string arg_coerce_type_str;
  llvm::raw_string_ostream ostream(arg_coerce_type_str);
  arg_coerce_type->print(ostream);
  std::string line = func->getName().str() + "|" + std::to_string(arg_pos) +
                     "|" + arg_original_name + "|" + arg_original_type + "|" +
                     arg_coerce_name + "|" + arg_coerce_type_str + "|" +
                     std::to_string(arg_coerce_size) + "\n";
  dumpLine(line, fileName);
}

std::string remove_quotes(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\"'), s.end());
  return s;
}

void dumpApiInfo(function_record a_fun) {
  std::string fileName = getApiFileLog();
  std::string line = a_fun.to_json() + "\n";
  dumpLine(line, fileName);
}
std::string argument_record::to_json() const {
  std::string str_ret;

  str_ret += "{";
  str_ret += "\"name\": \"" + this->name + "\", ";
  str_ret += "\"flag\": \"" + this->flag + "\", ";
  str_ret += "\"size\": \"" + std::to_string(this->size) + "\", ";
  str_ret += "\"type\": \"" + remove_quotes(this->type) + "\"";
  str_ret += "}";

  return str_ret;
}

std::string function_record::to_json() {
  std::string str_ret;
  str_ret += "{";
  str_ret += "\"function_name\": \"" + this->function_name + "\", ";
  str_ret += "\"return_info\": " + this->return_info.to_json() + ", ";
  str_ret += "\"is_vararg\": " + this->is_vararg + ", ";
  str_ret += "\"arguments_info\": [";

  int max_arg = this->arguments_info.size();
  int n_arg = 0;
  for (auto arg : this->arguments_info) {
    str_ret += arg.to_json();
    if (n_arg < max_arg - 1)
      str_ret += ", ";
    n_arg++;
  }

  str_ret += "]"; // close argument_info list

  str_ret += "}";

  return str_ret;
}

uint64_t estimate_size(const llvm::Argument *arg, const llvm::DataLayout *DL) {
  llvm::Type *type_to_size = nullptr;

  if (arg->hasByValAttr()) {
    type_to_size = arg->getParamByValType();
  } else {
    type_to_size = arg->getType();
  }

  return type_to_size->isSized() ? DL->getTypeSizeInBits(type_to_size) : 0;
}
uint64_t estimate_size(llvm::Type *type, const llvm::DataLayout *DL) {
  return type->isSized() ? DL->getTypeSizeInBits(type) : 0;
}

} // namespace liberator
