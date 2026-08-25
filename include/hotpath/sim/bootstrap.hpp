#pragma once
#include "hotpath/core/types.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hotpath::sim {

// A time-stamped, weighted observation -- e.g. one fill's markout weighted by
// its share count.
struct WeightedObs {
  Ts     ts;
  double value;
  double weight;
};

struct Interval {
  double point{0};
  double lo{0};
  double hi{0};
  std::uint64_t n{0};
  [[nodiscard]] bool excludes_zero() const noexcept { return (lo > 0.0) || (hi < 0.0); }
};

namespace detail {

inline double weighted_mean(const std::vector<std::vector<WeightedObs>>& blocks,
                            const std::vector<std::size_t>& pick) noexcept {
  double num = 0, den = 0;
  for (std::size_t b : pick)
    for (const auto& o : blocks[b]) { num += o.value * o.weight; den += o.weight; }
  return den > 0 ? num / den : 0.0;
}

// Bucket observations into fixed wall-clock blocks spanning [t0, t1].
inline std::vector<std::vector<WeightedObs>> bucket(const std::vector<WeightedObs>& obs,
                                                    Ts t0, Ts t1, Ts block_ns) {
  const std::size_t nb = static_cast<std::size_t>((t1 - t0) / block_ns) + 1;
  std::vector<std::vector<WeightedObs>> out(nb);
  for (const auto& o : obs) {
    if (o.ts < t0 || o.ts > t1) continue;
    out[static_cast<std::size_t>((o.ts - t0) / block_ns)].push_back(o);
  }
  return out;
}

struct Rng {
  std::uint64_t s{0x9E3779B97F4A7C15ull};
  std::uint64_t operator()() noexcept {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
  }
};

inline void percentiles(std::vector<double>& v, Interval& out) {
  if (v.size() < 50) { out.lo = out.hi = out.point; return; }
  std::sort(v.begin(), v.end());
  out.lo = v[static_cast<std::size_t>(0.025 * static_cast<double>(v.size() - 1))];
  out.hi = v[static_cast<std::size_t>(0.975 * static_cast<double>(v.size() - 1))];
}

} // namespace detail

// Block bootstrap on a weighted mean.
//
// Blocks are wall-clock, not fixed counts of observations. Fills cluster hard --
// executions arrive in microsecond bursts against a ~100 ms median event gap --
// so resampling individual observations would treat a burst of highly
// correlated fills as independent evidence and return an interval several times
// too tight.
inline Interval block_bootstrap(const std::vector<WeightedObs>& obs,
                                Ts block_ns, int resamples = 2000) {
  Interval r;
  if (obs.empty()) return r;
  r.n = obs.size();
  const Ts t0 = obs.front().ts, t1 = obs.back().ts;
  if (t1 <= t0) return r;

  auto blocks = detail::bucket(obs, t0, t1, block_ns);
  const std::size_t nb = blocks.size();
  std::vector<std::size_t> all(nb);
  for (std::size_t i = 0; i < nb; ++i) all[i] = i;
  r.point = detail::weighted_mean(blocks, all);
  if (nb < 8) { r.lo = r.hi = r.point; return r; }

  detail::Rng rng;
  std::vector<double> means;
  means.reserve(static_cast<std::size_t>(resamples));
  std::vector<std::size_t> pick(nb);
  for (int k = 0; k < resamples; ++k) {
    for (std::size_t i = 0; i < nb; ++i) pick[i] = rng() % nb;
    const double m = detail::weighted_mean(blocks, pick);
    if (m != 0.0) means.push_back(m);
  }
  detail::percentiles(means, r);
  return r;
}

// Block bootstrap on the DIFFERENCE (a - b), resampling the SAME time blocks
// for both series.
//
// This is the right test whenever a and b are observed over the same session --
// two policies trading the same market, or two subsets of one fill stream. Most
// of the variance is common to both and cancels inside each resample. Comparing
// two independently-bootstrapped intervals instead is far weaker and will call
// real differences non-significant.
inline Interval paired_block_bootstrap(const std::vector<WeightedObs>& a,
                                       const std::vector<WeightedObs>& b,
                                       Ts block_ns, int resamples = 2000) {
  Interval r;
  if (a.empty() || b.empty()) return r;
  r.n = a.size() + b.size();
  const Ts t0 = std::min(a.front().ts, b.front().ts);
  const Ts t1 = std::max(a.back().ts, b.back().ts);
  if (t1 <= t0) return r;

  auto ba = detail::bucket(a, t0, t1, block_ns);
  auto bb = detail::bucket(b, t0, t1, block_ns);
  const std::size_t nb = ba.size();
  std::vector<std::size_t> all(nb);
  for (std::size_t i = 0; i < nb; ++i) all[i] = i;
  r.point = detail::weighted_mean(ba, all) - detail::weighted_mean(bb, all);
  if (nb < 8) { r.lo = r.hi = r.point; return r; }

  detail::Rng rng;
  std::vector<double> diffs;
  diffs.reserve(static_cast<std::size_t>(resamples));
  std::vector<std::size_t> pick(nb);
  for (int k = 0; k < resamples; ++k) {
    for (std::size_t i = 0; i < nb; ++i) pick[i] = rng() % nb;
    const double x = detail::weighted_mean(ba, pick);
    const double y = detail::weighted_mean(bb, pick);
    if (x != 0.0 || y != 0.0) diffs.push_back(x - y);
  }
  detail::percentiles(diffs, r);
  return r;
}

} // namespace hotpath::sim
