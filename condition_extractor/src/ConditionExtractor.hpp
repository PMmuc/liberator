#pragma once

#include "FunctionConditions.hpp"
#include "GlobalStruct.h"
#include <Graphs/SVFG.h>
#include <MSSA/SVFGBuilder.h>
#include <SVF-LLVM/BasicTypes.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <SVFIR/SVFIR.h>
#include <memory>
#include <set>
#include <string>

namespace liberator {

class condition_extractor_t {
  typedef SVF::PAG::FunToArgsListMap fun_param_map_t;

  condition_extractor_t(const std::set<std::string> &&functions,
                        SVF::Module *module, std::unique_ptr<SVFGBuilder> svfg,
                        SVFIR *pag, GlobalStruct *point_to_analyses) noexcept;

public:
  ~condition_extractor_t() noexcept;

  function_condition_set_t extract_function_conditions();

  SVFG *get_svfg() const { return svfg_builder_->getSVFG(); }

private:
  std::set<std::string> functions_;
  SVF::Module *module_;
  std::unique_ptr<SVFGBuilder> svfg_builder_;
  SVFIR *pag_;
  GlobalStruct *point_to_analyses_;
  friend unique_ptr<condition_extractor_t>
  make_condition_extractor(std::vector<std::string> &,
                           const std::set<std::string> &);
};

unique_ptr<condition_extractor_t>
make_condition_extractor(std::vector<std::string> &module_name_vec,
                         const std::set<std::string> &functions);

void save_llvm_data_layout();

} // namespace liberator
