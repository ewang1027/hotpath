#pragma once
#include "hotpath/itch/byteorder.hpp"
#include "hotpath/itch/messages.hpp"

#include <cstdint>

namespace hotpath::itch {

// Nasdaq's downloadable ITCH files are in "BinaryFILE" framing: each message
// is preceded by a 2-byte big-endian length. We advance by that prefix, never
// by a hardcoded per-type length, so an unrecognised message type cannot
// desynchronise the stream -- it is counted and skipped.
struct RawMessage {
  const std::uint8_t* body;
  std::uint16_t       length;
  [[nodiscard]] char type() const noexcept { return static_cast<char>(body[0]); }
};

// Diagnostics gathered during a pass. Cheap enough to always collect.
struct ReaderStats {
  std::uint64_t messages{0};
  std::uint64_t bytes{0};
  std::uint64_t unknown_type{0};      // type not in spec_length() table
  std::uint64_t length_mismatch{0};   // prefix disagreed with the spec table
  std::uint64_t truncated{0};         // file ended mid-message
  std::uint64_t per_type[256]{};
};

class Reader {
public:
  Reader(const std::uint8_t* data, std::size_t size) noexcept
      : cur_(data), end_(data + size) {}

  // Returns false at end of stream. Hot path: one bounds check, one 16-bit
  // byte-swapped load, one pointer bump. No allocation, no I/O, no branching
  // on message type -- that is the caller's job.
  [[nodiscard]] bool next(RawMessage& out) noexcept {
    if (__builtin_expect(cur_ + 2 > end_, 0)) return false;
    const std::uint16_t len = rd_u16(cur_);
    if (__builtin_expect(len == 0, 0)) return false;          // end-of-file marker
    if (__builtin_expect(cur_ + 2 + len > end_, 0)) {         // truncated tail
      ++stats_.truncated;
      cur_ = end_;
      return false;
    }
    out.body = cur_ + 2;
    out.length = len;
    cur_ += 2 + len;

    ++stats_.messages;
    stats_.bytes += 2u + len;
    const auto t = static_cast<std::uint8_t>(out.body[0]);
    ++stats_.per_type[t];
    const std::uint16_t expect = spec_length(static_cast<char>(t));
    if (__builtin_expect(expect == 0, 0)) ++stats_.unknown_type;
    else if (__builtin_expect(expect != len, 0)) ++stats_.length_mismatch;
    return true;
  }

  [[nodiscard]] const ReaderStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t bytes_remaining() const noexcept {
    return static_cast<std::size_t>(end_ - cur_);
  }

private:
  const std::uint8_t* cur_;
  const std::uint8_t* end_;
  ReaderStats stats_{};
};

} // namespace hotpath::itch
