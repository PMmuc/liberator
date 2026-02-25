#include "Profiler.hpp"
#include <ConditionExtractor.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json/json.h>

namespace fs = std::filesystem;

TEST_CASE("Condition Extraction on .ll files", "[integration]") {
  std::string assets_dir =
      "../../test/assets"; // Relative to execution directory

  // Ensure the assets directory exists
  if (!fs::exists(assets_dir)) {
    // Fallback or attempt to locate if running from build dir
    if (fs::exists("../test/assets")) {
      assets_dir = "../test/assets";
    } else if (fs::exists("../../condition_extractor/test/assets")) {
      assets_dir = "../../condition_extractor/test/assets";
    } else if (fs::exists("test/assets")) {
       assets_dir = "test/assets";
    }
  }

  if (!fs::exists(assets_dir)) {
    FAIL("Test assets directory not found.");
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
    if (entry.path().extension() == ".ll") {
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

        // Dump Profiler stats
        std::cout << liberator::Profiler::instance().dump() << std::endl;
        liberator::Profiler::instance().clear();

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
