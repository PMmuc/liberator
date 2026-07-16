#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
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

  bool empty() const { return stats_.empty(); }

  // Append one CSV row per key so repeated benchmark runs accumulate in a
  // single table. The header is written only when the file is new/empty.
  // `target` and `label` (e.g. the analysed library and the implementation
  // under test) are copied into every row to keep runs distinguishable.
  void write_csv(const std::string &path, const std::string &target,
                 const std::string &label) const {
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
      std::cerr << "[Profiler] cannot open CSV file: " << path << "\n";
      return;
    }
    if (out.tellp() == 0)
      out << "timestamp,target,label,key,count,total_ms,avg_ms,min_ms,max_ms"
          << "\n";

    auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S",
                  std::localtime(&now));

    out << std::fixed << std::setprecision(3);
    for (const auto &[key, data] : stats_) {
      out << timestamp << ',' << csv_quote(target) << ',' << csv_quote(label)
          << ',' << csv_quote(key) << ',' << data.count << ',' << data.total_ms
          << ',' << (data.total_ms / data.count) << ',' << data.min_ms << ','
          << data.max_ms << "\n";
    }
  }

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
  static std::string csv_quote(const std::string &s) {
    std::string quoted = "\"";
    for (char c : s) {
      if (c == '"')
        quoted += "\"\"";
      else
        quoted += c;
    }
    quoted += '"';
    return quoted;
  }

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
  // steady_clock: monotonic, unaffected by system clock adjustments —
  // high_resolution_clock may alias system_clock and jump during a run.
  ScopedTimer(std::string key)
      : key_(std::move(key)), start_(std::chrono::steady_clock::now()) {}

  ~ScopedTimer() {
    auto end = std::chrono::steady_clock::now();
    double duration =
        std::chrono::duration<double, std::milli>(end - start_).count();
    Profiler::instance().record(key_, duration);
  }

private:
  std::string key_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
};

} // namespace liberator
