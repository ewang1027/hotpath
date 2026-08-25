#pragma once
// Fuzz targets, written so one body serves two drivers: the seeded runner in
// the normal test suite (always on, no toolchain requirement, and under ASan it
// catches real memory errors) and a libFuzzer entry point for deeper
// coverage-guided campaigns.
//
// Failures throw rather than abort, so Catch2 reports them with context. An
// escaping exception terminates a libFuzzer run, which is what it treats as a
// crash, so both drivers see the failure.
#include "hotpath/book/flat_book.hpp"
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/intrusive_book.hpp"
#include "hotpath/book/map_book.hpp"
#include "hotpath/itch/messages.hpp"
#include "hotpath/itch/reader.hpp"
#include "fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace hotpath::fuzz {

// ---------------------------------------------------------------------------
// Input generators, shared by the seeded test-suite runner and the long-running
// campaign tool so both explore the same space.
// ---------------------------------------------------------------------------
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  std::uint32_t below(std::uint32_t n) { return n ? static_cast<std::uint32_t>(next() % n) : 0; }
};

inline std::vector<std::uint8_t> random_bytes(Rng& r, std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (auto& b : v) b = static_cast<std::uint8_t>(r.next());
  return v;
}

// Purely random bytes almost never produce a plausible length prefix followed
// by a known message type, so they exercise the framing and little else. This
// builds a valid ITCH stream then corrupts a few bytes, which is what reaches
// the typed accessors -- and therefore what tests the length/offset handling
// at all.
inline std::vector<std::uint8_t> mutated_itch(Rng& r, std::size_t messages) {
  test::ItchBuilder b;
  for (std::size_t i = 0; i < messages; ++i) {
    const OrderId id = 1 + r.below(64);
    switch (r.below(6)) {
      case 0: b.add_order(id, (r.next() & 1) ? Side::Buy : Side::Sell,
                          1 + r.below(1000), "AAPL", 1000000 + r.below(5000)); break;
      case 1: b.order_executed(id, 1 + r.below(500), r.next()); break;
      case 2: b.order_cancel(id, 1 + r.below(500)); break;
      case 3: b.order_delete(id); break;
      case 4: b.order_replace(id, id + 1000, 1 + r.below(500), 1000000 + r.below(5000)); break;
      default: b.system_event(static_cast<char>('A' + r.below(26))); break;
    }
  }
  auto v = b.bytes();
  if (v.empty()) return v;
  const std::size_t flips = 1 + r.below(8);
  for (std::size_t i = 0; i < flips; ++i)
    v[r.below(static_cast<std::uint32_t>(v.size()))] ^= static_cast<std::uint8_t>(1u << r.below(8));
  if (r.next() & 1) v.resize(r.below(static_cast<std::uint32_t>(v.size())) + 1);
  return v;
}

// ---------------------------------------------------------------------------
// Target 1: parse arbitrary bytes as an ITCH stream and touch every accessor.
//
// The point is not that the values mean anything -- they will not -- but that
// no input makes the parser read outside its buffer, loop forever, or trap.
// Run under AddressSanitizer for this to have teeth.
// ---------------------------------------------------------------------------
inline std::uint64_t fuzz_itch_parse(const std::uint8_t* data, std::size_t size) {
  using namespace hotpath::itch;
  Reader r(data, size);
  RawMessage m{};
  std::uint64_t sink = 0;
  std::size_t guard = 0;

  while (r.next(m)) {
    if (++guard > (size / 2) + 16) {
      throw std::runtime_error("Reader produced more messages than the input can hold");
    }
    sink += static_cast<std::uint64_t>(m.length);
    const Header h{m.body};
    sink += h.locate() + h.tracking() + h.timestamp();

    switch (m.type()) {
      case 'A': case 'F': {
        const AddOrderView v{m.body};
        sink += v.order_ref() + v.shares() + v.price() + v.stock().raw
              + static_cast<std::uint64_t>(v.side());
        if (m.type() == 'F') sink += v.attribution();
        break;
      }
      case 'E': case 'C': {
        const OrderExecutedView v{m.body};
        sink += v.order_ref() + v.executed_shares() + v.match_number();
        if (m.type() == 'C') sink += v.execution_price() + (v.printable() ? 1u : 0u);
        break;
      }
      case 'X': { const OrderCancelView v{m.body}; sink += v.order_ref() + v.cancelled_shares(); break; }
      case 'D': { sink += OrderDeleteView{m.body}.order_ref(); break; }
      case 'U': {
        const OrderReplaceView v{m.body};
        sink += v.original_ref() + v.new_ref() + v.shares() + v.price();
        break;
      }
      case 'P': {
        const TradeView v{m.body};
        sink += v.order_ref() + v.shares() + v.price() + v.stock().raw + v.match_number();
        break;
      }
      case 'Q': {
        const CrossTradeView v{m.body};
        sink += v.shares() + v.stock().raw + v.cross_price() + v.match_number()
              + static_cast<std::uint64_t>(v.cross_type());
        break;
      }
      case 'R': {
        const StockDirectoryView v{m.body};
        sink += v.stock().raw + v.round_lot_size()
              + static_cast<std::uint64_t>(v.market_category());
        break;
      }
      case 'H': {
        const StockTradingActionView v{m.body};
        sink += v.stock().raw + static_cast<std::uint64_t>(v.trading_state());
        break;
      }
      case 'S': sink += static_cast<std::uint64_t>(SystemEventView{m.body}.event_code()); break;
      default: break;
    }
  }
  // Every byte must be accounted for: consumed, or left unread at a stop.
  if (r.stats().bytes > size) throw std::runtime_error("Reader consumed more than the input");
  return sink;
}

// ---------------------------------------------------------------------------
// Target 2: differential fuzzing across the four book designs.
//
// Bytes are decoded into a book-event script and replayed into all four
// implementations, whose ten-deep snapshots must agree after every event. This
// is the automated form of the gate that already found a real bug by hand --
// the intrusive book silently dropping orders when its level pool filled.
// ---------------------------------------------------------------------------
inline void fuzz_book_differential(const std::uint8_t* data, std::size_t size) {
  using namespace hotpath::book;
  if (size < 8) return;

  // A deliberately narrow grid so that out-of-window and sub-penny prices --
  // the overflow path -- are reachable from short inputs.
  constexpr Price kLo = 1'000'000, kHi = 1'020'000;
  constexpr std::size_t kMaxEvents = 4096;

  MapBook a;
  IntrusiveBook b(1u << 14, 1u << 14);
  FlatBook c(kLo, kHi, 1u << 14);
  HybridBook d(kLo, kHi, 1u << 14, 1u << 14);

  std::vector<OrderId> live;
  OrderId next_id = 1;
  Snapshot sa{}, sb{}, sc{}, sd{};
  std::size_t pos = 0, ev = 0;

  auto take = [&](std::size_t n) -> std::uint64_t {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | (pos < size ? data[pos++] : 0);
    return v;
  };

  while (pos + 4 <= size && ev < kMaxEvents) {
    ++ev;
    const std::uint8_t op = static_cast<std::uint8_t>(take(1)) % 5;
    const std::uint16_t raw = static_cast<std::uint16_t>(take(2));
    const std::uint8_t sel = static_cast<std::uint8_t>(take(1));

    // Spread prices across the grid, just outside it, far outside it, and off
    // the penny, so the dense path and both overflow paths all get exercised.
    Price px;
    switch (sel % 8) {
      case 0: case 1: case 2: case 3:
        px = kLo + (raw % 200) * 100; break;                 // inside, on the penny
      case 4: px = kLo + (raw % 200) * 100 + (raw % 99) + 1; break;  // sub-penny
      case 5: px = 1 + (raw % 500); break;                   // far below
      case 6: px = 5'000'000 + raw; break;                   // far above
      default: px = kHi - (raw % 300) * 100; break;          // near the top edge
    }
    const Qty qty = 1 + (raw % 500);

    if (op == 0 || live.size() < 3) {
      const OrderId id = next_id++;
      a.add(id, (sel & 1) ? Side::Buy : Side::Sell, px, qty);
      b.add(id, (sel & 1) ? Side::Buy : Side::Sell, px, qty);
      c.add(id, (sel & 1) ? Side::Buy : Side::Sell, px, qty);
      d.add(id, (sel & 1) ? Side::Buy : Side::Sell, px, qty);
      live.push_back(id);
    } else {
      const std::size_t idx = raw % live.size();
      const OrderId id = live[idx];
      switch (op) {
        case 1: a.execute(id, qty); b.execute(id, qty); c.execute(id, qty); d.execute(id, qty); break;
        case 2: a.cancel(id, qty);  b.cancel(id, qty);  c.cancel(id, qty);  d.cancel(id, qty);  break;
        case 3:
          a.remove(id); b.remove(id); c.remove(id); d.remove(id);
          live[idx] = live.back(); live.pop_back();
          break;
        default: {
          const OrderId nid = next_id++;
          a.replace(id, nid, px, qty); b.replace(id, nid, px, qty);
          c.replace(id, nid, px, qty); d.replace(id, nid, px, qty);
          live[idx] = nid;
          break;
        }
      }
    }

    a.snapshot(sa); b.snapshot(sb); c.snapshot(sc); d.snapshot(sd);
    if (!(sa == sb) || !(sa == sc) || !(sa == sd)) {
      char msg[256];
      std::snprintf(msg, sizeof msg,
                    "book designs diverged at event %zu (op=%u px=%u qty=%u): "
                    "map/intrusive %s, map/flat %s, map/hybrid %s",
                    ev, op, px, qty, (sa == sb) ? "ok" : "DIFFER",
                    (sa == sc) ? "ok" : "DIFFER", (sa == sd) ? "ok" : "DIFFER");
      throw std::runtime_error(msg);
    }
  }

  // Capacity exhaustion would make the bounded designs drop orders and diverge
  // for a reason unrelated to correctness, so it must not happen at this scale.
  if (b.rejected() || c.rejected() || d.rejected())
    throw std::runtime_error("a bounded book rejected an order: pools too small for the fuzzer");
}

} // namespace hotpath::fuzz
