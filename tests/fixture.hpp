#pragma once
// Builds a synthetic ITCH 5.0 stream in memory, in the same BinaryFILE framing
// Nasdaq ships (2-byte big-endian length prefix per message).
//
// Tests run against this rather than the 3.5 GB sample day: CI cannot download
// that, and a hand-built stream lets us assert exact field values at exact
// offsets, which a real file cannot (we would only be checking the parser
// against itself).
#include "hotpath/core/types.hpp"
#include "hotpath/itch/symbol.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hotpath::test {

class ItchBuilder {
public:
  // Appends a message body; the framing prefix is written automatically.
  void add(const std::vector<std::uint8_t>& body) {
    const auto len = static_cast<std::uint16_t>(body.size());
    buf_.push_back(static_cast<std::uint8_t>(len >> 8));
    buf_.push_back(static_cast<std::uint8_t>(len & 0xff));
    buf_.insert(buf_.end(), body.begin(), body.end());
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return buf_; }

  // ---- field writers (big-endian) ----
  static void put8(std::vector<std::uint8_t>& v, std::uint8_t x) { v.push_back(x); }
  static void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(std::uint8_t(x >> 8)); v.push_back(std::uint8_t(x));
  }
  static void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(std::uint8_t(x >> (8 * i)));
  }
  static void put48(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 5; i >= 0; --i) v.push_back(std::uint8_t(x >> (8 * i)));
  }
  static void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back(std::uint8_t(x >> (8 * i)));
  }
  static void put_sym(std::vector<std::uint8_t>& v, const std::string& s) {
    for (std::size_t i = 0; i < 8; ++i) v.push_back(i < s.size() ? std::uint8_t(s[i]) : ' ');
  }
  static std::vector<std::uint8_t> header(char type, StockLocate locate, Ts ts) {
    std::vector<std::uint8_t> v;
    put8(v, std::uint8_t(type));
    put16(v, locate);
    put16(v, 0);            // tracking number
    put48(v, ts);
    return v;
  }

  // ---- message constructors ----
  void add_order(OrderId ref, Side side, Qty shares, const std::string& sym,
                 Price px, StockLocate locate = 1, Ts ts = 0) {
    auto v = header('A', locate, ts);
    put64(v, ref);
    put8(v, side == Side::Buy ? 'B' : 'S');
    put32(v, shares);
    put_sym(v, sym);
    put32(v, px);
    add(v);
  }

  void order_executed(OrderId ref, Qty shares, MatchId match,
                      StockLocate locate = 1, Ts ts = 0) {
    auto v = header('E', locate, ts);
    put64(v, ref);
    put32(v, shares);
    put64(v, match);
    add(v);
  }

  void order_cancel(OrderId ref, Qty shares, StockLocate locate = 1, Ts ts = 0) {
    auto v = header('X', locate, ts);
    put64(v, ref);
    put32(v, shares);
    add(v);
  }

  void order_delete(OrderId ref, StockLocate locate = 1, Ts ts = 0) {
    auto v = header('D', locate, ts);
    put64(v, ref);
    add(v);
  }

  void order_replace(OrderId orig, OrderId neu, Qty shares, Price px,
                     StockLocate locate = 1, Ts ts = 0) {
    auto v = header('U', locate, ts);
    put64(v, orig);
    put64(v, neu);
    put32(v, shares);
    put32(v, px);
    add(v);
  }

  void system_event(char code, Ts ts = 0) {
    auto v = header('S', 0, ts);
    put8(v, std::uint8_t(code));
    add(v);
  }

private:
  std::vector<std::uint8_t> buf_;
};

} // namespace hotpath::test
