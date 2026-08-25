#include "hotpath/sim/bootstrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace hotpath;
using namespace hotpath::sim;
using Catch::Matchers::WithinAbs;

namespace {
constexpr Ts kSec = 1'000'000'000ull;
constexpr Ts kBlock = 10 * kSec;

// n observations spread evenly over `span` blocks.
std::vector<WeightedObs> series(std::size_t n, double value, double weight = 1.0,
                                Ts spacing = kSec) {
  std::vector<WeightedObs> v;
  v.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    v.push_back(WeightedObs{static_cast<Ts>(i) * spacing, value, weight});
  return v;
}
} // namespace

TEST_CASE("bootstrap: a constant series has no uncertainty", "[stats]") {
  const Interval iv = block_bootstrap(series(500, 2.5), kBlock);
  REQUIRE_THAT(iv.point, WithinAbs(2.5, 1e-9));
  REQUIRE_THAT(iv.lo, WithinAbs(2.5, 1e-9));
  REQUIRE_THAT(iv.hi, WithinAbs(2.5, 1e-9));
  REQUIRE(iv.excludes_zero());
}

TEST_CASE("bootstrap: weights actually weight", "[stats]") {
  // One observation of value 100 with weight 999 against 99 of value 0.
  std::vector<WeightedObs> v = series(99, 0.0, 1.0);
  v.push_back(WeightedObs{99 * kSec, 100.0, 999.0});
  const Interval iv = block_bootstrap(v, kBlock);
  // Share-weighted mean is 999*100/(999+99) ~= 91, not the unweighted 1.0.
  REQUIRE(iv.point > 80.0);
}

TEST_CASE("bootstrap: symmetric noise straddles zero", "[stats]") {
  std::vector<WeightedObs> v;
  for (std::size_t i = 0; i < 2000; ++i)
    v.push_back(WeightedObs{static_cast<Ts>(i) * kSec / 4,
                            (i % 2 == 0) ? 1.0 : -1.0, 1.0});
  const Interval iv = block_bootstrap(v, kBlock);
  REQUIRE_THAT(iv.point, WithinAbs(0.0, 1e-6));
  REQUIRE_FALSE(iv.excludes_zero());
}

TEST_CASE("bootstrap: a real offset is detected", "[stats]") {
  // Alternating +/-1 shifted up by 0.5: mean 0.5, and with 200 blocks the
  // interval should clear zero.
  std::vector<WeightedObs> v;
  for (std::size_t i = 0; i < 2000; ++i)
    v.push_back(WeightedObs{static_cast<Ts>(i) * kSec / 4,
                            ((i % 2 == 0) ? 1.0 : -1.0) + 0.5, 1.0});
  const Interval iv = block_bootstrap(v, kBlock);
  REQUIRE_THAT(iv.point, WithinAbs(0.5, 1e-6));
  REQUIRE(iv.excludes_zero());
  REQUIRE(iv.lo > 0.0);
}

TEST_CASE("bootstrap: identical series differ by exactly nothing", "[stats]") {
  const auto a = series(500, 1.25);
  const Interval d = paired_block_bootstrap(a, a, kBlock);
  REQUIRE_THAT(d.point, WithinAbs(0.0, 1e-9));
  REQUIRE_FALSE(d.excludes_zero());
}

TEST_CASE("bootstrap: a constant offset between series is detected", "[stats]") {
  const Interval d = paired_block_bootstrap(series(500, 2.0), series(500, 1.0), kBlock);
  REQUIRE_THAT(d.point, WithinAbs(1.0, 1e-9));
  REQUIRE(d.excludes_zero());
}

// The reason the paired version exists. Two series that move together but
// differ by a small constant: pairing cancels the common swing, so the
// difference is resolvable even though each series on its own is far too noisy
// to say anything about.
TEST_CASE("bootstrap: pairing resolves what independent intervals cannot",
          "[stats]") {
  std::vector<WeightedObs> a, b;
  for (std::size_t i = 0; i < 1200; ++i) {
    const Ts t = static_cast<Ts>(i) * kSec / 2;
    const double common = 20.0 * std::sin(static_cast<double>(i) / 7.0);  // shared swing
    a.push_back(WeightedObs{t, common + 0.5, 1.0});
    b.push_back(WeightedObs{t, common - 0.5, 1.0});
  }
  const Interval ia = block_bootstrap(a, kBlock);
  const Interval ib = block_bootstrap(b, kBlock);
  const Interval d  = paired_block_bootstrap(a, b, kBlock);

  // Each series alone is dominated by the shared swing...
  REQUIRE(ia.hi - ia.lo > 1.0);
  REQUIRE(ib.hi - ib.lo > 1.0);
  // ...but their difference is a clean +1.0 with a tight interval.
  REQUIRE_THAT(d.point, WithinAbs(1.0, 1e-6));
  REQUIRE(d.excludes_zero());
  REQUIRE(d.hi - d.lo < 0.1);
}

TEST_CASE("bootstrap: empty and degenerate inputs do not blow up", "[stats]") {
  const std::vector<WeightedObs> none;
  REQUIRE(block_bootstrap(none, kBlock).n == 0);
  REQUIRE(paired_block_bootstrap(none, none, kBlock).n == 0);
  // Too few blocks to resample: report the point estimate with no spread
  // rather than a fabricated interval.
  const Interval tiny = block_bootstrap(series(3, 5.0), kBlock);
  REQUIRE_THAT(tiny.point, WithinAbs(5.0, 1e-9));
  REQUIRE_THAT(tiny.lo, WithinAbs(5.0, 1e-9));
}
