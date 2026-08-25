#pragma once
#include "hotpath/core/cache.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(__APPLE__)
#  include <pthread.h>
#  include <sys/sysctl.h>
#else
#  include <sched.h>
#  include <unistd.h>
#endif

namespace hotpath {

#if defined(__APPLE__)
inline std::size_t query_size(const char* name, std::size_t fallback) noexcept {
  std::uint64_t v = 0;
  std::size_t len = sizeof(v);
  if (::sysctlbyname(name, &v, &len, nullptr, 0) == 0 && v != 0)
    return static_cast<std::size_t>(v);
  return fallback;
}
#else
// Linux exposes the same facts through sysfs and sysconf rather than sysctl.
inline std::size_t read_sysfs_size(const char* path, std::size_t fallback) noexcept {
  std::FILE* f = std::fopen(path, "r");
  if (!f) return fallback;
  unsigned long long v = 0;
  const int n = std::fscanf(f, "%llu", &v);
  std::fclose(f);
  return (n == 1 && v != 0) ? static_cast<std::size_t>(v) : fallback;
}
#endif

struct PlatformInfo {
  std::size_t cache_line;
  std::size_t page_size;
  std::size_t perf_cores;       // P-cores on Apple Silicon; all online cores on Linux
  std::size_t efficiency_cores; // 0 where the platform does not distinguish
  std::size_t l1d_bytes;
  std::size_t l2_bytes;

  static PlatformInfo query() noexcept {
#if defined(__APPLE__)
    return PlatformInfo{
        query_size("hw.cachelinesize", 64),
        query_size("hw.pagesize", 4096),
        query_size("hw.perflevel0.physicalcpu", query_size("hw.physicalcpu", 1)),
        query_size("hw.perflevel1.physicalcpu", 0),
        query_size("hw.l1dcachesize", 0),
        query_size("hw.l2cachesize", 0),
    };
#else
    const long line = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    return PlatformInfo{
        line > 0 ? static_cast<std::size_t>(line)
                 : read_sysfs_size("/sys/devices/system/cpu/cpu0/cache/index0/"
                                   "coherency_line_size", 64),
        static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)),
        static_cast<std::size_t>(::sysconf(_SC_NPROCESSORS_ONLN)),
        0,
        static_cast<std::size_t>(::sysconf(_SC_LEVEL1_DCACHE_SIZE) > 0
                                     ? ::sysconf(_SC_LEVEL1_DCACHE_SIZE) : 0),
        static_cast<std::size_t>(::sysconf(_SC_LEVEL2_CACHE_SIZE) > 0
                                     ? ::sysconf(_SC_LEVEL2_CACHE_SIZE) : 0),
    };
#endif
  }
};

// Padding to kCacheLine must cover a real line, or the structures this repo
// pads to avoid false sharing still share one.
//
// The check is >=, not ==. Over-padding wastes a little space and is harmless;
// under-padding is the actual bug. That distinction matters when the same
// binary can run on 64-byte x86 lines and 128-byte Apple Silicon ones, and
// under a VM whose reported line size may not match the host's.
[[nodiscard]] inline bool cache_line_covers_os() noexcept {
  return kCacheLine >= PlatformInfo::query().cache_line;
}
[[nodiscard]] inline bool cache_line_matches_os() noexcept {
  return kCacheLine == PlatformInfo::query().cache_line;
}

// Ask to run somewhere good. What that means differs fundamentally:
//
//   macOS  -- a QoS class, which *hints* the scheduler toward the P-core
//             cluster. THREAD_AFFINITY_POLICY is accepted and ignored on Apple
//             Silicon, so there is no way to actually pin.
//   Linux  -- sched_setaffinity, which actually binds the thread to a CPU.
//
// That difference is one of the reasons this repo publishes no tail latency on
// macOS: you cannot hold a core still enough to measure one.
inline bool request_performance_cores() noexcept {
#if defined(__APPLE__)
  return ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
  // Default to the highest-numbered online CPU: CPU 0 typically fields more
  // interrupt and kernel work than the rest.
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  if (n <= 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(n - 1), &set);
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
#endif
}

// Bind to one specific CPU. A no-op returning false on macOS, which cannot.
inline bool pin_to_core([[maybe_unused]] int cpu) noexcept {
#if defined(__APPLE__)
  return false;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
#endif
}

[[nodiscard]] inline const char* affinity_mechanism() noexcept {
#if defined(__APPLE__)
  return "QoS hint (Apple Silicon ignores thread affinity)";
#else
  return "sched_setaffinity (hard binding)";
#endif
}

} // namespace hotpath
