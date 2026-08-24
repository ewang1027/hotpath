#pragma once
#include "hotpath/core/cache.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace hotpath::ipc {

// Memory ordering policy. The Relaxed variant is DELIBERATELY BROKEN and exists
// only so the litmus tests can demonstrate what it breaks. It must never be
// used for anything real.
//
// The point of having both behind one policy parameter is that the two variants
// are otherwise the same code -- so a litmus test that fails on one and passes
// on the other isolates the memory ordering and nothing else.
enum class Ordering { AcqRel, Relaxed };

// Padding policy, for the false-sharing experiment. Padded is correct; Packed
// puts the producer's and consumer's indices in the same cache line on purpose.
enum class Padding { Padded, Packed };

// Index-caching policy. Cached is correct and is what makes the steady-state
// path never touch the other side's atomic at all. Uncached re-reads the
// opposite index on every single operation -- the textbook shape, and the
// configuration in which false sharing is actually observable.
enum class Caching { Cached, Uncached };

// Single-producer / single-consumer lock-free ring buffer.
//
// Capacity must be a power of two so the modulo is a mask. One slot is left
// unused so full and empty are distinguishable without a separate count, which
// would be a third contended variable.
template <typename T, Ordering O = Ordering::AcqRel, Padding P = Padding::Padded,
          Caching C = Caching::Cached>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing publishes by plain store; T must be trivially copyable");

  static constexpr std::memory_order kPub =
      O == Ordering::AcqRel ? std::memory_order_release : std::memory_order_relaxed;
  static constexpr std::memory_order kObs =
      O == Ordering::AcqRel ? std::memory_order_acquire : std::memory_order_relaxed;

  // Padded: head and tail sit on their own cache lines, so the producer
  // advancing tail_ does not invalidate the line the consumer is spinning on.
  // Packed: both in one line -- every publish steals the line back from the
  // other core. That is the whole false-sharing experiment.
  static constexpr std::size_t kAlign = P == Padding::Padded ? kCacheLine : alignof(std::uint64_t);

public:
  explicit SpscRing(std::size_t capacity_pow2)
      : mask_(capacity_pow2 - 1),
        buf_(static_cast<T*>(::operator new[](capacity_pow2 * sizeof(T),
                                              std::align_val_t{kCacheLine}))) {
    // Zero the storage: the litmus tests rely on an unwritten slot being
    // distinguishable from a written one.
    for (std::size_t i = 0; i < capacity_pow2; ++i) new (buf_ + i) T{};
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  ~SpscRing() { ::operator delete[](buf_, std::align_val_t{kCacheLine}); }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // --- producer side ---
  [[nodiscard]] bool try_push(const T& v) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = (tail + 1) & mask_;
    if constexpr (C == Caching::Cached) {
      if (next == cached_head_) {                     // maybe full: re-read
        cached_head_ = head_.load(kObs);
        if (next == cached_head_) return false;
      }
    } else {
      if (next == head_.load(kObs)) return false;     // reads the consumer's line every time
    }
    buf_[tail] = v;
    // THE release. It orders the slot write above against the index update
    // below: a consumer that observes this new tail is guaranteed to see the
    // payload. Under Ordering::Relaxed this is a plain store and arm64 is free
    // to make the index visible first.
    tail_.store(next, kPub);
    return true;
  }

  // --- consumer side ---
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if constexpr (C == Caching::Cached) {
      if (head == cached_tail_) {                     // maybe empty: re-read
        cached_tail_ = tail_.load(kObs);
        if (head == cached_tail_) return false;
      }
    } else {
      if (head == tail_.load(kObs)) return false;     // reads the producer's line every time
    }
    out = buf_[head];
    head_.store((head + 1) & mask_, kPub);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_; }
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_relaxed);
    return (t - h) & mask_;
  }

private:
  std::size_t mask_;
  T* buf_;

  // cached_head_ is producer-private, cached_tail_ is consumer-private. They
  // exist so the common case never loads the other side's atomic at all: the
  // producer only re-reads head_ when it believes the ring is full. Without
  // them every push would touch a line the consumer owns.
  alignas(kAlign) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};
  alignas(kAlign) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};
};

} // namespace hotpath::ipc
