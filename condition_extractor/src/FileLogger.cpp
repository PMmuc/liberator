#include "FileLogger.h"
#include <Util/SVFUtil.h>
#include <cmath>
#include <cstring>
#include <expected>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

using namespace std;
namespace {
optional<string> getApiFileLog() {
  char *file_path_env = getenv("LIBFUZZ_LOG_PATH");
  if (file_path_env) {
    std::string ApiFilePath;
    auto size = strlen(file_path_env);
    return string(file_path_env);
  }
  return std::nullopt;
}
} // namespace
//
namespace liberator {
logger_manager_t *logger_manager_t::instance_ = nullptr;

expected<log_ctx_t *, log_error>
logger_manager_t::logger(const std::string &filename) {
  auto it = loggers_.find(filename);
  if (loggers_.find(filename) != loggers_.end())
    return it->second.get();

  auto logger = make_unique<log_ctx_t>();
  logger->fstream_ =
      fstream(output_path_ + filename, std::ios::out | std::ios::app);

  if (!logger->fstream_.is_open()) {
    return std::unexpected(log_error::file_open_failed);
  }

  auto result = loggers_.insert({filename, std::move(logger)});

  return result.first->second.get();
}

logger_manager_t::~logger_manager_t() noexcept {
  if (instance_) {
    delete instance_;
    instance_ = nullptr;
  }
}

logger_manager_t *logger_manager_t::instance() {
  if (instance_ == nullptr) {
    auto result = create();
    if (result) {
      instance_ = *result;
    } else {
      log_error err = result.error();
      if (err == log_error::invalid_config) {
        SVF::SVFUtil::errs() << "Missing LIBFUZZ_LOG_PATH\n";
      }
    }
  }

  return instance_;
}

std::expected<logger_manager_t *, log_error> logger_manager_t::create() {
  logger_manager_t *manager = new logger_manager_t;

  auto opt = getApiFileLog();
  if (!opt.has_value()) {
    // try to fallback to default log path
    manager->output_path_ = "default_app.log";
    return std::unexpected(log_error::invalid_config);
  } else {

    manager->output_path_ = opt.value();
  }

  return manager;
}
} // namespace liberator
