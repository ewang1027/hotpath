#pragma once
#include "hotpath/core/types.hpp"
#include "hotpath/itch/byteorder.hpp"
#include "hotpath/itch/symbol.hpp"

#include <cstdint>

namespace hotpath::itch {

// NASDAQ TotalView-ITCH 5.0 message types.
enum class MsgType : char {
  SystemEvent          = 'S',
  StockDirectory       = 'R',
  StockTradingAction   = 'H',
  RegSHO               = 'Y',
  ParticipantPosition  = 'L',
  MwcbDeclineLevel     = 'V',
  MwcbStatus           = 'W',
  IpoQuotingPeriod     = 'K',
  LuldAuctionCollar    = 'J',
  OperationalHalt      = 'h',
  AddOrder             = 'A',
  AddOrderMpid         = 'F',
  OrderExecuted        = 'E',
  OrderExecutedPrice   = 'C',
  OrderCancel          = 'X',
  OrderDelete          = 'D',
  OrderReplace         = 'U',
  TradeNonCross        = 'P',
  CrossTrade           = 'Q',
  BrokenTrade          = 'B',
  Noii                 = 'I',
  Rpii                 = 'N',
};

// Spec lengths, used to VALIDATE the length prefix rather than to advance by.
// The file's 2-byte prefix is what actually drives iteration -- that way an
// unknown or newly-added message type is skipped cleanly instead of
// desynchronising the whole stream. Returns 0 for "not in the table".
[[nodiscard]] constexpr std::uint16_t spec_length(char t) noexcept {
  switch (t) {
    case 'S': return 12;  case 'R': return 39;  case 'H': return 25;
    case 'Y': return 20;  case 'L': return 26;  case 'V': return 35;
    case 'W': return 12;  case 'K': return 28;  case 'J': return 35;
    case 'h': return 21;  case 'A': return 36;  case 'F': return 40;
    case 'E': return 31;  case 'C': return 36;  case 'X': return 23;
    case 'D': return 19;  case 'U': return 35;  case 'P': return 44;
    case 'Q': return 40;  case 'B': return 19;  case 'I': return 50;
    case 'N': return 20;
    default:  return 0;
  }
}

// Every ITCH message begins with this header, so 11 bytes is the universal
// minimum length for a well-formed message of ANY type -- including a type this
// build does not know about.
inline constexpr std::uint16_t kHeaderLength = 11;

// Every message shares an 11-byte header:
//   [0]    message type
//   [1:3)  stock locate
//   [3:5)  tracking number
//   [5:11) timestamp, nanoseconds since midnight
struct Header {
  const std::uint8_t* p;
  [[nodiscard]] char type() const noexcept { return static_cast<char>(p[0]); }
  [[nodiscard]] StockLocate locate() const noexcept { return rd_u16(p + 1); }
  [[nodiscard]] std::uint16_t tracking() const noexcept { return rd_u16(p + 3); }
  [[nodiscard]] Ts timestamp() const noexcept { return rd_u48(p + 5); }
};

namespace detail {
inline Side side_of(std::uint8_t c) noexcept {
  return c == 'B' ? Side::Buy : Side::Sell;
}
} // namespace detail

struct AddOrderView : Header {
  [[nodiscard]] OrderId order_ref() const noexcept { return rd_u64(p + 11); }
  [[nodiscard]] Side    side()      const noexcept { return detail::side_of(p[19]); }
  [[nodiscard]] Qty     shares()    const noexcept { return rd_u32(p + 20); }
  [[nodiscard]] Symbol  stock()     const noexcept { return Symbol::from_bytes(p + 24); }
  [[nodiscard]] Price   price()     const noexcept { return rd_u32(p + 32); }
  // Only valid when type() == 'F'.
  [[nodiscard]] std::uint32_t attribution() const noexcept { return rd_u32(p + 36); }
};

struct OrderExecutedView : Header {
  [[nodiscard]] OrderId order_ref()       const noexcept { return rd_u64(p + 11); }
  [[nodiscard]] Qty     executed_shares() const noexcept { return rd_u32(p + 19); }
  [[nodiscard]] MatchId match_number()    const noexcept { return rd_u64(p + 23); }
  // Only valid when type() == 'C'.
  [[nodiscard]] bool    printable()       const noexcept { return p[31] == 'Y'; }
  [[nodiscard]] Price   execution_price() const noexcept { return rd_u32(p + 32); }
};

struct OrderCancelView : Header {
  [[nodiscard]] OrderId order_ref()        const noexcept { return rd_u64(p + 11); }
  [[nodiscard]] Qty     cancelled_shares() const noexcept { return rd_u32(p + 19); }
};

struct OrderDeleteView : Header {
  [[nodiscard]] OrderId order_ref() const noexcept { return rd_u64(p + 11); }
};

struct OrderReplaceView : Header {
  [[nodiscard]] OrderId original_ref() const noexcept { return rd_u64(p + 11); }
  [[nodiscard]] OrderId new_ref()      const noexcept { return rd_u64(p + 19); }
  [[nodiscard]] Qty     shares()       const noexcept { return rd_u32(p + 27); }
  [[nodiscard]] Price   price()        const noexcept { return rd_u32(p + 31); }
};

struct TradeView : Header {   // 'P', non-cross
  [[nodiscard]] OrderId order_ref()    const noexcept { return rd_u64(p + 11); }
  [[nodiscard]] Side    side()         const noexcept { return detail::side_of(p[19]); }
  [[nodiscard]] Qty     shares()       const noexcept { return rd_u32(p + 20); }
  [[nodiscard]] Symbol  stock()        const noexcept { return Symbol::from_bytes(p + 24); }
  [[nodiscard]] Price   price()        const noexcept { return rd_u32(p + 32); }
  [[nodiscard]] MatchId match_number() const noexcept { return rd_u64(p + 36); }
};

struct CrossTradeView : Header {  // 'Q'
  [[nodiscard]] std::uint64_t shares() const noexcept { return rd_u64(p + 11); }
  [[nodiscard]] Symbol  stock()        const noexcept { return Symbol::from_bytes(p + 19); }
  [[nodiscard]] Price   cross_price()  const noexcept { return rd_u32(p + 27); }
  [[nodiscard]] MatchId match_number() const noexcept { return rd_u64(p + 31); }
  [[nodiscard]] char    cross_type()   const noexcept { return static_cast<char>(p[39]); }
};

struct StockDirectoryView : Header {  // 'R'
  [[nodiscard]] Symbol stock()          const noexcept { return Symbol::from_bytes(p + 11); }
  [[nodiscard]] char   market_category()const noexcept { return static_cast<char>(p[19]); }
  [[nodiscard]] Qty    round_lot_size() const noexcept { return rd_u32(p + 21); }
};

struct StockTradingActionView : Header {  // 'H'
  [[nodiscard]] Symbol stock()         const noexcept { return Symbol::from_bytes(p + 11); }
  [[nodiscard]] char   trading_state() const noexcept { return static_cast<char>(p[19]); }
};

struct SystemEventView : Header {  // 'S'
  // 'O' start of messages, 'S' start of system hours, 'Q' start of market
  // hours, 'M' end of market hours, 'E' end of system hours, 'C' end of messages
  [[nodiscard]] char event_code() const noexcept { return static_cast<char>(p[11]); }
};

// True for the message types that mutate the limit order book. Everything else
// is reference/administrative data.
[[nodiscard]] constexpr bool mutates_book(char t) noexcept {
  return t == 'A' || t == 'F' || t == 'E' || t == 'C' ||
         t == 'X' || t == 'D' || t == 'U';
}

} // namespace hotpath::itch
