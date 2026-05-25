#pragma once

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

// Profile macro for release mode only
#define PROFILE_CONCAT_INNER(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_INNER(a, b)

#if defined(NDEBUG) && defined(PROFILING)
#define PROFILE_SCOPE(name)                                                    \
  liberator::ScopedTimer PROFILE_CONCAT(_timer_, __LINE__)(name)
#else
#define PROFILE_SCOPE(name)
#endif

namespace liberator {

class Profiler {
public:
  static Profiler &instance() {
    static Profiler instance;
    return instance;
  }

  void record(const std::string &key, double duration_ms) {
    stats_[key].count++;
    stats_[key].total_ms += duration_ms;
    stats_[key].min_ms = std::min(stats_[key].min_ms, duration_ms);
    stats_[key].max_ms = std::max(stats_[key].max_ms, duration_ms);
  }

  void clear() { stats_.clear(); }

  std::string dump() const {
    std::stringstream ss;
    ss << "Profiler Stats:\n";
    ss << std::left << std::setw(40) << "Key" << std::right << std::setw(10)
       << "Total(ms)" << std::setw(10) << "Count" << std::setw(10) << "Avg(ms)"
       << "\n";
    ss << std::string(70, '-') << "\n";

    for (const auto &[key, data] : stats_) {
      ss << std::left << std::setw(40) << key << std::right << std::fixed
         << std::setprecision(2) << std::setw(10) << data.total_ms
         << std::setw(10) << data.count << std::setw(10)
         << (data.total_ms / data.count) << "\n";
    }
    return ss.str();
  }

private:
  struct StatData {
    long long count = 0;
    double total_ms = 0.0;
    double min_ms = 1e9;
    double max_ms = 0.0;
  };

  std::map<std::string, StatData> stats_;
  Profiler() = default;
};

class ScopedTimer {
public:
  ScopedTimer(std::string key)
      : key_(std::move(key)),
        start_(std::chrono::high_resolution_clock::now()) {}

  ~ScopedTimer() {
    auto end = std::chrono::high_resolution_clock::now();
    double duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start_)
            .count() /
        1000.0;
    Profiler::instance().record(key_, duration);
  }

private:
  std::string key_;
  std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

} // namespace liberator
