#pragma once

#include <string>
#include <set>

// std because stdout gives conflict
enum OutType { txt, json, stdo };
enum Verbosity { v0, v1, v2, v3 };

#define ASSETS_DIR "/home/mashmallow/source/liberator/condition_extractor/tests/assets"

struct config_t {
  // Function to analyze
  std::string function;
  Verbosity verbose;
  bool debug;
  std::set<std::string> log_tags;
  std::string debug_condition;
  bool consider_indirect_calls;
  OutType output_type;
  std::string output_file;
  std::string minimize_api;
  std::string input_filename;
  std::string cache_folder;
  std::string extract_data_layout;
  bool print_dominator;
  bool use_dominator;
  /**
   * File that contains the extracted function names
   */
  std::string interface_file;
  bool dump_svfg;

  static inline config_t *instance() {
    static config_t instance;
    return &instance;
  }
};
