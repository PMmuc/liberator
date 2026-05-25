#include <ConditionExtractor.hpp>
#include <Config.h>
#include <GlobalStruct.h>
#include <Util/GeneralType.h>
#include <ValueMetadata.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json/json.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "Util/Options.h"
#include "WPA/Andersen.h"
#include <Graphs/CallGraph.h>
#include <MSSA/SVFGBuilder.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <SVFIR/SVFVariables.h>
#include <llvm/Demangle/Demangle.h>

#include <sys/wait.h>
#include <unistd.h>

#include "Config.h"
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

void write_mssa_file(SVFG *svfg, const std::string &filename) {
  std::ofstream file(filename);

  if (file.is_open()) {
    svfg->getMSSA()->dumpMSSA(file);
    file.close();
  } else {
    SVFUtil::errs() << "[ERROR] Failed to open file for writing.\n";
  }
}

// Strip the parameter list and any trailing qualifiers from a demangled C++
// signature so "testfunc(A*)" becomes "testfunc" and
// "ns::C::foo(int) const" becomes "ns::C::foo".
static std::string base_name_from_demangled(const std::string &demangled) {
  size_t depth = 0;
  for (size_t i = 0; i < demangled.size(); ++i) {
    char c = demangled[i];
    if (c == '<')
      ++depth;
    else if (c == '>' && depth > 0)
      --depth;
    else if (c == '(' && depth == 0)
      return demangled.substr(0, i);
  }
  return demangled;
}

// Look up a function in the PAG by demangled base name, falling back to a
// direct (mangled) match. Lets C++ tests reference functions by their source
// name (e.g. "testfunc") rather than the mangled symbol.
static const SVF::FunObjVar *
find_fun_by_demangled_name(SVF::SVFIR *pag, const std::string &name) {
  if (auto *direct = pag->getFunObjVar(name))
    return direct;

  for (const auto &item : *pag->getCallGraph()) {
    const std::string &mangled = item.second->getName();
    std::string demangled = llvm::demangle(mangled);
    if (demangled == name || base_name_from_demangled(demangled) == name) {
      return item.second->getFunction();
    }
  }
  return nullptr;
}

void run_extract_parameter_test(const std::string &bitcode_filename,
                                const std::string &function) {
  config_t::instance()->debug = true;
  config_t::instance()->log_tags.insert("paramMetadata");
  // config_t::instance()->log_tags.insert("handler");
  // config_t::instance()->log_tags.insert("GEPHandler");
  config_t::instance()->log_tags.insert("MyExLog");
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);

  std::string file_path = std::string(ASSETS_DIR) + "/" + bitcode_filename;
  if (!fs::exists(file_path)) {
    file_path = std::string(BINARY_DIR) + "/assets/" + bitcode_filename;
  }
  std::vector<std::string> modules = {file_path};
  std::set<std::string> functions = {function};

  std::string temp_log =
      "/tmp/svf_standalone_" + std::to_string(getpid()) + ".log";

  // Set LIBERATOR_TEST_NO_FORK=1 to skip the fork wrapper and run inline.
  // The fork is normally there to isolate child crashes from Catch2, but it
  // makes interactive debugging painful (gdb has to follow into the child).
  bool no_fork = getenv("LIBERATOR_TEST_NO_FORK") != nullptr;
  auto pid = no_fork ? 0 : fork();

  if (pid == 0) {
    // Child process: Redirect std::cout to a temporary file so the parent can
    // read it back into Catch2's stream. In no_fork mode we don't redirect at
    // all so output goes straight to the user's terminal/debugger.
    std::ofstream out_file;
    std::streambuf *old_cout_buf = nullptr;
    if (!no_fork) {
      out_file.open(temp_log);
      old_cout_buf = std::cout.rdbuf(out_file.rdbuf());
    }

    auto extractor = liberator::make_condition_extractor(modules, functions);
    REQUIRE(extractor != nullptr);

    auto pag = SVF::SVFIR::getPAG();
    auto svfg = extractor->get_svfg();

    std::string home_directory = "/mnt/c/Users/MaschPaul/Downloads/";
    // Strip .bc for dot path
    std::string bitcode_name =
        bitcode_filename.substr(0, bitcode_filename.find_last_of("."));
    write_mssa_file(svfg, home_directory + bitcode_name + "_mssa.txt");
    auto icfg = pag->getICFG();
    auto svfg_dot_path = home_directory + bitcode_name + "_svfg";
    auto icfg_dot_path = home_directory + bitcode_name + "_icfg";

    icfg->dump(icfg_dot_path);
    svfg->dump(svfg_dot_path);
    std::string sys_cmd1 =
        "dot -Tpng " + svfg_dot_path + ".dot -o " + svfg_dot_path + ".png";
    std::string sys_cmd2 =
        "dot -Tpng " + icfg_dot_path + ".dot -o " + icfg_dot_path + ".png";
    int sys_res = system(sys_cmd1.c_str());
    sys_res = system(sys_cmd2.c_str());
    (void)sys_res; // suppress unused warning

    auto llvmModuleSet = SVF::LLVMModuleSet::getLLVMModuleSet();

    auto svf_fun = find_fun_by_demangled_name(pag, function);
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
        /*auto metadata_p1 = liberator::extractParameterMetadata(
            *svfg, formal_param_llvm1, formal_param_llvm1->getType(),
            param1->getId());*/
        auto metadata_p1 = liberator::my_extract_parameter_metadata(
            *svfg, formal_param_llvm1, formal_param_llvm1->getType());
      }
    }

    std::cout.flush();
    if (no_fork) {
      if (old_cout_buf)
        std::cout.rdbuf(old_cout_buf);
      return;
    }
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

TEST_CASE("svf test arrays", "[unit]") {
  run_extract_parameter_test("arrays.bc", "main");
}
TEST_CASE("svf test basic_load_store", "[unit]") {
  run_extract_parameter_test("basic_load_store.bc", "main");
}
TEST_CASE("svf test complex_call_graph", "[unit]") {
  run_extract_parameter_test("complex_call_graph.bc", "main");
}
TEST_CASE("svf test control_flow", "[unit]") {
  run_extract_parameter_test("control_flow.bc", "main");
}
TEST_CASE("svf test function_calls", "[unit]") {
  run_extract_parameter_test("function_calls.bc", "main");
}
TEST_CASE("svf test globals", "[unit]") {
  run_extract_parameter_test("globals.bc", "main");
}
TEST_CASE("svf test pointer_arithmetic", "[unit]") {
  run_extract_parameter_test("pointer_arithmetic.bc", "main");
}
TEST_CASE("svf test struct_access", "[unit]") {
  run_extract_parameter_test("struct_access.bc", "test_fun");
}
TEST_CASE("svf test array_of_structs", "[unit]") {
  run_extract_parameter_test("array_of_structs.bc", "test_fun");
}
TEST_CASE("svf test classes", "[unit]") {
  run_extract_parameter_test("test_classes.bc", "testfunc");
}
TEST_CASE("svf test test_context", "[unit]") {
  run_extract_parameter_test("test_context.bc", "test_fun");
}
TEST_CASE("svf test test_context1", "[unit]") {
  run_extract_parameter_test("test_context1.bc", "test_fun");
}
TEST_CASE("svf test test_malloc", "[unit]") {
  run_extract_parameter_test("test_malloc.bc", "test_fun");
}
TEST_CASE("svf test recursive", "[unit]") {
  run_extract_parameter_test("recursive.bc", "testfunc");
}
TEST_CASE("svf test test_meta", "[unit]") {
  run_extract_parameter_test("test_meta.bc", "test_parameter_metadata");
}
