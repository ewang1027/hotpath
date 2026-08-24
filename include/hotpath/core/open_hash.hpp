#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace hotpath {

// Open-addressing hash map keyed on uint64 order reference numbers.
//
// Why not std::unordered_map: it is a chained hash, so every lookup is a
// pointer chase into a separately allocated node, and every insert is a
// malloc. On a stream where ~1 in 3 messages is an order lookup, that is both
// the dominant cache miss and a guaranteed violation of the zero-allocation
// invariant. This map allocates its whole table once, up front, and never
// grows -- so a lookup is a linear probe over contiguous memory and an insert
// allocates nothing.
//
// Deletion uses backward-shift rather than tombstones. ITCH deletes orders
// constantly (most resting orders are cancelled, not executed), and a
// tombstoned table degrades to a linear scan over a full trading day.
template <typename Value>
class OpenHashMap {
public:
  static constexpr std::uint64_t kEmpty = 0;  // ITCH never uses order ref 0

  struct Slot {
    std::uint64_t key;
    Value value;
  };

  // capacity_pow2 is the number of slots; keep load factor under ~0.7.
  explicit OpenHashMap(std::size_t capacity_pow2)
      : slots_(capacity_pow2), mask_(capacity_pow2 - 1) {
    if (capacity_pow2 == 0 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
      throw std::invalid_argument("OpenHashMap capacity must be a power of two");
    }
    for (auto& s : slots_) s.key = kEmpty;
  }

  static std::uint64_t hash(std::uint64_t x) noexcept {
    // Order reference numbers are near-sequential, which linear probing turns
    // into one long cluster. Mix before masking.
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }

  [[nodiscard]] Value* find(std::uint64_t key) noexcept {
    std::size_t i = hash(key) & mask_;
    while (true) {
      const std::uint64_t k = slots_[i].key;
      if (k == key) return &slots_[i].value;
      if (k == kEmpty) return nullptr;
      i = (i + 1) & mask_;
    }
  }

  [[nodiscard]] const Value* find(std::uint64_t key) const noexcept {
    return const_cast<OpenHashMap*>(this)->find(key);
  }

  // Returns nullptr if the table is full. No growth, by design.
  Value* insert(std::uint64_t key, const Value& v) noexcept {
    if (size_ + 1 > (slots_.size() * 7) / 10) return nullptr;
    std::size_t i = hash(key) & mask_;
    while (true) {
      const std::uint64_t k = slots_[i].key;
      if (k == kEmpty) {
        slots_[i].key = key;
        slots_[i].value = v;
        ++size_;
        return &slots_[i].value;
      }
      if (k == key) {            // replace in place
        slots_[i].value = v;
        return &slots_[i].value;
      }
      i = (i + 1) & mask_;
    }
  }

  bool erase(std::uint64_t key) noexcept {
    std::size_t i = hash(key) & mask_;
    while (true) {
      const std::uint64_t k = slots_[i].key;
      if (k == key) break;
      if (k == kEmpty) return false;
      i = (i + 1) & mask_;
    }
    // Backward-shift: walk forward closing the gap, moving any entry whose
    // ideal slot is not cyclically inside (i, j].
    std::size_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].key == kEmpty) break;
      const std::size_t k = hash(slots_[j].key) & mask_;
      if (i <= j) { if (i < k && k <= j) continue; }
      else        { if (i < k || k <= j) continue; }
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].key = kEmpty;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
  [[nodiscard]] double load_factor() const noexcept {
    return static_cast<double>(size_) / static_cast<double>(slots_.size());
  }
  void clear() noexcept {
    for (auto& s : slots_) s.key = kEmpty;
    size_ = 0;
  }

private:
  std::vector<Slot> slots_;
  std::size_t mask_;
  std::size_t size_{0};
};

} // namespace hotpath
