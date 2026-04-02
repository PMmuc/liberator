#pragma once

#include "FileLogger.h"
#include <format>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

// std because stdout gives conflict
enum OutType { txt, json, stdo };
enum Verbosity { v0, v1, v2, v3 };

struct config_t {
  // Function to analyze
  std::string function;
  Verbosity verbose;
  bool debug;
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

  std::unordered_set<std::string> log_tags;

  static inline config_t *instance() {
    static config_t instance;
    return &instance;
  }
};

#ifndef NDEBUG

struct StdoutLogger {};
struct FileLogger {};

template <typename TargetLogger, typename... Args>
void tag_log(const std::string &tag, std::format_string<Args...> fmt,
             Args &&...args) {
  if (::config_t::instance()->log_tags.find(tag) ==
      ::config_t::instance()->log_tags.end()) {
    return;
  }

  if constexpr (std::is_same_v<TargetLogger, StdoutLogger>) {
    std::cout << "[" << tag << "] "
              << std::format(fmt, std::forward<Args>(args)...) << "\n";
  } else if constexpr (std::is_same_v<TargetLogger, FileLogger>) {
    auto manager = liberator::logger_manager_t::instance();
    if (manager) {
      auto logger_res = manager->logger(tag + ".log");
      if (logger_res.has_value()) {
        (*logger_res)
            ->info("[{}] {}", tag,
                   std::format(fmt, std::forward<Args>(args)...));
      }
    }
  }
}

template <typename TargetLogger, typename... Args>
void tag_log(std::initializer_list<std::string_view> tags,
             std::format_string<Args...> fmt, Args &&...args) {
  bool formatted = false;
  std::string msg;

  for (std::string_view tag_view : tags) {
    std::string tag{tag_view};
    if (::config_t::instance()->log_tags.find(tag) !=
        ::config_t::instance()->log_tags.end()) {
      if (!formatted) {
        msg = std::format(fmt, std::forward<Args>(args)...);
        formatted = true;
      }

      if constexpr (std::is_same_v<TargetLogger, StdoutLogger>) {
        std::cout << "[" << tag << "] " << msg << "\n";
      } else if constexpr (std::is_same_v<TargetLogger, FileLogger>) {
        auto manager = liberator::logger_manager_t::instance();
        if (manager) {
          auto logger_res = manager->logger(tag + ".log");
          if (logger_res.has_value()) {
            (*logger_res)->info("[{}] {}", tag, msg);
          }
        }
      }
    }
  }
}

#define HANDLER_LOG(...) tag_log<StdoutLogger>("handler", __VA_ARGS__)
#define GEP_LOG(...) tag_log<StdoutLogger>("GEPHandler", __VA_ARGS__)
#define APARM_LOG(...) tag_log<StdoutLogger>("APARM", __VA_ARGS__)
#else

struct StdoutLogger {};
struct FileLogger {};

template <typename TargetLogger, typename... Args>
void tag_log(const std::string &tag, std::format_string<Args...> fmt,
             Args &&...args) {}

template <typename TargetLogger, typename... Args>
void tag_log(std::initializer_list<std::string_view> tags,
             std::format_string<Args...> fmt, Args &&...args) {}

#endif
