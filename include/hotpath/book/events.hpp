#pragma once
#include "hotpath/core/types.hpp"
#include "hotpath/itch/symbol.hpp"

#include <cstdint>

namespace hotpath::book {

// A book-mutating event, normalised out of ITCH and resolved to one symbol.
//
// Why a separate tape instead of parsing ITCH in the benchmark loop: the three
// book designs must see byte-identical input, and re-parsing 12 GB per design
// per trial would make the parse dominate the thing being compared. Extracting
// once to a compact array isolates the variable.
//
// Note what is deliberately NOT resolved here: for Execute/Cancel/Delete/
// Replace, ITCH carries only the order reference -- no symbol, no side, no
// price. Recovering those is the book's own job, so leaving them unresolved
// keeps the order-lookup cost inside the design being measured rather than
// hoisting it into the harness.
enum class EventType : std::uint8_t {
  Add     = 0,   // 'A' / 'F'
  Execute = 1,   // 'E' / 'C'
  Cancel  = 2,   // 'X'  (partial cancel: reduce by shares)
  Delete  = 3,   // 'D'  (remove whole order)
  Replace = 4,   // 'U'  (delete original, add new_ref at new price/size)
};

struct BookEvent {
  Ts        ts;          // nanoseconds since midnight
  OrderId   order_ref;   // the order being acted on
  OrderId   new_ref;     // Replace only; 0 otherwise
  Price     price;       // Add / Replace only
  Qty       shares;      // Add / Replace / Execute / Cancel
  EventType type;
  Side      side;        // Add only (Replace inherits the original's side)
  std::uint16_t _pad{0};
};
static_assert(sizeof(BookEvent) == 40, "BookEvent layout changed; bump kTapeVersion");

// On-disk tape header. The tape is this header followed by a flat array of
// BookEvent, so a consumer can mmap it and cast straight to the array.
struct TapeHeader {
  char          magic[8];      // "HPTAPE\0\0"
  std::uint32_t version;
  std::uint32_t event_size;    // sizeof(BookEvent), so a layout change is caught
  std::uint64_t event_count;
  std::uint64_t symbol_raw;    // itch::Symbol::raw
  std::uint32_t stock_locate;
  std::uint32_t _reserved{0};
};

inline constexpr std::uint32_t kTapeVersion = 1;
inline constexpr char kTapeMagic[8] = {'H','P','T','A','P','E','\0','\0'};

// Apply one tape event to any book design. Kept in one place so the
// cross-validator and the benchmark cannot drift apart in how they replay.
template <typename Book>
inline void apply(Book& b, const BookEvent& e) {
  switch (e.type) {
    case EventType::Add:     b.add(e.order_ref, e.side, e.price, e.shares); break;
    case EventType::Execute: b.execute(e.order_ref, e.shares); break;
    case EventType::Cancel:  b.cancel(e.order_ref, e.shares); break;
    case EventType::Delete:  b.remove(e.order_ref); break;
    case EventType::Replace: b.replace(e.order_ref, e.new_ref, e.price, e.shares); break;
  }
}

} // namespace hotpath::book
