#pragma once
#include "hotpath/core/cache.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hotpath::ipc {

// Cross-process market-data ring over file-backed shared memory.
//
// Deliberately NOT the same design as SpscRing. That one blocks the producer
// when the consumer falls behind, which is right for an internal pipeline and
// wrong for market data: you cannot apply backpressure to an exchange. Here the
// producer NEVER waits. It overwrites, and a consumer that falls behind detects
// that it was lapped and reports a gap rather than silently reading a torn or
// stale message. Losing data loudly beats stalling the feed handler.
//
// This is also the boundary that justifies a ring at all. The in-process
// pipeline measured 1.30-1.62x SLOWER than doing the work on one thread,
// because a ~15 ns hop bought nothing (docs/PERFORMANCE.md). A process boundary
// is different: it is not an optimisation you chose, it is a constraint you
// have, and shared memory is how you cross it without a copy through the kernel.
//
// Atomics are std::atomic_ref over plain integers in the mapping rather than
// std::atomic objects placement-new'd into it. Constructing an atomic in
// another process's mapping is the usual approach and is not actually
// well-defined; atomic_ref is, and it is lock-free for uint64 on every target
// this runs on (asserted below).

inline constexpr std::uint64_t kShmMagic = 0x484F5450'52494E47ull;  // "HOTPRING"
inline constexpr std::uint32_t kShmVersion = 1;

struct ShmHeader {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t slot_bytes;     // usable payload per slot
  std::uint64_t capacity;       // slots, power of two
  std::uint64_t slot_stride;    // bytes per slot including its control word
  // Subscribers announce themselves so a publisher can wait before streaming;
  // otherwise a late attacher simply misses the beginning and reports it as a
  // gap, which is correct but makes the demo uninformative.
  std::uint64_t reader_count;
  // Set to the final sequence + 1 when the publisher is done, so subscribers
  // know the difference between "nothing yet" and "nothing ever again".
  std::uint64_t eof_seq;
  std::uint64_t _reserved;
  // The producer's published count lives on its own cache line: consumers poll
  // it constantly and must not share a line with anything the producer writes
  // per-message.
  alignas(kCacheLine) std::uint64_t write_seq;
};
static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= 8);

// Per slot: a seqlock version word, the message sequence, then the payload.
//   version odd  -> a write is in progress, the payload is not readable
//   version even -> the payload and `seq` are consistent
struct SlotHeader {
  std::uint64_t version;
  std::uint64_t seq;
};

class ShmRing {
public:
  enum class Status { Ok, Empty, Gap };

  ShmRing() = default;
  ~ShmRing() { close(); }
  ShmRing(const ShmRing&) = delete;
  ShmRing& operator=(const ShmRing&) = delete;
  ShmRing(ShmRing&& o) noexcept { steal(o); }
  ShmRing& operator=(ShmRing&& o) noexcept { if (this != &o) { close(); steal(o); } return *this; }

  // Producer: create (or truncate) the backing file and lay out the ring.
  static ShmRing create(const std::string& path, std::uint64_t capacity_pow2,
                        std::uint32_t slot_bytes) {
    if (capacity_pow2 == 0 || (capacity_pow2 & (capacity_pow2 - 1)) != 0)
      throw std::invalid_argument("ShmRing capacity must be a power of two");

    const std::uint64_t stride = align_up(sizeof(SlotHeader) + slot_bytes, kCacheLine);
    const std::uint64_t bytes = sizeof(ShmHeader) + stride * capacity_pow2;

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::runtime_error("shm create failed: " + path);
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      ::close(fd); throw std::runtime_error("ftruncate failed: " + path);
    }
    ShmRing r;
    r.map(fd, bytes);
    // Zero first, then stamp the header last: a consumer that opens the file
    // mid-setup must not see a valid magic over an uninitialised layout.
    std::memset(r.base_, 0, bytes);
    ShmHeader* h = r.header();
    h->version = kShmVersion;
    h->slot_bytes = slot_bytes;
    h->capacity = capacity_pow2;
    h->slot_stride = stride;
    std::atomic_ref<std::uint64_t>(h->magic).store(kShmMagic, std::memory_order_release);
    r.finish_open();
    return r;
  }

  // Consumer: attach to an existing ring.
  static ShmRing open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) throw std::runtime_error("shm open failed: " + path);
    struct ::stat st{};
    if (::fstat(fd, &st) != 0 || static_cast<std::uint64_t>(st.st_size) < sizeof(ShmHeader)) {
      ::close(fd); throw std::runtime_error("shm too small: " + path);
    }
    ShmRing r;
    r.map(fd, static_cast<std::uint64_t>(st.st_size));
    // atomic_ref requires a non-const T; the mapping is writable either way,
    // the constness was only on the accessor.
    ShmHeader* h = r.header();
    if (std::atomic_ref<std::uint64_t>(h->magic).load(std::memory_order_acquire) != kShmMagic)
      throw std::runtime_error("not a hotpath ring: " + path);
    if (h->version != kShmVersion)
      throw std::runtime_error("ring version mismatch: " + path);
    r.finish_open();
    return r;
  }

  // ---- producer ----
  //
  // Never blocks and never fails for lack of space. If the consumer has not
  // kept up, its message is overwritten and it will see a gap.
  void publish(const void* data, std::uint32_t n) noexcept {
    ShmHeader* h = header();
    const std::uint64_t seq = h->write_seq;          // producer-private
    SlotHeader* s = slot(seq & mask_);
    std::atomic_ref<std::uint64_t> ver(s->version);

    const std::uint64_t v = ver.load(std::memory_order_relaxed);
    ver.store(v + 1, std::memory_order_release);     // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    s->seq = seq;
    std::memcpy(payload(s), data, n < h->slot_bytes ? n : h->slot_bytes);

    ver.store(v + 2, std::memory_order_release);     // even: consistent again
    std::atomic_ref<std::uint64_t>(h->write_seq).store(seq + 1, std::memory_order_release);
  }

  // ---- consumer ----
  //
  // `next` is the sequence this consumer wants. On Gap it is advanced to the
  // oldest sequence still present and `gap` reports how many were missed.
  Status try_read(std::uint64_t& next, void* out, std::uint32_t n,
                  std::uint64_t& gap) const noexcept {
    ShmHeader* h = header();
    const std::uint64_t published =
        std::atomic_ref<std::uint64_t>(h->write_seq).load(std::memory_order_acquire);
    if (next >= published) return Status::Empty;

    // Anything older than published - capacity has certainly been overwritten.
    if (published > capacity_ && next < published - capacity_) {
      gap = (published - capacity_) - next;
      next = published - capacity_;
      return Status::Gap;
    }

    SlotHeader* s = slot(next & mask_);
    std::atomic_ref<std::uint64_t> ver(s->version);

    for (int attempt = 0; attempt < 64; ++attempt) {
      const std::uint64_t v1 = ver.load(std::memory_order_acquire);
      if (v1 & 1u) continue;                       // producer is mid-write
      const std::uint64_t seq = s->seq;
      std::memcpy(out, payload(s), n < h->slot_bytes ? n : h->slot_bytes);
      std::atomic_thread_fence(std::memory_order_acquire);
      const std::uint64_t v2 = ver.load(std::memory_order_acquire);
      if (v1 != v2) continue;                      // overwritten while we read

      if (seq == next) { ++next; return Status::Ok; }
      if (seq > next) {                            // lapped
        gap = seq - next;
        next = seq;
        return Status::Gap;
      }
      return Status::Empty;                        // not published into yet
    }
    // Persistently torn means the producer is lapping us faster than we can
    // read a single slot. That is a gap, not a retry.
    gap = 1;
    ++next;
    return Status::Gap;
  }

  [[nodiscard]] std::uint64_t published() const noexcept {
    return std::atomic_ref<std::uint64_t>(header()->write_seq)
        .load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

  std::uint64_t attach_reader() noexcept {
    return std::atomic_ref<std::uint64_t>(header()->reader_count)
        .fetch_add(1, std::memory_order_acq_rel) + 1;
  }
  [[nodiscard]] std::uint64_t readers() const noexcept {
    return std::atomic_ref<std::uint64_t>(header()->reader_count)
        .load(std::memory_order_acquire);
  }
  void mark_eof() noexcept {
    std::atomic_ref<std::uint64_t>(header()->eof_seq)
        .store(published() + 1, std::memory_order_release);
  }
  // 0 while the publisher is still running.
  [[nodiscard]] std::uint64_t eof() const noexcept {
    return std::atomic_ref<std::uint64_t>(header()->eof_seq)
        .load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint32_t slot_bytes() const noexcept { return header()->slot_bytes; }
  [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }

  void close() noexcept {
    if (base_) ::munmap(base_, bytes_);
    if (fd_ >= 0) ::close(fd_);
    base_ = nullptr; bytes_ = 0; fd_ = -1;
  }

private:
  static std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept {
    return (v + a - 1) / a * a;
  }
  void map(int fd, std::uint64_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); throw std::runtime_error("shm mmap failed"); }
    base_ = static_cast<std::uint8_t*>(p);
    bytes_ = bytes;
    fd_ = fd;
  }
  void finish_open() noexcept {
    capacity_ = header()->capacity;
    mask_ = capacity_ - 1;
    stride_ = header()->slot_stride;
  }
  void steal(ShmRing& o) noexcept {
    base_ = o.base_; bytes_ = o.bytes_; fd_ = o.fd_;
    capacity_ = o.capacity_; mask_ = o.mask_; stride_ = o.stride_;
    o.base_ = nullptr; o.bytes_ = 0; o.fd_ = -1;
  }
  [[nodiscard]] ShmHeader* header() const noexcept {
    return reinterpret_cast<ShmHeader*>(base_);
  }
  [[nodiscard]] SlotHeader* slot(std::uint64_t i) const noexcept {
    return reinterpret_cast<SlotHeader*>(base_ + sizeof(ShmHeader) + i * stride_);
  }
  static std::uint8_t* payload(SlotHeader* s) noexcept {
    return reinterpret_cast<std::uint8_t*>(s) + sizeof(SlotHeader);
  }

  std::uint8_t* base_{nullptr};
  std::uint64_t bytes_{0};
  int fd_{-1};
  std::uint64_t capacity_{0}, mask_{0}, stride_{0};
};

} // namespace hotpath::ipc
