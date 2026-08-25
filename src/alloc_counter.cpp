#include "hotpath/bench/counters.hpp"

#include <atomic>
#include <cstdlib>
#include <stdlib.h>
#include <new>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

// Relaxed is correct here: these are monotonic tallies read after the measured
// region has been joined. We are counting events, not establishing ordering,
// and we do not want a fence in the hot path just to observe it.
std::atomic<std::uint64_t> g_allocs{0};
std::atomic<std::uint64_t> g_deallocs{0};
std::atomic<std::uint64_t> g_bytes{0};
std::atomic<std::uint64_t> g_syscalls{0};

inline void bump_alloc(std::size_t n) noexcept {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(n, std::memory_order_relaxed);
}
inline void bump_free() noexcept {
  g_deallocs.fetch_add(1, std::memory_order_relaxed);
}
inline void bump_syscall() noexcept {
  g_syscalls.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

// ---------------------------------------------------------------------------
// Global allocation operators.
// ---------------------------------------------------------------------------
void* operator new(std::size_t n) {
  bump_alloc(n);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  bump_alloc(n);
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
  return ::operator new(n, t);
}
void operator delete(void* p) noexcept { if (p) bump_free(); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

// Aligned variants (C++17). Missing these means an over-aligned type -- and
// every cache-line-padded structure in this repo is over-aligned -- allocates
// through a path the counter never sees.
void* operator new(std::size_t n, std::align_val_t a) {
  bump_alloc(n);
  // NOT std::aligned_alloc: it requires the size to be a multiple of the
  // alignment, but C++17's aligned operator new must accept any size. A
  // 16-byte allocation at 128-byte alignment -- e.g. a small cache-line-padded
  // ring -- makes aligned_alloc return null and turns into a spurious
  // bad_alloc. posix_memalign has no such restriction.
  void* p = nullptr;
  if (::posix_memalign(&p, static_cast<std::size_t>(a), n ? n : 1) != 0) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { if (p) bump_free(); std::free(p); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }

// ---------------------------------------------------------------------------
// Syscall counting.
//
// The mechanism is completely different on the two platforms, which is most of
// why this file needs a porting layer at all.
//
// macOS has no LD_PRELOAD, but dyld honours a __DATA,__interpose section that
// swaps a libSystem symbol for ours process-wide. It is silently ignored when
// the instrumentation is a static archive, which is why hotpath_instrument is
// built SHARED (see docs/BUILDLOG.md).
//
// Linux is simpler: a definition in a shared object that precedes libc in the
// link order preempts the libc symbol for the whole process, with no special
// section. Reaching the real function then needs dlsym(RTLD_NEXT), which is the
// only extra machinery.
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
namespace {

struct Interpose { const void* replacement; const void* replacee; };
#define HOTPATH_INTERPOSE(rep, orig)                                          \
  __attribute__((used)) static const Interpose interpose_##orig               \
      __attribute__((section("__DATA,__interpose"))) =                        \
          {reinterpret_cast<const void*>(&rep),                               \
           reinterpret_cast<const void*>(&orig)};

ssize_t hp_read(int fd, void* buf, size_t n) { bump_syscall(); return ::read(fd, buf, n); }
ssize_t hp_write(int fd, const void* buf, size_t n) { bump_syscall(); return ::write(fd, buf, n); }
void*   hp_mmap(void* a, size_t l, int p, int f, int fd, off_t o) { bump_syscall(); return ::mmap(a, l, p, f, fd, o); }
int     hp_munmap(void* a, size_t l) { bump_syscall(); return ::munmap(a, l); }
int     hp_madvise(void* a, size_t l, int adv) { bump_syscall(); return ::madvise(a, l, adv); }
int     hp_close(int fd) { bump_syscall(); return ::close(fd); }

HOTPATH_INTERPOSE(hp_read, read)
HOTPATH_INTERPOSE(hp_write, write)
HOTPATH_INTERPOSE(hp_mmap, mmap)
HOTPATH_INTERPOSE(hp_munmap, munmap)
HOTPATH_INTERPOSE(hp_madvise, madvise)
HOTPATH_INTERPOSE(hp_close, close)

} // namespace

#else   // Linux

#include <dlfcn.h>

namespace {
// Resolved lazily rather than in a constructor: this object may be interposing
// calls made before its own static initialisers have run.
template <typename Fn>
inline Fn real(Fn& cache, const char* name) noexcept {
  if (!cache) cache = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
  return cache;
}
using read_fn    = ssize_t (*)(int, void*, size_t);
using write_fn   = ssize_t (*)(int, const void*, size_t);
using mmap_fn    = void*   (*)(void*, size_t, int, int, int, off_t);
using munmap_fn  = int     (*)(void*, size_t);
using madvise_fn = int     (*)(void*, size_t, int);
using close_fn   = int     (*)(int);
read_fn    g_read{};
write_fn   g_write{};
mmap_fn    g_mmap{};
munmap_fn  g_munmap{};
madvise_fn g_madvise{};
close_fn   g_close{};
} // namespace

extern "C" {
ssize_t read(int fd, void* buf, size_t n) { bump_syscall(); return real(g_read, "read")(fd, buf, n); }
ssize_t write(int fd, const void* buf, size_t n) { bump_syscall(); return real(g_write, "write")(fd, buf, n); }
void*   mmap(void* a, size_t l, int p, int f, int fd, off_t o) { bump_syscall(); return real(g_mmap, "mmap")(a, l, p, f, fd, o); }
int     munmap(void* a, size_t l) { bump_syscall(); return real(g_munmap, "munmap")(a, l); }
int     madvise(void* a, size_t l, int adv) { bump_syscall(); return real(g_madvise, "madvise")(a, l, adv); }
int     close(int fd) { bump_syscall(); return real(g_close, "close")(fd); }
}
#endif

namespace hotpath::bench {

Counters read_counters() noexcept {
  return Counters{
      g_allocs.load(std::memory_order_relaxed),
      g_deallocs.load(std::memory_order_relaxed),
      g_bytes.load(std::memory_order_relaxed),
      g_syscalls.load(std::memory_order_relaxed),
  };
}

} // namespace hotpath::bench
