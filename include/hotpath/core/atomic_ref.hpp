#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <version>

// std::atomic_ref, for toolchains that do not have it yet.
//
// Why this exists rather than just using std::atomic_ref directly: ShmRing
// needs atomic operations on plain integers that live inside a shared-memory
// mapping. The usual approach -- placement-new a std::atomic into the mapping
// -- is not actually well-defined when a second process maps the same bytes,
// and atomic_ref is the C++20 facility that is (see the header comment in
// ipc/shm_ring.hpp). So the type is load-bearing; dropping it is not an option.
//
// But atomic_ref is one of the last C++20 library features to land. Apple's
// libc++ shipped it only recently: the GitHub macos-14 runner's AppleClang
// compiles every other C++20 construct in this repo and rejects this one name,
// which is what broke CI on all four macOS jobs while both Linux jobs stayed
// green. Pinning a newer runner image would fix today's failure and re-break
// whenever the image moves; feature detection does not.
//
// The fallback is NOT a reimplementation of atomics. __atomic_load_n and
// friends are the compiler builtins that std::atomic_ref itself lowers to on
// both clang and gcc, so for the orderings this repo uses the emitted
// instruction sequence is identical either way.

// HOTPATH_FORCE_ATOMIC_REF_FALLBACK exists so the fallback can be compiled on a
// machine whose libc++ *does* have atomic_ref. Without it the fallback is dead
// code everywhere it is not needed, which is precisely how a porting shim rots
// undetected -- the same reason CI builds Linux at all (docs/PORTING.md).
#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L && \
    !defined(HOTPATH_FORCE_ATOMIC_REF_FALLBACK)
#  define HOTPATH_ATOMIC_REF_IS_STD 1
#else
#  define HOTPATH_ATOMIC_REF_IS_STD 0
#endif

namespace hotpath {

#if HOTPATH_ATOMIC_REF_IS_STD

template <typename T>
using atomic_ref = std::atomic_ref<T>;

#else

namespace detail {
// std::memory_order and the __ATOMIC_* constants happen to share values on both
// clang and gcc, but that is an implementation detail and not something to bet
// a seqlock on. The switch costs nothing: every call site passes a constant.
constexpr int to_builtin_order(std::memory_order o) noexcept {
  switch (o) {
    case std::memory_order_relaxed: return __ATOMIC_RELAXED;
    case std::memory_order_consume: return __ATOMIC_CONSUME;
    case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
    case std::memory_order_release: return __ATOMIC_RELEASE;
    case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
    default:                        return __ATOMIC_SEQ_CST;
  }
}
} // namespace detail

// Only the subset of the atomic_ref interface this repo actually uses. A
// partial shim that fails to compile when someone reaches for a member it does
// not have is better than a complete one that is subtly wrong somewhere nobody
// looks.
template <typename T>
class atomic_ref {
  static_assert(std::is_trivially_copyable_v<T>,
                "atomic_ref requires a trivially copyable type");
  static_assert(__atomic_always_lock_free(sizeof(T), nullptr),
                "atomic_ref fallback requires a lock-free width: a libatomic "
                "call would take a lock, and a lock in one process's address "
                "space cannot make a shared mapping atomic for another");

public:
  using value_type = T;
  static constexpr std::size_t required_alignment = alignof(T);
  static constexpr bool is_always_lock_free = true;

  explicit atomic_ref(T& obj) noexcept : p_(&obj) {}
  atomic_ref(const atomic_ref&) noexcept = default;
  atomic_ref& operator=(const atomic_ref&) = delete;

  T load(std::memory_order o = std::memory_order_seq_cst) const noexcept {
    return __atomic_load_n(p_, detail::to_builtin_order(o));
  }
  void store(T v, std::memory_order o = std::memory_order_seq_cst) const noexcept {
    __atomic_store_n(p_, v, detail::to_builtin_order(o));
  }
  T exchange(T v, std::memory_order o = std::memory_order_seq_cst) const noexcept {
    return __atomic_exchange_n(p_, v, detail::to_builtin_order(o));
  }
  T fetch_add(T v, std::memory_order o = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_add(p_, v, detail::to_builtin_order(o));
  }
  T fetch_sub(T v, std::memory_order o = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_sub(p_, v, detail::to_builtin_order(o));
  }

private:
  T* p_;
};

#endif

} // namespace hotpath
