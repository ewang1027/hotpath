#pragma once
#include "hotpath/book/events.hpp"
#include "hotpath/itch/mapped_file.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hotpath::book {

// mmap'd view of a per-symbol event tape. The events are a flat array right
// after the header, so replaying is a linear walk with no decoding at all --
// which is the point: the benchmark should measure the book, not a parser.
class Tape {
public:
  explicit Tape(const std::string& path) : file_(path) {
    if (file_.size() < sizeof(TapeHeader)) throw std::runtime_error("tape too small: " + path);
    std::memcpy(&header_, file_.data(), sizeof header_);
    if (std::memcmp(header_.magic, kTapeMagic, sizeof kTapeMagic) != 0)
      throw std::runtime_error("not a tape: " + path);
    if (header_.version != kTapeVersion)
      throw std::runtime_error("tape version mismatch: " + path);
    if (header_.event_size != sizeof(BookEvent))
      throw std::runtime_error("BookEvent layout changed since this tape was written: " + path);
    events_ = reinterpret_cast<const BookEvent*>(file_.data() + sizeof(TapeHeader));
    const std::size_t avail = (file_.size() - sizeof(TapeHeader)) / sizeof(BookEvent);
    count_ = std::min<std::size_t>(header_.event_count, avail);
  }

  [[nodiscard]] const BookEvent* events() const noexcept { return events_; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] const TapeHeader& header() const noexcept { return header_; }

  // Price window for the flat design, derived from the tape's own add prices.
  // Real books contain resting orders four orders of magnitude away from the
  // touch, so the window is set by quantile rather than by min/max -- min/max
  // would demand an 8 GB array.
  struct Window { Price lo; Price hi; double covered; };

  [[nodiscard]] Window price_window(double q_lo = 0.005, double q_hi = 0.995) const {
    std::vector<Price> px;
    px.reserve(count_ / 2);
    for (std::size_t i = 0; i < count_; ++i) {
      const BookEvent& e = events_[i];
      if ((e.type == EventType::Add || e.type == EventType::Replace) && e.price != 0)
        px.push_back(e.price);
    }
    if (px.empty()) return Window{0, FlatTickAlign(1), 0.0};
    std::sort(px.begin(), px.end());
    const Price lo = px[static_cast<std::size_t>(q_lo * static_cast<double>(px.size() - 1))];
    const Price hi = px[static_cast<std::size_t>(q_hi * static_cast<double>(px.size() - 1))];
    std::size_t inside = 0;
    for (Price p : px) if (p >= lo && p <= hi) ++inside;
    return Window{lo, hi, static_cast<double>(inside) / static_cast<double>(px.size())};
  }

private:
  static constexpr Price FlatTickAlign(Price p) noexcept { return p; }
  itch::MappedFile file_;
  TapeHeader header_{};
  const BookEvent* events_{nullptr};
  std::size_t count_{0};
};

} // namespace hotpath::book
