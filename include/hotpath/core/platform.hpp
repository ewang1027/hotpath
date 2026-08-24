#pragma once
#include "hotpath/core/cache.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/sysctl.h>
#include <pthread.h>

namespace hotpath {

inline std::size_t sysctl_size(const char* name, std::size_t fallback) noexcept {
  std::uint64_t v = 0;
  std::size_t len = sizeof(v);
  if (::sysctlbyname(name, &v, &len, nullptr, 0) == 0 && v != 0) {
    return static_cast<std::size_t>(v);
  }
  return fallback;
}

struct PlatformInfo {
  std::size_t cache_line;
  std::size_t page_size;
  std::size_t perf_cores;      // P-cores  (perflevel0 on Apple Silicon)
  std::size_t efficiency_cores;// E-cores  (perflevel1)
  std::size_t l1d_bytes;
  std::size_t l2_bytes;

  static PlatformInfo query() noexcept {
    return PlatformInfo{
        sysctl_size("hw.cachelinesize", 64),
        sysctl_size("hw.pagesize", 4096),
        sysctl_size("hw.perflevel0.physicalcpu", sysctl_size("hw.physicalcpu", 1)),
        sysctl_size("hw.perflevel1.physicalcpu", 0),
        sysctl_size("hw.l1dcachesize", 0),
        sysctl_size("hw.l2cachesize", 0),
    };
  }
};

// The compile-time kCacheLine must match what the OS reports, or every
// alignas() in the ring buffer is padding to the wrong boundary and the
// false-sharing experiment silently measures nothing.
[[nodiscard]] inline bool cache_line_matches_os() noexcept {
  return PlatformInfo::query().cache_line == kCacheLine;
}

// macOS gives no CPU affinity on Apple Silicon -- THREAD_AFFINITY_POLICY is
// accepted and ignored. The only lever is QoS class, which steers the thread
// to the P-core cluster. This is strictly weaker than isolcpus/taskset: it is
// a scheduling hint, not a guarantee, and it is one of the reasons this repo
// does not claim tail latency. See docs/METHODOLOGY.md.
inline bool request_performance_cores() noexcept {
  return ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
}

} // namespace hotpath
