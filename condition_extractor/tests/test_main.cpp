#include <ConditionExtractor.hpp>
#include <Config.h>
#include <GlobalStruct.h>
#include <MSSA/MemRegion.h>
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

#include "AccessType.h"
#include "AccessTypeIO.h"
#include "DebugInfoParser.hpp"
#include "ScevLenDependency.hpp"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include <Graphs/CallGraph.h>
#include <MSSA/SVFGBuilder.h>
#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <SVFIR/SVFVariables.h>
#include <functional>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/TypedPointerType.h>

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

  if (!file.is_open()) {
    SVFUtil::errs() << "[ERROR] Failed to open: " << filename << "\n";
    return;
  }

  auto memssa = svfg->getMSSA();
  auto gen = memssa->getMRGenerator();

  std::streambuf *saved = std::cout.rdbuf(file.rdbuf());
  SVFUtil::outs() << "Memory Regions:\n";
  for (const MemRegion *mr : gen->getMRSet()) {
    SVFUtil::outs() << "MR_" << mr->getMRID() << "\t" << mr->dumpStr() << "\n";
  }
  SVFUtil::outs() << "------------------------------\n";
  svfg->getMSSA()->dumpMSSA(file);
  std::cout.flush();
  std::cout.rdbuf(saved);
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
    cout << mangled << endl;
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
  // config_t::instance()->log_tags.insert("paramMetadata");
  config_t::instance()->log_tags.insert("handler");
  config_t::instance()->log_tags.insert("GEPHandler");
  // config_t::instance()->log_tags.insert("MyExLog");
  // config_t::instance()->log_tags.insert("Type");
  // config_t::instance()->log_tags.insert("Global");
  config_t::instance()->log_tags.insert("Summary");
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);

  std::string file_path =
      std::string(BINARY_DIR) + "/assets/" + bitcode_filename;
  if (!fs::exists(file_path)) {
    file_path = std::string(ASSETS_DIR) + "/" + bitcode_filename;
  }
  std::vector<std::string> modules = {file_path};
  std::set<std::string> functions = {function};

  std::string temp_log =
      "/tmp/svf_standalone_" + std::to_string(getpid()) + ".log";

  // disable forking making debugging easier because following childs is a pain.
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
    if (!svf_fun) {
      std::cerr << "ERROR: No function found with name: " << function
                << " in bitcode: " << bitcode_name << std::endl;
      return;
    }
    std::cout << "DEBUG: Function " << svf_fun->toString() << std::endl;

    if (svf_fun != nullptr) {
      auto params = pag->getFunArgsMap()[svf_fun];
      std::cout << "DEBUG: Number of Parameters: " << params.size()
                << std::endl;

      if (params.size() > 0) {
        // Test Param 1: int*
        auto param1 = params[0];
        extractor->extract_function_conditions();
        for (auto *param : params) {
          auto *formal_param_llvm = llvmModuleSet->getLLVMValue(param);
          if (formal_param_llvm &&
              formal_param_llvm->getType()->isPointerTy()) {
            auto metadata = liberator::my_extract_parameter_metadata(
                *svfg, formal_param_llvm, param->getId());
            std::cout
                << "DEBUG: my_extract_parameter_metadata completed for param "
                << param->getId() << std::endl;

            cout << liberator::print_summary(metadata, true) << endl;
          }
        }
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

// Fork-isolated assertion on the bottom-up parameter summary. SVF keeps
// module-global singletons, so every extraction runs in a fresh child
// process; the child encodes the outcome of `pred` in its exit code
// (0 = pred holds, 2 = pred fails, 1 = setup error) and the parent asserts
// on it. Set LIBERATOR_TEST_NO_FORK to run in-process for debugging.
static void run_param_metadata_check(
    const std::string &bitcode_filename, const std::string &function,
    unsigned param_index, bool consider_indirect,
    const std::function<bool(const liberator::ValueMetadata &)> &pred) {
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);
  config_t::instance()->consider_indirect_calls = consider_indirect;

  std::string file_path =
      std::string(BINARY_DIR) + "/assets/" + bitcode_filename;
  if (!fs::exists(file_path)) {
    file_path = std::string(ASSETS_DIR) + "/" + bitcode_filename;
  }
  REQUIRE(fs::exists(file_path));

  std::vector<std::string> modules = {file_path};
  std::set<std::string> functions = {function};

  bool no_fork = getenv("LIBERATOR_TEST_NO_FORK") != nullptr;
  pid_t pid = no_fork ? 0 : fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto fail_child = [&](const char *msg) {
      std::cerr << "[param-check setup error] " << msg << std::endl;
      if (no_fork)
        FAIL(msg);
      else
        _exit(1);
    };

    auto extractor = liberator::make_condition_extractor(modules, functions);
    if (!extractor)
      return fail_child("extractor is null");

    auto *pag = SVF::SVFIR::getPAG();
    auto *svfg = extractor->get_svfg();
    auto *llvm_module_set = SVF::LLVMModuleSet::getLLVMModuleSet();

    // Runs the full pipeline, which populates myCallEdgeMap_inst and the
    // bottom-up summaries.
    extractor->extract_function_conditions();

    const SVF::FunObjVar *svf_fun = find_fun_by_demangled_name(pag, function);
    if (!svf_fun)
      return fail_child("function not found");

    auto params = pag->getFunArgsMap()[svf_fun];
    if (param_index >= params.size())
      return fail_child("parameter index out of range");

    auto *param = params[param_index];
    auto *param_llvm = llvm_module_set->getLLVMValue(param);
    if (!param_llvm || !param_llvm->getType()->isPointerTy())
      return fail_child("parameter is not a pointer");

    auto metadata = liberator::my_extract_parameter_metadata(*svfg, param_llvm,
                                                             param->getId());

    std::cout << "[param-check] " << function << " param " << param_index
              << ":\n"
              << liberator::print_summary(metadata, true) << std::endl;

    bool ok = pred(metadata);
    if (no_fork) {
      CHECK(ok);
      return;
    }
    std::cout.flush();
    _exit(ok ? 0 : 2);
  }

  int status;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  INFO("child exit status = " << WEXITSTATUS(status)
                              << " (1 = setup error, 2 = predicate false)");
  CHECK(WEXITSTATUS(status) == 0);
}

// Same fork protocol as run_param_metadata_check, but drives
// extractReturnMetadata on the function's return node instead of a formal
// parameter. The return path is currently not wired into
// extract_function_conditions (the "Process Return" block in
// ConditionExtractor.cpp is commented out), so the pipeline is run only to
// populate myCallEdgeMap_inst and the extraction is invoked directly.
static void run_return_metadata_check(
    const std::string &bitcode_filename, const std::string &function,
    bool consider_indirect,
    const std::function<bool(const liberator::ValueMetadata &)> &pred) {
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);
  config_t::instance()->consider_indirect_calls = consider_indirect;

  std::string file_path =
      std::string(BINARY_DIR) + "/assets/" + bitcode_filename;
  if (!fs::exists(file_path)) {
    file_path = std::string(ASSETS_DIR) + "/" + bitcode_filename;
  }
  REQUIRE(fs::exists(file_path));

  std::vector<std::string> modules = {file_path};
  std::set<std::string> functions = {function};

  bool no_fork = getenv("LIBERATOR_TEST_NO_FORK") != nullptr;
  pid_t pid = no_fork ? 0 : fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto fail_child = [&](const char *msg) {
      std::cerr << "[return-check setup error] " << msg << std::endl;
      if (no_fork)
        FAIL(msg);
      else
        _exit(1);
    };

    auto extractor = liberator::make_condition_extractor(modules, functions);
    if (!extractor)
      return fail_child("extractor is null");

    auto *pag = SVF::SVFIR::getPAG();
    auto *svfg = extractor->get_svfg();
    auto *llvm_module_set = SVF::LLVMModuleSet::getLLVMModuleSet();

    extractor->extract_function_conditions();

    const SVF::FunObjVar *svf_fun = find_fun_by_demangled_name(pag, function);
    if (!svf_fun)
      return fail_child("function not found");
    if (!pag->funHasRet(svf_fun))
      return fail_child("function has no return node");

    // The return *value*, not the return instruction - see the doc comment on
    // extractReturnMetadata.
    const SVF::ValVar *ret_var = pag->getFunRet(svf_fun);
    const llvm::Value *ret_llvm = llvm_module_set->getLLVMValue(ret_var);
    if (!ret_llvm)
      return fail_child("return node has no llvm value");

    auto metadata = liberator::extractReturnMetadata(*svfg, ret_llvm);

    std::cout << "[return-check] " << function << ":\n"
              << liberator::print_summary(metadata, true) << std::endl;

    bool ok = pred(metadata);
    if (no_fork) {
      CHECK(ok);
      return;
    }
    std::cout.flush();
    _exit(ok ? 0 : 2);
  }

  int status;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  INFO("child exit status = " << WEXITSTATUS(status)
                              << " (1 = setup error, 2 = predicate false)");
  CHECK(WEXITSTATUS(status) == 0);
}

static bool metadata_has_kind(const liberator::ValueMetadata &m,
                              liberator::AccessType::kind_e kind) {
  for (const auto &at : m.get_access_type_set()) {
    if (at.get_kind() == kind)
      return true;
  }
  return false;
}

// Looks for one exact field path, e.g. {2} with kind write for p->field2 = x.
static bool metadata_has_field_access(const liberator::ValueMetadata &m,
                                      const std::vector<int> &fields,
                                      liberator::AccessType::kind_e kind) {
  for (const auto &at : m.get_access_type_set()) {
    if (at.get_kind() == kind && at.get_fields() == fields)
      return true;
  }
  return false;
}

// is_array detection in the GEP handler: array-style indexing of a pointer
// parameter (a[i]) must set is_array.
TEST_CASE("gep handler sets is_array for array indexing", "[unit][isarray]") {
  run_param_metadata_check(
      "gep_array_param.bc", "read_array", 0, /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) { return m.isArray(); });
}

// Constant struct-field selection (p->x) must NOT be treated as an array.
TEST_CASE("gep handler leaves is_array false for field access",
          "[unit][isarray]") {
  run_param_metadata_check(
      "gep_array_param.bc", "read_field", 0, /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) { return !m.isArray(); });
}

// AParm / indirect-call path: a parameter flowing into an indirect call must
// pick up the resolved callee's write effect via merge_summary. This case is
// resolved by GlobalStruct signature matching (myCallEdgeMap_inst).
TEST_CASE("indirect call merges callee write into param summary",
          "[unit][aparam]") {
  run_param_metadata_check(
      "indirect_param.bc", "dispatch", 0, /*consider_indirect=*/true,
      [](const liberator::ValueMetadata &m) {
        return metadata_has_kind(m, liberator::AccessType::kind_e::write);
      });
}

// Same, but for an indirect call resolved precisely by points-to (recorded on
// the call graph, not in myCallEdgeMap_inst). Exercises the getIndCSCallees
// arm of callee_targets.
TEST_CASE("resolved indirect call merges callee write into param summary",
          "[unit][aparam]") {
  run_param_metadata_check(
      "indirect_param_resolved.bc", "dispatch", 0, /*consider_indirect=*/true,
      [](const liberator::ValueMetadata &m) {
        return metadata_has_kind(m, liberator::AccessType::kind_e::write);
      });
}

// Return path, opaque-pointer regression. make_buffer spills the malloc into a
// local before returning it, so the IR clang emits at -O0 is
//   %call = call ptr @malloc(i64 16)
//   store ptr %call, ptr %b
//   %3 = load ptr, ptr %b
//   ret ptr %3
// Under LLVM <= 14 there was a `bitcast i8* %call to %struct.Buffer*` in that
// chain and extractReturnMetadata found the allocation by matching the cast's
// destination type against the return type (leadsToBitCastOfType). Opaque
// pointers removed the cast, so the allocation is only reachable by following
// the value flow across the alloca. A missing `create` here means the analysis
// lost the allocation, i.e. callers are told the returned pointer is not owned.
TEST_CASE("return summary records malloc spilled through a local",
          "[unit][return][malloc]") {
  run_return_metadata_check(
      "ret_malloc_alloca.bc", "make_buffer", /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) {
        return metadata_has_kind(m, liberator::AccessType::kind_e::create);
      });
}

// Control for the case above: the allocation is returned directly, so the
// malloc reaches the `ret` over a plain def-use chain with no memory-SSA hop.
// If this passes while make_buffer fails, the gap is the store/load through
// the alloca specifically, not malloc recognition in general.
TEST_CASE("return summary records malloc returned directly",
          "[unit][return][malloc]") {
  run_return_metadata_check(
      "ret_malloc_alloca.bc", "make_buffer_direct", /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) {
        return metadata_has_kind(m, liberator::AccessType::kind_e::create);
      });
}

// Field-sensitive GEP tracing. test_meta.c touches three distinct fields of
// struct MyStruct { int id; char *buffer; int buffer_len; } through param1:
//   local_id = param1->id           -> .0 read
//   external_sink(param1->buffer)   -> .1 read
//   param1->buffer_len = len        -> .2 write
// Whole-struct accesses without any field index mean the DWARF/LLVM type
// match in handleGep failed and the field indices were never recorded.
TEST_CASE("gep handler traces struct field accesses", "[unit][isarray][gep]") {
  run_param_metadata_check(
      "test_meta.bc", "test_parameter_metadata", 0,
      /*consider_indirect=*/false, [](const liberator::ValueMetadata &m) {
        using kind_e = liberator::AccessType::kind_e;
        return metadata_has_field_access(m, {0}, kind_e::read) &&
               metadata_has_field_access(m, {1}, kind_e::read) &&
               metadata_has_field_access(m, {2}, kind_e::write);
      });
}

// The same function's int *param2 is written as param2[i] in a loop, so it is
// an array rather than a field selection.
TEST_CASE("gep handler flags the array parameter of test_meta",
          "[unit][isarray][gep]") {
  run_param_metadata_check(
      "test_meta.bc", "test_parameter_metadata", 1,
      /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) { return m.isArray(); });
}

// The DWARF type printer is pure metadata handling, so it can be driven
// directly with DIBuilder instead of going through the SVF pipeline.
TEST_CASE("di type printer emits old-style llvm ir types", "[unit][ditype]") {
  using namespace llvm;
  using namespace llvm::dwarf;
  using liberator::to_string;

  LLVMContext ctx;
  Module mod("ditype_test", ctx);
  DIBuilder db(mod);
  DIFile *file = db.createFile("ditype_test.c", "/");
  db.createCompileUnit(DW_LANG_C99, file, "test", false, "", 0);

  auto *i32 = db.createBasicType("int", 32, DW_ATE_signed);
  auto *i8 = db.createBasicType("char", 8, DW_ATE_signed_char);
  auto *dbl = db.createBasicType("double", 64, DW_ATE_float);
  auto *boolean = db.createBasicType("_Bool", 8, DW_ATE_boolean);

  SECTION("base types") {
    CHECK(to_string(i32) == "i32");
    CHECK(to_string(i8) == "i8");
    CHECK(to_string(dbl) == "double");
    CHECK(to_string(boolean) == "i1");
    CHECK(to_string(static_cast<const DIType *>(nullptr)) == "void");
  }

  auto *i8p = db.createPointerType(i8, 64);

  SECTION("pointers") {
    CHECK(to_string(i8p) == "i8*");
    CHECK(to_string(db.createPointerType(i8p, 64)) == "i8**");
    // void * has no pointee in DWARF and is spelled i8* in typed-pointer IR.
    CHECK(to_string(db.createPointerType(nullptr, 64)) == "i8*");
  }

  SECTION("qualifiers and typedefs are peeled") {
    auto *const_i32 = db.createQualifiedType(DW_TAG_const_type, i32);
    CHECK(to_string(const_i32) == "i32");
    CHECK(to_string(db.createTypedef(const_i32, "my_int", file, 1, nullptr)) ==
          "i32");
    CHECK(to_string(db.createPointerType(const_i32, 64)) == "i32*");
  }

  // struct A { int id; char *buffer; };
  auto *id = db.createMemberType(nullptr, "id", file, 1, 32, 0, 0,
                                 DINode::FlagZero, i32);
  auto *buffer = db.createMemberType(nullptr, "buffer", file, 2, 64, 0, 64,
                                     DINode::FlagZero, i8p);
  auto *struct_a =
      db.createStructType(nullptr, "A", file, 1, 128, 0, DINode::FlagZero,
                          nullptr, db.getOrCreateArray({id, buffer}));

  SECTION("structs") {
    // By value: the full body, like an identified type definition.
    CHECK(to_string(struct_a) == "%struct.A = type { i32, i8* }");
    // Behind a pointer: the name only.
    CHECK(to_string(db.createPointerType(struct_a, 64)) == "%struct.A*");
  }

  SECTION("nested composites print by name") {
    auto *inner = db.createMemberType(nullptr, "inner", file, 1, 128, 0, 0,
                                      DINode::FlagZero, struct_a);
    auto *outer =
        db.createStructType(nullptr, "B", file, 1, 128, 0, DINode::FlagZero,
                            nullptr, db.getOrCreateArray({inner}));
    CHECK(to_string(outer) == "%struct.B = type { %struct.A }");
  }

  SECTION("unions keep their own prefix") {
    auto *u = db.createUnionType(nullptr, "U", file, 1, 32, 0, DINode::FlagZero,
                                 db.getOrCreateArray({id}));
    CHECK(to_string(db.createPointerType(u, 64)) == "%union.U*");
  }

  SECTION("arrays") {
    // char *a[4]
    CHECK(to_string(db.createArrayType(
              256, 0, i8p,
              db.getOrCreateArray({db.getOrCreateSubrange(0, 4)}))) ==
          "[4 x i8*]");
    // int a[2][3] carries both subranges on one composite.
    CHECK(to_string(db.createArrayType(
              192, 0, i32,
              db.getOrCreateArray({db.getOrCreateSubrange(0, 2),
                                   db.getOrCreateSubrange(0, 3)}))) ==
          "[2 x [3 x i32]]");
  }

  SECTION("function pointers") {
    // int (*)(char *, int)
    auto *sig =
        db.createSubroutineType(db.getOrCreateTypeArray({i32, i8p, i32}));
    CHECK(to_string(db.createPointerType(sig, 64)) == "i32 (i8*, i32)*");
    // void (*)(void)
    auto *void_sig =
        db.createSubroutineType(db.getOrCreateTypeArray({nullptr}));
    CHECK(to_string(db.createPointerType(void_sig, 64)) == "void ()*");
  }

  // A trailing null in the parameter slots is DW_TAG_unspecified_parameters -
  // the `...` of a variadic prototype, not a void parameter. Resolving it as
  // void made FunctionType::get assert; libxml2's xmlGenericErrorFunc
  // (`void (*)(void *, const char *, ...)`) aborted the whole run.
  SECTION("variadic function pointers resolve to a vararg FunctionType") {
    auto *va_sig = db.createSubroutineType(
        db.getOrCreateTypeArray({nullptr, i8p, i8p, nullptr}));
    auto *resolved = liberator::resolve_di_type_to_llvm(va_sig, mod);
    REQUIRE(resolved != nullptr);
    auto *fn = llvm::dyn_cast<FunctionType>(resolved);
    REQUIRE(fn != nullptr);
    CHECK(fn->isVarArg());
    CHECK(fn->getNumParams() == 2);
    CHECK(fn->getReturnType()->isVoidTy());

    // Behind a pointer, which is how the DWARF fallback in restore_llvm_type
    // actually reaches it.
    auto *ptr = liberator::resolve_di_type_to_llvm(
        db.createPointerType(va_sig, 64), mod);
    REQUIRE(ptr != nullptr);
    auto *tp = llvm::dyn_cast<TypedPointerType>(ptr);
    REQUIRE(tp != nullptr);
    CHECK(tp->getElementType() == fn);
  }

  SECTION("non-variadic function pointers stay non-vararg") {
    auto *sig = db.createSubroutineType(db.getOrCreateTypeArray({i32, i8p}));
    auto *fn = llvm::dyn_cast_if_present<FunctionType>(
        liberator::resolve_di_type_to_llvm(sig, mod));
    REQUIRE(fn != nullptr);
    CHECK_FALSE(fn->isVarArg());
    CHECK(fn->getNumParams() == 1);
  }

  db.finalize();
}

// External API models (accessTypeHandlers) have to be dispatched by
// merge_summary at the call boundary; memcpy has no body the bottom-up
// analysis could summarize, so without the dispatch the array flag is lost.
TEST_CASE("memcpy model marks parameter as array", "[unit][extapi]") {
  run_param_metadata_check(
      "extapi_effects.bc", "copy_buffer", 0, /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) { return m.isArray(); });
}

// memcpy_handler also records its size argument, which is what
// extractLenDependencyParameter later resolves to "param_2".
TEST_CASE("memcpy model records the length argument", "[unit][extapi]") {
  run_param_metadata_check("extapi_effects.bc", "copy_buffer", 0,
                           /*consider_indirect=*/false,
                           [](const liberator::ValueMetadata &m) {
                             liberator::ValueMetadata copy = m;
                             return !copy.getFunParams().empty();
                           });
}

// Same for the second memcpy operand (src).
TEST_CASE("memcpy model marks source parameter as array", "[unit][extapi]") {
  run_param_metadata_check(
      "extapi_effects.bc", "copy_buffer", 1, /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) { return m.isArray(); });
}

// strlen_handler sets the array flag without any length argument.
TEST_CASE("strlen model marks parameter as array", "[unit][extapi]") {
  run_param_metadata_check(
      "extapi_effects.bc", "measure", 0, /*consider_indirect=*/false,
      [](const liberator::ValueMetadata &m) { return m.isArray(); });
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
  run_extract_parameter_test("struct_access.bc", "test_func");
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
TEST_CASE("svf test type_inference1", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func");
}
TEST_CASE("svf test type_inference2", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func1");
}
TEST_CASE("svf test type_inference3", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func2");
}
TEST_CASE("svf test type_inference4", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func3");
}
TEST_CASE("svf test type_inference5", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func4");
}
TEST_CASE("svf test type_inference6", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func5");
}
TEST_CASE("svf test type_inference7", "[unit]") {
  run_extract_parameter_test("type_inference.bc", "test_func6");
}
// Compiled without -g so the DWARF fallback returns nothing — the chain
// then falls through to inferTypeFromForwardUses, which should pick
// %struct.ForwardUseStruct off the GEP in the function body.
TEST_CASE("svf test forward_use_scan", "[unit]") {
  run_extract_parameter_test("nodwarf_forward_use.bc", "test_forward_use");
}

TEST_CASE("svf test simple_phi", "[unit]") {
  run_extract_parameter_test("simple_phi.bc", "target_func");
}

TEST_CASE("svf test global_func_pointers", "[unit]") {
  run_extract_parameter_test("function_pointers.bc", "test_func");
}

TEST_CASE("svf test rec_simple", "[unit]") {
  run_extract_parameter_test("rec_simple.bc", "sum");
}

TEST_CASE("svf test rec_nested", "[unit]") {
  run_extract_parameter_test("rec_nested.bc", "outer_rec");
}

TEST_CASE("svf test rec_mutual", "[unit]") {
  run_extract_parameter_test("rec_mutual.bc", "foo");
}

TEST_CASE("svf test rec_struct", "[unit]") {
  run_extract_parameter_test("rec_struct.bc", "sum_list");
}

TEST_CASE("svf test rec_multiple_params", "[unit]") {
  run_extract_parameter_test("rec_multiple_params.bc", "copy_rec");
}

TEST_CASE("svf test rec_mutual_nonrec", "[unit]") {
  run_extract_parameter_test("rec_mutual_nonrec.bc", "rec_foo");
}

TEST_CASE("svf test test_array_malloc", "[unit]") {
  run_extract_parameter_test("test_array_malloc.bc", "test_fun");
}

TEST_CASE("svf test resolve_struct pointers", "[unit]") {
  run_extract_parameter_test("resolve_struct.bc", "test_func");
}

// ---------------------------------------------------------------------------
// Field-path explosion across a `T **` heap array (slist_path_explosion.c)
//
// slist_path_explosion.c is a reduced c-ares skip list. `struct list` has a
// `struct node **head` - a heap-allocated array of pointers - and `struct node`
// has a self-referential `next` walked in a loop plus a `parent` back edge to
// the list. c-ares' [24,3,2,2] appears here as [1,3,2,2].
//
// Reading head[lvl] is a load of a pointer *out of the heap*: the node it
// yields is not derived from `list` by any def-use chain, it comes from the
// store that filled the array. So every path past `.1.3` is only reachable by
// reasoning about memory, and in the SVFG the GEP for `list->head` and the node
// objects are separated by indirect (memory-SSA) edges - which is exactly what
// the dumped graph is here to show.
//
// my_extract_parameter_metadata does not walk value flow to build these paths.
// merge_access_type concatenates a caller-side prefix with a callee's whole
// summary, gated only by the type check and the two budgets; nothing checks
// that the callee's argument actually points into the prefix's points-to set.
// The heap load it would have to cross is therefore never in its way, which is
// how the loop `left = left->next[lvl]` gets unrolled into nested struct fields
// (.1.3 -> .1.3.2 -> .1.3.2.2) and how node->parent re-enters the same list a
// second time at .1.3.4 / .1.3.2.2.4.
//
// On c-ares this is not a corner case. Over the whole conditions.json, counting
// paths whose type ends in `**` and asking whether any of them is extended by a
// struct field: the Jul-16 output has 0 of 119, the current output has 319 of
// 908, and 1177 of 5483 access types sit exactly at MAX_FIELD_DEPTH.
// ---------------------------------------------------------------------------

// Same fork protocol as run_param_metadata_check, but writes the SVFG, ICFG and
// call graph as .dot next to the test binary, prints their paths, and hands the
// predicate both the top-down and the bottom-up summary of the same formal.
//
// NOTE on the top-down number: extractParameterMetadata is printed for
// comparison only, never asserted on. In the current tree it no longer
// reproduces what the Jul-16 pipeline produced - it returns just the flat `.`
// read/write even for test_meta.bc, where it used to report .0/.1/.2 - so it is
// diagnostic output, not a baseline.
static void run_access_path_probe(
    const std::string &bitcode_filename, const std::string &function,
    unsigned param_index, const std::string &dump_stem,
    const std::function<bool(const liberator::ValueMetadata &top_down,
                             const liberator::ValueMetadata &bottom_up)>
        &pred) {
  setenv("LIBFUZZ_LOG_PATH", "/tmp/", 1);
  config_t::instance()->consider_indirect_calls = false;

  std::string file_path =
      std::string(BINARY_DIR) + "/assets/" + bitcode_filename;
  if (!fs::exists(file_path)) {
    file_path = std::string(ASSETS_DIR) + "/" + bitcode_filename;
  }
  REQUIRE(fs::exists(file_path));

  std::vector<std::string> modules = {file_path};
  std::set<std::string> functions = {function};

  bool no_fork = getenv("LIBERATOR_TEST_NO_FORK") != nullptr;
  pid_t pid = no_fork ? 0 : fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    auto fail_child = [&](const char *msg) {
      std::cerr << "[access-path probe setup error] " << msg << std::endl;
      if (no_fork)
        FAIL(msg);
      else
        _exit(1);
    };

    auto extractor = liberator::make_condition_extractor(modules, functions);
    if (!extractor)
      return fail_child("extractor is null");

    auto *pag = SVF::SVFIR::getPAG();
    auto *svfg = extractor->get_svfg();
    auto *llvm_module_set = SVF::LLVMModuleSet::getLLVMModuleSet();

    extractor->extract_function_conditions();

    const SVF::FunObjVar *svf_fun = find_fun_by_demangled_name(pag, function);
    if (!svf_fun)
      return fail_child("function not found");

    auto params = pag->getFunArgsMap()[svf_fun];
    if (param_index >= params.size())
      return fail_child("parameter index out of range");

    auto *param = params[param_index];
    auto *param_llvm = llvm_module_set->getLLVMValue(param);
    if (!param_llvm || !param_llvm->getType()->isPointerTy())
      return fail_child("parameter is not a pointer");

    // Graph dumps; SVF appends ".dot" itself.
    // fs::path dump_dir = fs::path(BINARY_DIR) / "graphs";
    fs::path dump_dir = "/mnt/c/Users/MaschPaul/Downloads/";
    fs::create_directories(dump_dir);
    std::string svfg_dot = (dump_dir / (dump_stem + ".svfg")).string();
    std::string icfg_dot = (dump_dir / (dump_stem + ".icfg")).string();
    std::string cg_dot = (dump_dir / (dump_stem + ".callgraph")).string();
    svfg->dump(svfg_dot);
    pag->getICFG()->dump(icfg_dot);
    const_cast<SVF::CallGraph *>(pag->getCallGraph())->dump(cg_dot);
    std::cout << "\n[graphs] SVFG       -> " << svfg_dot << ".dot\n"
              << "[graphs] ICFG       -> " << icfg_dot << ".dot\n"
              << "[graphs] call graph -> " << cg_dot << ".dot\n"
              << "[graphs] render: dot -Tsvg " << svfg_dot
              << ".dot -o svfg.svg\n"
              << std::endl;

    std::string sys_cmd1 =
        "dot -Tpng " + svfg_dot + ".dot -o " + svfg_dot + ".png";
    std::string sys_cmd2 =
        "dot -Tpng " + icfg_dot + ".dot -o " + icfg_dot + ".png";
    int sys_res = system(sys_cmd1.c_str());
    sys_res = system(sys_cmd2.c_str());
    (void)sys_res; // suppress unused warning
    // extractParameterMetadata wants the source-level pointee type; under
    // opaque pointers the formal's LLVM type is just `ptr`, so recover it from
    // DWARF the way the pre-bottom-up pipeline did.
    const llvm::Type *seek_type = param_llvm->getType();
    if (llvm::DIType *di = liberator::restore_param_di_type(param_llvm)) {
      if (llvm::Type *resolved = liberator::resolve_di_type_to_llvm(
              di, *llvm_module_set->getMainLLVMModule())) {
        if (!resolved->isVoidTy())
          seek_type = resolved;
      }
    }

    auto top_down = liberator::extractParameterMetadata(
        *svfg, param_llvm, seek_type, param->getId());
    auto bottom_up = liberator::my_extract_parameter_metadata(*svfg, param_llvm,
                                                              param->getId());

    std::cout << "[top-down  extractParameterMetadata] (diagnostic only) "
              << function << " param " << param_index << ":\n"
              << liberator::print_summary(top_down, true) << "\n"
              << "[bottom-up my_extract_parameter_metadata] " << function
              << " param " << param_index << ":\n"
              << liberator::print_summary(bottom_up, true) << std::endl;

    bool ok = pred(top_down, bottom_up);
    if (no_fork) {
      CHECK(ok);
      return;
    }
    std::cout.flush();
    _exit(ok ? 0 : 2);
  }

  int status;
  waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status));
  INFO("child exit status = " << WEXITSTATUS(status)
                              << " (1 = setup error, 2 = predicate false)");
  CHECK(WEXITSTATUS(status) == 0);
}

// True if any access type in `m` has exactly this field path, any kind.
static bool metadata_has_path(const liberator::ValueMetadata &m,
                              const std::vector<int> &fields) {
  for (const auto &at : m.get_access_type_set()) {
    if (at.get_fields() == fields)
      return true;
  }
  return false;
}

// Records the step that no value-flow edge supports: the summary reaches
// `c->servers->head` (.1.3, `struct node **`) and then keeps going to
// `head[lvl]->next` (.1.3.2), which requires loading a pointer out of the heap
// array. Passing means composition crossed a memory boundary on type shape
// alone. This is the entry point for the [!shouldfail] case below - if this
// ever stops passing, check whether composition became points-to gated before
// concluding the test is stale.
TEST_CASE("summary composition crosses a heap array of pointers",
          "[unit][pathexplosion][svfg]") {
  run_access_path_probe("slist_path_explosion.bc", "channel_walk", 0,
                        "slist_path_explosion",
                        [](const liberator::ValueMetadata &,
                           const liberator::ValueMetadata &bottom_up) {
                          return metadata_has_path(bottom_up, {1, 3}) &&
                                 metadata_has_path(bottom_up, {1, 3, 2});
                        });
}

// The defect itself. `left = left->next[lvl]` is a loop over distinct node
// objects, but an access path has no way to say "the same field again", so one
// `.2` is appended per unrolled iteration until MAX_GEP_RECURSION_DEPTH stops
// it; and node->parent closes a cycle back onto the list, so the whole list
// subtree is appended a second time without ever leaving one allocation. None
// of these paths is a real access - they are the shape of the type graph, not
// of the program.
//
// Tagged [!shouldfail]: the assertions state the CORRECT behaviour, so the case
// is expected to fail today and the suite stays green. Once composition is
// gated on the callee argument actually aliasing the prefix, Catch2 will report
// this as unexpectedly passing - that is the signal to drop the tag.
TEST_CASE("bottom-up composition unrolls a list traversal into nested fields",
          "[unit][pathexplosion][svfg][!shouldfail]") {
  run_access_path_probe("slist_path_explosion.bc", "channel_walk", 0,
                        "slist_path_explosion",
                        [](const liberator::ValueMetadata &,
                           const liberator::ValueMetadata &bottom_up) {
                          // .1.3.2     head[i]->next            - one unrolled
                          // loop iteration .1.3.2.2   head[i]->next[j]->next -
                          // two .1.3.4     head[i]->parent          - the back
                          // edge, a struct list * .1.3.2.2.4 the back edge
                          // after two unrolled iterations
                          return !metadata_has_path(bottom_up, {1, 3, 2}) &&
                                 !metadata_has_path(bottom_up, {1, 3, 2, 2}) &&
                                 !metadata_has_path(bottom_up, {1, 3, 4}) &&
                                 !metadata_has_path(bottom_up, {1, 3, 2, 2, 4});
                        });
}
