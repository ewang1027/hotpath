#include "hotpath/sim/signals.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace hotpath;
using namespace hotpath::sim;
using Catch::Matchers::WithinAbs;

TEST_CASE("touch: imbalance sign and range", "[signals]") {
  // Braces around the aggregate would be split on their commas by the
  // preprocessor inside REQUIRE_THAT, so these are named.
  const Touch heavy_bid{100, 101, 900, 100};
  const Touch heavy_ask{100, 101, 100, 900};
  const Touch balanced{100, 101, 500, 500};
  const Touch all_bid{100, 101, 1, 0};
  const Touch all_ask{100, 101, 0, 1};
  REQUIRE(heavy_bid.imbalance() > 0);
  REQUIRE(heavy_ask.imbalance() < 0);
  REQUIRE_THAT(balanced.imbalance(), WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(all_bid.imbalance(), WithinAbs(1.0, 1e-12));
  REQUIRE_THAT(all_ask.imbalance(), WithinAbs(-1.0, 1e-12));
}

TEST_CASE("touch: microprice leans toward the thin side", "[signals]") {
  // A heavy bid means the bid is unlikely to break, so fair value sits nearer
  // the ask. Getting this backwards is the classic sign error.
  const Touch heavy_bid{1000000, 1000100, 9000, 1000};
  REQUIRE(heavy_bid.microprice() > heavy_bid.mid());
  const Touch heavy_ask{1000000, 1000100, 1000, 9000};
  REQUIRE(heavy_ask.microprice() < heavy_ask.mid());
}

// Not two signals: (microprice - mid) / (spread/2) reduces algebraically to
// (Qb-Qa)/(Qb+Qa). Worth pinning, because shipping both as separate features
// would be double-counting one piece of information.
TEST_CASE("touch: microprice tilt IS the queue imbalance", "[signals]") {
  for (Qty qb : {1u, 7u, 250u, 9999u}) {
    for (Qty qa : {1u, 13u, 640u, 100000u}) {
      const Touch t{1000000, 1000250, qb, qa};
      INFO("qb=" << qb << " qa=" << qa);
      REQUIRE_THAT(t.micro_tilt(), WithinAbs(t.imbalance(), 1e-9));
    }
  }
}

TEST_CASE("touch: invalid states are rejected rather than producing garbage",
          "[signals]") {
  const Touch empty{};
  const Touch crossed{1000100, 1000000, 10, 10};
  const Touch no_size{1000000, 1000100, 0, 0};
  const Touch ok{1000000, 1000100, 1, 1};
  REQUIRE_FALSE(empty.valid());
  REQUIRE_FALSE(crossed.valid());
  REQUIRE_FALSE(no_size.valid());
  REQUIRE(ok.valid());
}

TEST_CASE("ofi: signs follow buy/sell pressure", "[signals]") {
  const Touch base{1000000, 1000100, 500, 500};

  const Touch bid_up{1000050, 1000100, 300, 500};      // bid improves
  const Touch bid_down{999950, 1000100, 300, 500};     // bid pulled back
  const Touch ask_down{1000000, 1000050, 500, 300};    // ask steps toward the bid
  const Touch ask_up{1000000, 1000150, 500, 300};      // ask pulled away
  const Touch bid_bigger{1000000, 1000100, 700, 500};  // size added at same bid

  REQUIRE(OrderFlowImbalance::increment(base, bid_up) > 0);
  REQUIRE(OrderFlowImbalance::increment(base, bid_down) < 0);
  REQUIRE(OrderFlowImbalance::increment(base, ask_down) < 0);
  REQUIRE(OrderFlowImbalance::increment(base, ask_up) > 0);
  // Size added at an unchanged bid is buy pressure of exactly that size.
  REQUIRE_THAT(OrderFlowImbalance::increment(base, bid_bigger), WithinAbs(200.0, 1e-9));
  REQUIRE_THAT(OrderFlowImbalance::increment(base, base), WithinAbs(0.0, 1e-9));
}

TEST_CASE("ofi: accumulates and decays with the time constant", "[signals]") {
  OrderFlowImbalance ofi(/*tau_ns=*/1e9);
  const Touch a{1000000, 1000100, 500, 500};
  ofi.update(a, 0);
  REQUIRE_THAT(ofi.raw(), WithinAbs(0.0, 1e-9));

  const Touch pressure{1000000, 1000100, 900, 500};
  ofi.update(pressure, 1000);                            // +400 buy pressure
  const double first = ofi.raw();
  REQUIRE(first > 0);

  // One time constant of silence with no further pressure decays it toward 1/e.
  OrderFlowImbalance decayed(1e9);
  decayed.update(a, 0);
  decayed.update(pressure, 1000);
  decayed.update(pressure, 1000 + 1'000'000'000ull);
  REQUIRE(decayed.raw() < first);
  REQUIRE(decayed.raw() > 0);
  REQUIRE_THAT(decayed.raw(), WithinAbs(first / std::exp(1.0), first * 0.02));
}
