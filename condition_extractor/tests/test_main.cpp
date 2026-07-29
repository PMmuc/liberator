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

#include "AccessType.h"
#include "AccessTypeIO.h"
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
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

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
