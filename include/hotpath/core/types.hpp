#pragma once
#include <cstdint>

namespace hotpath {

// ITCH prices are unsigned 4-byte fixed point with 4 implied decimal places.
// We keep them in that raw integer form all the way through the hot path --
// no floating point anywhere in book maintenance or matching. 1 unit = $0.0001.
using Price = std::uint32_t;
using Qty   = std::uint32_t;
using OrderId = std::uint64_t;
using MatchId = std::uint64_t;
// Nanoseconds since midnight (ITCH carries a 6-byte ns timestamp).
using Ts = std::uint64_t;
// Nasdaq's per-symbol index; 0 is reserved/unused by the protocol.
using StockLocate = std::uint16_t;

inline constexpr Price kPriceScale = 10000;
inline constexpr Price kInvalidPrice = 0;
inline constexpr OrderId kInvalidOrderId = 0;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr const char* to_string(Side s) noexcept {
  return s == Side::Buy ? "B" : "S";
}

} // namespace hotpath
