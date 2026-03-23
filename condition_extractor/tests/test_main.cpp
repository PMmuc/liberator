#include <ConditionExtractor.hpp>
#include <Config.h>
#include <GlobalStruct.h>
#include <ValueMetadata.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json/json.h>

#include "Util/Options.h"
#include "WPA/Andersen.h"
#include <MSSA/SVFGBuilder.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <SVFIR/SVFVariables.h>

#include <unistd.h>

#include "config.h"

namespace fs = std::filesystem;

TEST_CASE("Condition Extraction on .ll files", "[integration]") {
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);
  std::string assets_dir = ASSETS_DIR;

  if (!fs::exists(assets_dir)) {
    FAIL("Test assets directory not found at: " << assets_dir);
  }

  // Set LIBFUZZ_LOG_PATH if not set to avoid segfault/crash in logger
  if (!getenv("LIBFUZZ_LOG_PATH")) {
    fs::path log_path = fs::current_path() / "logs" / "";
    fs::create_directories(log_path);
    // putenv requires a static buffer or leak, but for tests it is okay-ish to
    // set it once Better to use setenv if available (linux)
    setenv("LIBFUZZ_LOG_PATH", log_path.c_str(), 1);
  }

  for (const auto &entry : fs::directory_iterator(assets_dir)) {
    if (entry.path().extension() == ".ll" &&
        entry.path().filename() != "test_meta.ll" &&
        entry.path().filename() != "test_meta.bc") {
      std::string file_path = entry.path().string();
      std::string file_name = entry.path().filename().string();
      fs::path json_p = entry.path();
      json_p.replace_extension(".json");
      std::string json_path = json_p.string();

      SECTION("Testing " + file_name) {
        INFO("Processing: " + file_path);

        // Check if expected JSON exists
        if (!fs::exists(json_path)) {
          WARN("No expected JSON found for " << file_name
                                             << ", skipping verification.");
          continue;
        }

        // Setup configuration
        std::vector<std::string> modules = {file_path};
        std::set<std::string> functions = {
            "main"}; // Default to main, or could read from sidebar

        auto start_time = std::chrono::high_resolution_clock::now();

        // Run Extraction
        auto extractor =
            liberator::make_condition_extractor(modules, functions);
        REQUIRE(extractor != nullptr);

        auto conditions = extractor->extract_function_conditions();

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end_time - start_time)
                            .count();

        std::cout << "Execution time for " << file_name << ": " << duration
                  << "ms" << std::endl;

        // Verify Results
        Json::Value actual_json = liberator::to_json(conditions, false);

        // Load expected JSON
        std::ifstream json_file(json_path);
        Json::Value expected_json;
        Json::Reader reader;
        REQUIRE(reader.parse(json_file, expected_json));

        // Simple comparison (exact match)
        // For more robust comparison, we might need to canonicalize or ignore
        // order But Json::Value equality checks structure and values.

        // Note: The dummy json I created might not match exactly what to_json
        // produces (e.g. empty fields) We assert they are equal
        CHECK(actual_json == expected_json);
      }
    }
  }
}

TEST_CASE("extractParameterMetadata standalone test", "[unit]") {
  config_t::instance()->debug = true;
  config_t::instance()->log_tags.insert("paramMetadata");
  config_t::instance()->log_tags.insert("handler");
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);
  // std::string bitcode_name = "test_meta";
  std::string bitcode_name = "test_context1";
  // std::string function = "test_parameter_metadata";
  std::string function = "test_fun";
  std::vector<std::string> module_name_vec = {std::string(ASSETS_DIR) + "/" +
                                              bitcode_name + ".bc"};
  std::set<std::string> functions = {function};

  auto extractor =
      liberator::make_condition_extractor(module_name_vec, functions);
  REQUIRE(extractor != nullptr);

  auto pag = SVF::SVFIR::getPAG();
  auto svfg = extractor->get_svfg();

  std::string dot_path =
      "/mnt/c/Users/MaschPaul/Downloads/" + bitcode_name + "_svfg";
  svfg->dump(dot_path);
  std::string sys_cmd =
      "dot -Tpng " + dot_path + ".dot -o " + dot_path + ".png";
  int sys_res = system(sys_cmd.c_str());
  (void)sys_res; // suppress unused warning

  auto llvmModuleSet = SVF::LLVMModuleSet::getLLVMModuleSet();

  auto svf_fun = pag->getFunObjVar(function);
  std::cout << "DEBUG: svf_fun = " << svf_fun << std::endl;
  REQUIRE(svf_fun != nullptr);

  auto params = pag->getFunArgsMap()[svf_fun];
  std::cout << "DEBUG: params.size() = " << params.size() << std::endl;
  // REQUIRE(params.size() == 4);

  // Test Param 1: int*
  auto param1 = params[0];
  std::cout << "DEBUG: param1 extracted" << std::endl;
  auto formal_param_llvm1 = llvmModuleSet->getLLVMValue(param1);
  std::cout << "DEBUG: llvm value extracted" << std::endl;
  auto metadata_p1 = liberator::extractParameterMetadata(
      *svfg, formal_param_llvm1, formal_param_llvm1->getType(),
      param1->getId());
  /*
     auto ats1 = metadata_p1.getAccessTypeSet();
     int reads = 0, writes = 0;
     for (auto at : *ats1) {
       if (at.get_kind() == liberator::AccessType::kind_e::read)
         reads++;
       if (at.get_kind() == liberator::AccessType::kind_e::write)
         writes++;
     }

     CHECK(reads > 0);  // "id" field read
     CHECK(writes > 0); // "buffer_len" field written

     // Test Param 2: int* array indexing
     auto param2 = params[1];
     auto formal_param_llvm2 = llvmModuleSet->getLLVMValue(param2);
     auto metadata_p2 = liberator::extractParameterMetadata(
         *svfg, formal_param_llvm2, formal_param_llvm2->getType(),
         param2->getId());

     auto ats2 = metadata_p2.getAccessTypeSet();
     bool write_found = false;
     for (auto at : *ats2) {
       if (at.get_kind() == liberator::AccessType::kind_e::write) {
         write_found = true;
       }
     }
     CHECK(write_found); // param2[i] = ...

     // Param 2 array depends on length (Param 3)
     std::string depends_on = liberator::extractLenDependencyParameter(
         param2, metadata_p2, *svfg, svf_fun);
     CHECK(depends_on == "param_2"); // zero-indexed excluding self (Param 3
     logic)

     // Cleanup is handled by the extractor destructor*/
}
