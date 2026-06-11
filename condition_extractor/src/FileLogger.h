#pragma once

#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

namespace liberator {

enum class log_error { file_open_failed, invalid_config };

template <typename... TArgs>
void println(std::ostream &os, std::format_string<TArgs...> fmt,
             TArgs &&...args) {
  os << std::format(fmt, std::forward<TArgs>(args)...);
}

class log_ctx_t {
public:
  template <typename... TArgs>
  void info(std::format_string<TArgs...> fmt, TArgs &&...args) {
    std::string msg = std::format(fmt, std::forward<TArgs>(args)...);

    fstream_ << std::format("[INFO] {}\n", msg);
  }

private:
  std::fstream fstream_;
  friend class logger_manager_t;
};

class logger_manager_t {
public:
  ~logger_manager_t() noexcept;
  std::expected<log_ctx_t *, log_error> logger(const std::string &filename);

  static logger_manager_t *instance();

private:
  static logger_manager_t *instance_;
  static std::expected<logger_manager_t *, log_error> create();
  std::unordered_map<std::string, std::unique_ptr<log_ctx_t>> loggers_;
  std::string output_path_;
};
} // namespace liberator
