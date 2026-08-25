// Does a microstructure signal actually improve a passive strategy -- and does
// the fill model change the answer?
//
// This is the question the rest of the repo was built to be able to ask. The
// naive fill model books 3.4-15.6x the passive volume a queue-aware model does
// (docs/ADVERSE-SELECTION.md). If it also reports a different verdict on a
// signal, then every backtest built on it is not merely optimistic about
// volume, it is optimistic about whether the strategy works at all.
//
// Both models run the SAME MarketMaker with the same quoting logic, the same
// re-quote latency and the same signal. The only line that differs is the fill
// rule, so any difference in verdict is attributable to it.
//
// Signal: queue imbalance (Qb-Qa)/(Qb+Qa) at the touch. Note this is exactly
// Stoikov's microprice tilt -- (microprice - mid) / (spread/2) reduces to the
// imbalance algebraically, so they are one signal and not two.
//
// Policy: when the book leans hard one way, stop quoting the side that is about
// to be run over. Leaning bid means the price is likely to rise, so the ASK is
// the side that gets lifted at a stale price. Pulling a quote is not free: the
// cancel takes the same latency as any other quote change.
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/sim/bootstrap.hpp"
#include "hotpath/sim/market_maker.hpp"
#include "hotpath/sim/signals.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::sim;

namespace {

constexpr Ts kSec = 1'000'000'000ull;
// Markout horizon. Default 1s, not 10s: signal_study shows queue imbalance has
// real forecasting power at 100ms-1s and essentially none by 10s, so scoring a
// signal-driven policy at 10s measures mostly noise the signal never claimed to
// predict. Aligning the evaluation horizon with the signal's demonstrated
// horizon is a methodological fix, not a search for a better number -- the 10s
// result is reported alongside.
Ts g_horizon = 1 * kSec;

struct MidSample { Ts ts; double mid; };

double mid_at(const std::vector<MidSample>& s, Ts t) {
  if (s.empty() || t > s.back().ts) return -1.0;
  const auto it = std::upper_bound(s.begin(), s.end(), t,
                                   [](Ts v, const MidSample& m) { return v < m.ts; });
  return it == s.begin() ? -1.0 : (it - 1)->mid;
}

struct FillObs { Ts ts; double bps; Qty qty; Side side; };

// Adapt fills to the shared bootstrap's weighted-observation form. Weighting by
// share count matters: a 1-share fill and a 500-share fill are not equal
// evidence about a policy's economics.
std::vector<WeightedObs> to_obs(const std::vector<FillObs>& f) {
  std::vector<WeightedObs> v;
  v.reserve(f.size());
  for (const auto& x : f) v.push_back(WeightedObs{x.ts, x.bps, static_cast<double>(x.qty)});
  return v;
}

struct Outcome {
  std::uint64_t fills{0}, shares{0};
  double markout{0};
  double bid_markout{0}, ask_markout{0};
  double pnl{0};
  std::vector<FillObs> obs;             // time-ordered, for the paired bootstrap
};

// How a policy decides which sides to quote.
//
// When the book leans hard one way, quote only ONE side. Which side is the
// whole question.
//
//   HeavyOnly -- quote the side with the longer queue. This is what the
//                unconditional price forecast argues for: a heavy bid predicts
//                a rising price, so stop offering and stop being lifted.
//   ThinOnly  -- quote the side with the shorter queue. The exact mirror: same
//                threshold, near-identical duty cycle, opposite direction.
//
// Running both is what makes the experiment readable. Pulling a quote forfeits
// queue position whichever direction you pull it, so a one-sided policy can
// lose for reasons that have nothing to do with the signal. Only the mirror
// separates "the signal is worthless" from "the signal is backwards".
enum class Mode { Always, HeavyOnly, ThinOnly };

struct Policy {
  const char* name;
  Mode mode;
  double threshold;
};

Outcome run(const Tape& tape, const Tape::Window& win, Qty size, Ts latency,
            FillModel fm, const Policy& p, const std::vector<MidSample>& mids,
            std::vector<MidSample>* collect_mids = nullptr) {
  MarketMaker mm(win.lo, win.hi, size, latency, fm);
  Outcome o;
  double last_mid = -1.0;
  double bid_num = 0, bid_den = 0, ask_num = 0, ask_den = 0;

  const BookEvent* ev = tape.events();
  for (std::size_t i = 0; i < tape.size(); ++i) {
    mm.on_event(ev[i], [&](const Fill& f) {
      o.fills += 1; o.shares += f.qty;
      if (collect_mids) return;                  // baseline pass only builds mids
      const double m = mid_at(mids, f.ts + g_horizon);
      if (m < 0) return;
      const double sign = f.side == Side::Buy ? 1.0 : -1.0;
      const double diff = sign * (m - static_cast<double>(f.price));
      const double bps = 1e4 * diff / m;
      o.obs.push_back(FillObs{f.ts, bps, f.qty, f.side});
      o.pnl += diff * static_cast<double>(f.qty) / 1e4;
      if (f.side == Side::Buy) { bid_num += bps * f.qty; bid_den += f.qty; }
      else                     { ask_num += bps * f.qty; ask_den += f.qty; }
    });

    if (collect_mids) {
      const Touch t = mm.touch();
      if (t.valid() && t.mid() != last_mid) {
        collect_mids->push_back(MidSample{ev[i].ts, t.mid()});
        last_mid = t.mid();
      }
      continue;
    }

    // Signal from the state we have just finished observing, applied to the
    // next event: you cannot act on information before you have it.
    if (p.mode != Mode::Always) {
      const Touch t = mm.touch();
      if (t.valid()) {
        // imb > 0 means the bid is the heavy side. HeavyOnly suppresses the
        // thin side; ThinOnly flips the sign and suppresses the heavy one.
        const double imb = p.mode == Mode::HeavyOnly ? t.imbalance() : -t.imbalance();
        mm.set_quoting(imb > -p.threshold, imb < p.threshold);
      }
    }
  }
  double num = 0, den = 0;
  for (const auto& f : o.obs) { num += f.bps * f.qty; den += f.qty; }
  o.markout = den > 0 ? num / den : 0.0;
  o.bid_markout = bid_den > 0 ? bid_num / bid_den : 0.0;
  o.ask_markout = ask_den > 0 ? ask_num / ask_den : 0.0;
  return o;
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  Qty size = 100;
  Ts latency = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--size" && i + 1 < argc) size = static_cast<Qty>(std::atoi(argv[++i]));
    else if (a == "--latency" && i + 1 < argc) latency = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--horizon-ms" && i + 1 < argc)
      g_horizon = std::strtoull(argv[++i], nullptr, 10) * 1'000'000ull;
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: strategy_eval <SYM.tape> [--size N] [--latency NS] [--horizon-ms N]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();

  // Mids do not depend on our quotes -- we never enter the book -- so one pass
  // builds the series every configuration is marked out against.
  std::vector<MidSample> mids;
  mids.reserve(tape.size() / 4);
  (void)run(tape, win, size, latency, FillModel::QueueAware, Policy{"mids", Mode::Always, 0.0}, mids, &mids);

  const Policy policies[] = {
      {"baseline (quote both)",   Mode::Always,    0.00},
      {"quote HEAVY side  >0.25", Mode::HeavyOnly, 0.25},
      {"quote THIN  side  >0.25", Mode::ThinOnly,  0.25},
      {"quote HEAVY side  >0.50", Mode::HeavyOnly, 0.50},
      {"quote THIN  side  >0.50", Mode::ThinOnly,  0.50},
  };
  constexpr int kNPol = 5;

  std::printf("tape       : %s\n", path.c_str());
  std::printf("events     : %zu\n", tape.size());
  std::printf("quote size : %u   re-quote latency: %" PRIu64 " ns\n", size, latency);
  std::printf("signal     : queue imbalance at the touch, acted on one event later\n");
  std::printf("markout    : %" PRIu64 " ms horizon, share-weighted\n", g_horizon / 1'000'000);
  std::printf("comparison : paired block bootstrap on 5-minute blocks, 2000 resamples.\n");
  std::printf("             Both policies trade the same market, so resampling the same\n");
  std::printf("             time blocks for both cancels the shared variance -- comparing\n");
  std::printf("             two independent CIs would be a far weaker test.\n");

  const Ts kBlock = 300 * kSec;   // 5-minute blocks for the paired bootstrap
  double skew_cost[2] = {0, 0};
  double thin_minus_heavy[2][2] = {};
  bool   thin_sig[2][2] = {};
  for (int m = 0; m < 2; ++m) {
    const FillModel fm = m == 0 ? FillModel::Naive : FillModel::QueueAware;
    std::printf("\n=== %s fill model ===\n", m == 0 ? "NAIVE" : "QUEUE-AWARE");
    std::printf("  %-24s %9s %11s %9s %9s %9s   %s\n",
                "policy", "fills", "shares", "markout", "bid mo", "ask mo",
                "vs baseline (paired 95% CI)");
    std::vector<Outcome> outs;
    for (int i = 0; i < kNPol; ++i)
      outs.push_back(run(tape, win, size, latency, fm, policies[i], mids));

    for (int i = 0; i < kNPol; ++i) {
      const Outcome& o = outs[i];
      std::printf("  %-24s %9" PRIu64 " %11" PRIu64 " %9.3f %9.3f %9.3f",
                  policies[i].name, o.fills, o.shares, o.markout,
                  o.bid_markout, o.ask_markout);
      if (i > 0) {
        const Interval d = paired_block_bootstrap(to_obs(o.obs), to_obs(outs[0].obs), kBlock);
        std::printf("   %+7.3f [%+7.3f,%+7.3f] %s", d.point, d.lo, d.hi,
                    d.excludes_zero() ? "SIGNIF" : "n.s.");
        if (i == 1) skew_cost[m] = d.point;
      }
      std::printf("\n");
    }
    // The decisive comparison: the two directions against each other. Both pay
    // the same queue-position cost for pulling a quote, so differencing them
    // cancels it and leaves only the signal.
    for (int th = 0; th < 2; ++th) {
      const Interval d = paired_block_bootstrap(to_obs(outs[2 + 2 * th].obs),
                                                to_obs(outs[1 + 2 * th].obs), kBlock);
      std::printf("  -> THIN minus HEAVY at threshold %.2f: %+.3f bps "
                  "[%+.3f,%+.3f] %s\n",
                  policies[1 + 2 * th].threshold, d.point, d.lo, d.hi,
                  d.excludes_zero() ? "SIGNIFICANT" : "not significant");
      thin_minus_heavy[m][th] = d.point;
      thin_sig[m][th] = d.excludes_zero();
    }
  }
  std::printf("\n  quoting the THIN side rather than the HEAVY side is worth:\n");
  for (int th = 0; th < 2; ++th)
    std::printf("    threshold %.2f : %+.3f bps under NAIVE fills%s,  %+.3f bps under QUEUE-AWARE%s\n",
                policies[1 + 2 * th].threshold,
                thin_minus_heavy[0][th], thin_sig[0][th] ? " (signif)" : "",
                thin_minus_heavy[1][th], thin_sig[1][th] ? " (signif)" : "");

  std::printf("\n-- the question this repo exists to answer --\n");
  std::printf("  Both columns ran the same strategy code, the same signal, the same\n");
  std::printf("  threshold and the same latency. Only the fill rule differed.\n");
  return 0;
}
