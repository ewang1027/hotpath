#pragma once
#include "hotpath/core/types.hpp"

#include <cstdint>

namespace hotpath::book {

// One aggregated price level as seen by an observer of the book.
struct LevelView {
  Price         price;
  Qty           qty;      // total resting shares at this price
  std::uint32_t orders;   // number of resting orders at this price

  friend bool operator==(const LevelView& a, const LevelView& b) noexcept {
    return a.price == b.price && a.qty == b.qty && a.orders == b.orders;
  }
};

// book_crossval hashes snapshots byte-wise to check determinism, which is only
// sound because LevelView has no padding. Lock that in: adding a differently
// sized field here would silently start hashing indeterminate bytes and produce
// spurious "nondeterminism".
static_assert(sizeof(LevelView) == 12, "LevelView gained padding; hash its fields explicitly");

inline constexpr int kSnapshotDepth = 10;

// Fixed-depth snapshot, used to cross-validate the designs against each other.
// Comparing top-of-book alone would let a design be wrong two levels down and
// still pass; comparing the whole book would make the gate slower than the
// thing it validates. Ten levels is deeper than any strategy here looks.
struct Snapshot {
  LevelView bid[kSnapshotDepth];
  LevelView ask[kSnapshotDepth];
  int nbid{0};
  int nask{0};

  friend bool operator==(const Snapshot& a, const Snapshot& b) noexcept {
    if (a.nbid != b.nbid || a.nask != b.nask) return false;
    for (int i = 0; i < a.nbid; ++i) if (!(a.bid[i] == b.bid[i])) return false;
    for (int i = 0; i < a.nask; ++i) if (!(a.ask[i] == b.ask[i])) return false;
    return true;
  }
};

// ---------------------------------------------------------------------------
// The operation set every design implements. Deliberately not a virtual base:
// these are benchmarked against each other, and a vtable would add a uniform
// indirection that muddies the comparison. The designs are duck-typed and the
// driver is templated on them.
//
//   void add    (OrderId, Side, Price, Qty)
//   void execute(OrderId, Qty)     partial fill: reduce, remove at zero
//   void cancel (OrderId, Qty)     partial cancel: reduce, remove at zero
//   void remove (OrderId)          full delete
//   void replace(OrderId old, OrderId neu, Price, Qty)
//   Price best_bid() / best_ask()  kInvalidPrice when that side is empty
//   void snapshot(Snapshot&) const
//   void clear()
//
// Replace inherits the original order's side, because ITCH's Order Replace
// message does not carry one -- it only makes sense relative to the order
// being replaced.
// ---------------------------------------------------------------------------

} // namespace hotpath::book
