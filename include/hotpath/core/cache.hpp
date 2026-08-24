#pragma once
#include <cstddef>

namespace hotpath {

// Cache line size is NOT 64 everywhere, and getting this wrong silently
// reintroduces false sharing on exactly the structures we pad to avoid it.
//
// Verified on this machine (`sysctl hw.cachelinesize`):
//   Apple M5 Pro (arm64) -> 128
//   typical x86-64       -> 64
//
// libc++ does not reliably expose std::hardware_destructive_interference_size,
// so we define it ourselves and assert against the OS at startup
// (see hotpath::verify_cache_line_size() in platform.hpp).
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#elif defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 64;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Pad a member out to a full cache line so two atomics never share one.
#define HOTPATH_CACHE_ALIGNED alignas(::hotpath::kCacheLine)

} // namespace hotpath
