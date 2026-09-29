#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <limits.h>
#include <limits>
#include <map>
#include <sstream>
#include <string>

#define PROFILE_CONCATINNER(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCATINNER(a, b)

#if defined(NDEBUG) && defined(PROFILING)
#define PROFILE_SCOPED(name)                                                   \
  liberator::scoped_timer_t PROFILE_CONCAT(_timer_, __LINE__)(name)
#define PROFILE_MEM(name)                                                      \
  liberator::scoped_mem_tracker_t PROFILE_CONCAT(_memtracker_, __LINE__)(name)
#else
#define PROFILE_SCOPED(name)
#define PROFILE_MEM(name)
#endif

namespace liberator {
class profiler_t {
public:
  static profiler_t &instance() {
    static profiler_t instance;
    return instance;
  }

  void record_mem(const std::string &key, long delta_kb, long peak_kb) {
    auto &m = mem_samples_[key];
    m.count++;
    m.total_delta_kb += delta_kb;
    m.max_delta_kb = std::max(m.max_delta_kb, delta_kb);
    m.max_peak_kb = std::max(m.max_peak_kb, peak_kb);
  }

  void record(const std::string &key, double duration_ms) {
    auto &d = samples_[key];
    d.count++;
    d.total_ms += duration_ms;
    d.min_ms = std::min(d.min_ms, duration_ms);
    d.max_ms = std::max(d.max_ms, duration_ms);
  }

  void clear() { samples_.clear(); }

  bool empty() const { return samples_.empty(); }

  void write_csv(const std::string &path, const std::string &target,
                 const std::string &label) {
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
      std::cerr << "[PROF] cannot open CSV file: " << path << "\n";
      return;
    }
    if (out.tellp() == 0)
      out << "timestamp,target,label,key,count,total_ms,avg_ms,min_ms,max_ms"
          << "\n";

    auto now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S",
                  std::localtime(&now));

    out << std::fixed << std::setprecision(3);
    for (const auto &[key, data] : samples_) {
      out << timestamp << ',' << csv_quote(target) << ',' << csv_quote(label)
          << ',' << csv_quote(key) << ',' << data.count << ',' << data.total_ms
          << ',' << (data.total_ms / data.count) << ',' << data.min_ms << ','
          << data.max_ms << '\n';
    }
  }

  std::string dump_mem() const {
    std::stringstream ss;
    ss << "Memory Stats (MB):\n";
    ss << std::left << std::setw(40) << "Key" << std::right << std::setw(12)
       << "AvgDelta" << std::setw(12) << "MaxDelta" << std::setw(12)
       << "MaxPeak" << std::setw(8) << "Count" << "\n";

    for (const auto &[key, data] : mem_samples_) {
      ss << std::left << std::setw(40) << key << std::right << std::fixed
         << std::setprecision(1) << std::setw(12)
         << (data.total_delta_kb / 1024.0 / data.count) << std::setw(12)
         << (data.max_delta_kb / 1024.0) << std::setw(12)
         << (data.max_peak_kb / 1024.0) << std::setw(8) << data.count << "\n";
    }

    return ss.str();
  }

  std::string dump() const {
    std::stringstream ss;
    ss << "Profiler Stats:\n";
    ss << std::left << std::setw(40) << "Key" << std::right << std::setw(10)
       << "Total(ms)" << std::setw(10) << "Count" << std::setw(10) << "Avg(ms)"
       << "\n";
    ss << std::string(70, '-') << "\n";

    for (const auto &[key, data] : samples_) {
      ss << std::left << std::setw(40) << key << std::right << std::fixed
         << std::setprecision(2) << std::setw(10) << data.total_ms
         << std::setw(10) << data.count << std::setw(10)
         << (data.total_ms / data.count) << "\n";
    }

    return ss.str();
  }

private:
  // replace an " with an \"
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

  struct sample_result_t {
    // how many samples
    long long count = 0;
    // total duration
    double total_ms = 0.0;
    // minimum and maximum duration
    double min_ms = std::numeric_limits<double>::infinity();
    double max_ms = 0.0;
  };

  struct mem_sample_result_t {
    long long count = 0;
    long total_delta_kb = 0;
    long max_delta_kb = std::numeric_limits<long>::min();
    long max_peak_kb = 0;
  };

  // key -> sample
  std::map<std::string, sample_result_t> samples_;
  std::map<std::string, mem_sample_result_t> mem_samples_;
  profiler_t() = default;
};

class scoped_timer_t {
public:
  scoped_timer_t(std::string key)
      : key_(std::move(key)), start_(std::chrono::steady_clock::now()) {}

  ~scoped_timer_t() {
    auto end = std::chrono::steady_clock::now();
    double duration =
        std::chrono::duration<double, std::milli>(end - start_).count();
    profiler_t::instance().record(key_, duration);
  }

private:
  std::string key_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
};

class scoped_mem_tracker_t {
public:
  scoped_mem_tracker_t(std::string key) : key_(std::move(key)) {
    mem_sample_t m = read_mem();
    for (scoped_mem_tracker_t *t : active()) {
      t->peak_kb_ = std::max(t->peak_kb_, m.peak_kb);
    }
    reset_peak();
    start_rss_kb_ = m.rss_kb;
    peak_kb_ = m.rss_kb;
    active().push_back(this);
  }

  ~scoped_mem_tracker_t() {
    mem_sample_t m = read_mem();
    peak_kb_ = std::max(peak_kb_, m.peak_kb);
    active().pop_back();
    profiler_t::instance().record_mem(key_, m.rss_kb - start_rss_kb_, peak_kb_);
  }

private:
  struct mem_sample_t {
    long rss_kb = 0;
    long peak_kb = 0;
  };

  static std::vector<scoped_mem_tracker_t *> &active() {
    static std::vector<scoped_mem_tracker_t *> stack;
    return stack;
  }

  static mem_sample_t read_mem() {
    mem_sample_t m;
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
      if (line.rfind("VmRSS:", 0) == 0)
        m.rss_kb = std::stol(line.substr(6));
      else if (line.rfind("VmHWM:", 0) == 0)
        m.peak_kb = std::stol(line.substr(6));
    }

    return m;
  }

  static void reset_peak() { std::ofstream("/proc/self/clear_refs") << "5"; }

  std::string key_;
  long start_rss_kb_ = 0;
  long peak_kb_ = 0;
};
} // namespace liberator
