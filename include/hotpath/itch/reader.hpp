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
  std::uint64_t zero_length{0};       // 0-byte length prefix: stream stops here
  std::uint64_t short_message{0};     // body shorter than its type's spec length
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
    // A zero-length prefix terminates the stream. As the LAST two bytes of the
    // file that is a legitimate end-of-file marker; anywhere else it is
    // corruption, and treating it as a clean end lets a half-read file report
    // "no errors" having silently skipped everything after the zero.
    //
    // The distinction matters in both directions: without it a corrupt file
    // passes, and without the terminator case a perfectly good file that ends
    // with one fails the whole-file-consumed check by exactly two bytes.
    if (__builtin_expect(len == 0, 0)) {
      if (cur_ + 2 == end_) { cur_ = end_; stats_.bytes += 2; }  // clean terminator
      else ++stats_.zero_length;
      return false;
    }
    if (__builtin_expect(cur_ + 2 + len > end_, 0)) {         // truncated tail
      ++stats_.truncated;
      cur_ = end_;
      return false;
    }
    const std::uint8_t* body = cur_ + 2;
    cur_ += 2 + len;
    stats_.bytes += 2u + len;

    const auto t = static_cast<std::uint8_t>(body[0]);

    // Every ITCH message carries the 11-byte common header, so anything shorter
    // is malformed whatever its type claims to be. This has to be checked
    // BEFORE the per-type length, because an unrecognised type has no spec
    // length to check against -- and a 3-byte "message" of unknown type was
    // still handed out, so any caller reading Header::timestamp() (6 bytes at
    // offset 5) read past the buffer. Found by the fuzz campaign, not by
    // reasoning: the earlier fix only considered types we know.
    if (__builtin_expect(len < kHeaderLength, 0)) {
      ++stats_.short_message;
      ++stats_.messages;
      ++stats_.per_type[t];
      return next(out);
    }

    const std::uint16_t expect = spec_length(static_cast<char>(t));
    if (__builtin_expect(expect == 0, 0)) {
      ++stats_.unknown_type;
    } else if (__builtin_expect(expect != len, 0)) {
      ++stats_.length_mismatch;
      // A body SHORTER than its type's spec length must never be handed out.
      // The typed views read at fixed offsets -- AddOrderView::price() reads at
      // byte 32 -- so a 4-byte message claiming to be an 'A' makes every caller
      // read 28 bytes past it. Confirmed as a heap-buffer-overflow under ASan
      // before this check existed.
      //
      // Skipping rather than stopping: the length prefix still says where the
      // next message begins, so one malformed message does not have to end the
      // parse. The count is already part of the gate, which requires zero.
      if (__builtin_expect(len < expect, 0)) {
        ++stats_.short_message;
        ++stats_.messages;
        ++stats_.per_type[t];
        return next(out);        // skip it and try the next
      }
    }

    out.body = body;
    out.length = len;
    ++stats_.messages;
    ++stats_.per_type[t];
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
