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
#include <sys/wait.h>
#include <unistd.h>

#include "Util/Options.h"
#include "WPA/Andersen.h"
#include <MSSA/SVFGBuilder.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <SVFIR/SVFVariables.h>

#include <sys/wait.h>
#include <unistd.h>

#include "Config.h"

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

        int temp_pipe[2];
        REQUIRE(pipe(temp_pipe) == 0);

        pid_t pid = fork();
        REQUIRE(pid >= 0);

        if (pid == 0) {
          // Child process
          close(temp_pipe[0]);

          auto extractor =
              liberator::make_condition_extractor(modules, functions);
          if (extractor == nullptr) {
            std::cerr << "Extractor is null" << std::endl;
            exit(1);
          }

          auto conditions = extractor->extract_function_conditions();
          Json::Value actual_json = liberator::to_json(conditions, false);
          std::string json_str = actual_json.toStyledString();

          size_t remaining = json_str.length();
          const char *data = json_str.c_str();
          while (remaining > 0) {
            ssize_t written = write(temp_pipe[1], data, remaining);
            if (written < 0) {
              std::cerr << "Error writing to pipe" << std::endl;
              exit(1);
            }
            data += written;
            remaining -= written;
          }

          close(temp_pipe[1]);
          exit(0);
        }

        // Parent process
        close(temp_pipe[1]);

        std::string actual_str = "";
        char buffer[4096];
        ssize_t n;
        while ((n = read(temp_pipe[0], buffer, sizeof(buffer))) > 0) {
          actual_str.append(buffer, n);
        }
        close(temp_pipe[0]);

        int status;
        waitpid(pid, &status, 0);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end_time - start_time)
                            .count();

        std::cout << "Execution time for " << file_name << ": " << duration
                  << "ms" << std::endl;

        REQUIRE((WIFEXITED(status) && WEXITSTATUS(status) == 0));

        // Verify Results
        Json::Value actual_json;
        Json::Reader json_reader;
        REQUIRE(json_reader.parse(actual_str, actual_json));

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

void run_extract_parameter_test(const std::string &bitcode_filename,
                                const std::string &function) {
  config_t::instance()->debug = true;
  config_t::instance()->log_tags.insert("paramMetadata");
  config_t::instance()->log_tags.insert("handler");
  config_t::instance()->log_tags.insert("GEPHandler");
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);

  std::vector<std::string> modules = {std::string(ASSETS_DIR) + "/" +
                                      bitcode_filename};
  std::set<std::string> functions = {function};

  std::string temp_log =
      "/tmp/svf_standalone_" + std::to_string(getpid()) + ".log";
  auto pid = fork();

  if (pid == 0) {
    // Child process: Redirect std::cout to a temporary file so the parent can
    // read it back into Catch2's stream
    std::ofstream out_file(temp_log);
    std::streambuf *old_cout_buf = std::cout.rdbuf(out_file.rdbuf());

    auto extractor = liberator::make_condition_extractor(modules, functions);
    REQUIRE(extractor != nullptr);

    auto pag = SVF::SVFIR::getPAG();
    auto svfg = extractor->get_svfg();

    // Strip .bc for dot path
    std::string bitcode_name =
        bitcode_filename.substr(0, bitcode_filename.find_last_of("."));
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

    if (svf_fun != nullptr) {
      auto params = pag->getFunArgsMap()[svf_fun];
      std::cout << "DEBUG: params.size() = " << params.size() << std::endl;

      if (params.size() > 0) {
        // Test Param 1: int*
        auto param1 = params[0];
        std::cout << "DEBUG: param1 extracted" << std::endl;
        auto formal_param_llvm1 = llvmModuleSet->getLLVMValue(param1);
        std::cout << "DEBUG: llvm value extracted" << std::endl;
        auto metadata_p1 = liberator::extractParameterMetadata(
            *svfg, formal_param_llvm1, formal_param_llvm1->getType(),
            param1->getId());
      }
    }

    std::cout.flush();
    exit(0);
  }

  // Parent process
  int status;
  waitpid(pid, &status, 0);

  // Read the child's stdout back into Catch2's managed stdout
  std::ifstream in_file(temp_log);
  if (in_file) {
    std::cout << in_file.rdbuf();
  }
  fs::remove(temp_log);

  REQUIRE((WIFEXITED(status) && WEXITSTATUS(status) == 0));
}

TEST_CASE("extractParameterMetadata test_context1", "[unit]") {
  run_extract_parameter_test("test_context1.bc", "test_fun");
}

TEST_CASE("extractParameterMetadata test_context", "[unit]") {
  run_extract_parameter_test("test_context.bc", "test_fun");
}

TEST_CASE("extractParameterMetadata test_struct", "[unit]") {
  run_extract_parameter_test("struct_access.bc", "test_fun");
}