#include "hotpath/sim/market_maker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::sim;

namespace {

BookEvent add(OrderId ref, Side s, Price px, Qty q, Ts ts) {
  return BookEvent{ts, ref, 0, px, q, EventType::Add, s, 0, 0};
}
BookEvent exec(OrderId ref, Qty q, Ts ts) {
  return BookEvent{ts, ref, 0, 0, q, EventType::Execute, Side::Buy, 0, 0};
}
BookEvent del(OrderId ref, Ts ts) {
  return BookEvent{ts, ref, 0, 0, 0, EventType::Delete, Side::Buy, 0, 0};
}

// Builds a two-sided book, lets the maker's FIRST quote land, then removes the
// best bid so the touch drops to 999900. What happens next depends entirely on
// whether the maker's replacement has landed.
//
// Note the settling event: placing the initial quote costs latency too, so
// without a gap after the book becomes two-sided the maker would still have
// nothing resting and the test would pass for the wrong reason.
constexpr Ts kSettle = 5'000'000;   // 5 ms: past every latency under test

std::vector<Fill> run(Ts latency) {
  std::vector<BookEvent> tape = {
      add(1, Side::Buy, 1000000, 500, 0),
      add(3, Side::Buy,  999900, 300, 1),
      add(2, Side::Sell, 1000200, 500, 2),
      add(4, Side::Sell, 1000300, 100, kSettle),   // settle: initial quote lands
      del(1, kSettle + 1),           // best bid falls to 999900
      exec(3, 100, kSettle + 2),     // 100 shares trade at 999900
  };
  MarketMaker mm(900000, 1100000, 100, latency, FillModel::QueueAware, 1u << 12, 1u << 12);
  std::vector<Fill> fills;
  for (const auto& e : tape) mm.on_event(e, [&](const Fill& f) { fills.push_back(f); });
  return fills;
}

} // namespace

TEST_CASE("maker: with zero latency the quote follows the touch down", "[sim][mm]") {
  // The replacement lands immediately, so by the time 100 shares trade at
  // 999900 we are resting there behind 300 shares. Queue position protects us.
  const auto fills = run(0);
  REQUIRE(fills.empty());
}

TEST_CASE("maker: the initial quote placement also costs latency", "[sim][mm]") {
  // Not an edge case to design away -- getting your first order out takes just
  // as long as moving one, and at high latency that is part of why a slow maker
  // spends less time quoted.
  MarketMaker mm(900000, 1100000, 100, 1'000'000, FillModel::QueueAware, 1u << 12, 1u << 12);
  std::vector<Fill> fills;
  auto sink = [&](const Fill& f) { fills.push_back(f); };
  mm.on_event(add(1, Side::Buy, 1000000, 500, 0), sink);
  mm.on_event(add(2, Side::Sell, 1000200, 500, 1), sink);
  // Trade at our intended price before our quote could possibly have arrived.
  mm.on_event(exec(1, 500, 2), sink);
  REQUIRE(fills.empty());
}

TEST_CASE("maker: a stale quote gets swept while the replacement is in flight",
          "[sim][mm]") {
  // 1 ms of latency and only 1 ns of tape: the replacement never lands, so our
  // bid is still sitting at 1000000 when 100 shares trade at 999900. An
  // aggressor selling into 999900 would have hit our better-priced bid first.
  const auto fills = run(1'000'000);
  REQUIRE(fills.size() == 1);
  REQUIRE(fills[0].side == Side::Buy);
  REQUIRE(fills[0].price == 1000000);
  REQUIRE(fills[0].qty == 100);
  REQUIRE(fills[0].swept);      // filled by being more aggressive, not by queue
  REQUIRE(fills[0].stale);      // and on a quote we had already decided to move
}

TEST_CASE("maker: an in-flight replacement lands once the latency elapses",
          "[sim][mm]") {
  MarketMaker mm(900000, 1100000, 100, /*latency=*/1'000'000, FillModel::QueueAware, 1u << 12, 1u << 12);
  std::vector<Fill> fills;
  auto sink = [&](const Fill& f) { fills.push_back(f); };

  mm.on_event(add(1, Side::Buy, 1000000, 500, 0), sink);
  mm.on_event(add(3, Side::Buy,  999900, 300, 1), sink);
  mm.on_event(add(2, Side::Sell, 1000200, 500, 2), sink);
  mm.on_event(add(4, Side::Sell, 1000300, 100, kSettle), sink);   // initial quote lands
  mm.on_event(del(1, kSettle + 1), sink);                         // decide to move

  // Well past the 1 ms latency: the replacement has landed at 999900, so the
  // same trade that swept us above now finds us queued behind 300 shares.
  mm.on_event(exec(3, 100, kSettle + 1 + 2'000'000), sink);
  REQUIRE(fills.empty());
}

TEST_CASE("maker: latency does not change behaviour when the touch is stable",
          "[sim][mm]") {
  // No touch move means no replacement is ever in flight, so every latency
  // setting must produce identical fills.
  std::vector<BookEvent> tape = {
      add(1, Side::Buy, 1000000, 200, 0),
      add(2, Side::Sell, 1000200, 200, 1),
      add(5, Side::Sell, 1000400, 50, kSettle),   // let every latency's quote land
      exec(1, 150, kSettle + 1),
      exec(1, 50, kSettle + 2),
  };
  std::vector<std::size_t> counts;
  for (Ts L : {Ts(0), Ts(1'000), Ts(10'000), Ts(1'000'000)}) {
    MarketMaker mm(900000, 1100000, 100, L, FillModel::QueueAware, 1u << 12, 1u << 12);
    std::size_t n = 0;
    for (const auto& e : tape) mm.on_event(e, [&](const Fill&) { ++n; });
    counts.push_back(n);
  }
  for (std::size_t c : counts) REQUIRE(c == counts.front());
}
