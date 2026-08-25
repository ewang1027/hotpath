// Queue-position-aware fill simulation and adverse-selection measurement.
//
// Two fill models over identical order flow:
//   naive  -- fills whenever the price trades, up to the quoted size. What a
//             backtest does implicitly when it has trade prints but no
//             order-by-order book.
//   queue  -- MarketMaker: fills only once cumulative executed volume exceeds
//             the volume resting ahead of us, with the queue also advancing
//             when orders AHEAD are cancelled.
//
// The queue-aware path runs the shared sim::MarketMaker, the same code the
// threaded pipeline drives. Keeping a second copy here would mean the numbers
// in the docs and the numbers the pipeline reproduces could quietly diverge.
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/sim/bootstrap.hpp"
#include "hotpath/sim/market_maker.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace hotpath;
using namespace hotpath::book;
using namespace hotpath::sim;

namespace {

constexpr Ts kSec = 1'000'000'000ull;

struct MidSample { Ts ts; double mid; };

double mid_at(const std::vector<MidSample>& s, Ts t) {
  if (s.empty() || t > s.back().ts) return -1.0;
  const auto it = std::upper_bound(s.begin(), s.end(), t,
                                   [](Ts v, const MidSample& m) { return v < m.ts; });
  if (it == s.begin()) return -1.0;
  return (it - 1)->mid;
}

double markout_bps(const Fill& f, const std::vector<MidSample>& mids, Ts horizon) {
  const double m = mid_at(mids, f.ts + horizon);
  if (m < 0) return 1e18;                      // sentinel: no sample that late
  const double sign = f.side == Side::Buy ? 1.0 : -1.0;
  return 1e4 * sign * (m - static_cast<double>(f.price)) / m;
}

// 5-minute blocks: long enough that fills in different blocks are close to
// independent, short enough to leave ~190 blocks in a session to resample.
constexpr Ts kBlock = 300 * kSec;

// Turn fills into weighted observations for the bootstrap. Weighting by share
// count matters: a 1-share fill and a 500-share fill are not equal evidence
// about the strategy's economics.
std::vector<WeightedObs> to_obs(const std::vector<Fill>& fills,
                                const std::vector<MidSample>& mids, Ts horizon,
                                int side_filter = -1) {
  std::vector<WeightedObs> out;
  out.reserve(fills.size());
  for (const Fill& f : fills) {
    if (side_filter >= 0 && static_cast<int>(f.side) != side_filter) continue;
    const double b = markout_bps(f, mids, horizon);
    if (b > 1e17) continue;
    out.push_back(WeightedObs{f.ts, b, static_cast<double>(f.qty)});
  }
  return out;
}

void report_markouts(const char* label, const std::vector<Fill>& fills,
                     const std::vector<MidSample>& mids) {
  const Ts hz[3] = {1 * kSec, 10 * kSec, 60 * kSec};
  const char* names[3] = {"1s", "10s", "60s"};
  std::printf("\n  %s -- markout, share-weighted, 95%% block-bootstrap CI\n", label);
  std::printf("    (negative = adversely selected; an interval spanning 0 means the\n");
  std::printf("     sign is not established on this one session)\n");
  std::printf("    %-6s %9s %9s %18s %9s %9s\n",
              "horiz", "fills", "all", "95% CI", "buys", "sells");
  for (int h = 0; h < 3; ++h) {
    const Interval all = block_bootstrap(to_obs(fills, mids, hz[h]), kBlock);
    const Interval buy = block_bootstrap(to_obs(fills, mids, hz[h], 0), kBlock);
    const Interval sel = block_bootstrap(to_obs(fills, mids, hz[h], 1), kBlock);
    std::printf("    %-6s %9" PRIu64 " %9.3f  [%7.3f,%7.3f]%s %9.3f %9.3f\n",
                names[h], all.n, all.point, all.lo, all.hi,
                all.excludes_zero() ? "*" : " ", buy.point, sel.point);
  }
  std::printf("    * interval excludes zero\n");
}

void report_by_queue_depth(const std::vector<Fill>& fills,
                           const std::vector<MidSample>& mids) {
  struct Bucket { const char* name; Qty lo, hi; std::vector<Fill> f; };
  Bucket b[] = {
      {"0 (alone)", 0, 0, {}}, {"1-100", 1, 100, {}}, {"101-500", 101, 500, {}},
      {"501-2000", 501, 2000, {}}, {">2000", 2001, 0xFFFFFFFFu, {}},
  };
  for (const Fill& f : fills)
    for (auto& bk : b)
      if (f.queue_ahead_at_join >= bk.lo && f.queue_ahead_at_join <= bk.hi) {
        bk.f.push_back(f); break;
      }

  std::printf("\n  queue-aware -- 10s markout by queue depth at join\n");
  std::printf("    %-12s %9s %9s %18s\n", "ahead", "fills", "markout", "95% CI");
  for (const auto& bk : b) {
    const Interval iv = block_bootstrap(to_obs(bk.f, mids, 10 * kSec), kBlock);
    std::printf("    %-12s %9" PRIu64 " %9.3f  [%7.3f,%7.3f]%s\n",
                bk.name, iv.n, iv.point, iv.lo, iv.hi, iv.excludes_zero() ? "*" : " ");
  }

  // The actual claim is that deeper queue positions fill worse. Reading it off
  // the bucket means is not a test -- the buckets have wildly different sample
  // sizes. Both buckets come from the same session, so resampling the same time
  // blocks for both is a genuine paired comparison.
  const Interval d = paired_block_bootstrap(to_obs(b[3].f, mids, 10 * kSec),
                                            to_obs(b[1].f, mids, 10 * kSec), kBlock);
  std::printf("    deep (501-2000) minus shallow (1-100): %+.3f [%+.3f,%+.3f] %s\n",
              d.point, d.lo, d.hi,
              d.excludes_zero() ? "SIGNIFICANT" : "not significant");
}

// Fills split by how they were obtained. Under latency the interesting bucket
// is "stale": a quote we had already decided to move but had not yet replaced.
void report_by_kind(const std::vector<Fill>& fills, const std::vector<MidSample>& mids) {
  std::vector<Fill> fresh, stale, swept;
  for (const Fill& f : fills) {
    (f.stale ? stale : fresh).push_back(f);
    if (f.swept) swept.push_back(f);
  }
  std::printf("\n  queue-aware -- 10s markout by how the fill was obtained\n");
  std::printf("    %-32s %9s %9s %18s\n", "kind", "fills", "markout", "95% CI");
  const std::pair<const char*, const std::vector<Fill>*> rows[] = {
      {"fresh quote", &fresh},
      {"stale (replace in flight)", &stale},
      {"swept (we were more aggressive)", &swept},
  };
  for (const auto& [name, v] : rows) {
    const Interval iv = block_bootstrap(to_obs(*v, mids, 10 * kSec), kBlock);
    std::printf("    %-32s %9" PRIu64 " %9.3f  [%7.3f,%7.3f]%s\n",
                name, iv.n, iv.point, iv.lo, iv.hi, iv.excludes_zero() ? "*" : " ");
  }
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  Qty quote_size = 100;
  Ts latency = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--size" && i + 1 < argc) quote_size = static_cast<Qty>(std::atoi(argv[++i]));
    else if (a == "--latency" && i + 1 < argc) latency = std::strtoull(argv[++i], nullptr, 10);
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: sim_mm <SYM.tape> [--size SHARES] [--latency NS]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();
  const BookEvent* ev = tape.events();

  // ---- pass 1: queue-aware ----
  MarketMaker mm(win.lo, win.hi, quote_size, latency);
  std::vector<Fill> fills_q;
  std::vector<MidSample> mids;
  mids.reserve(tape.size() / 4);
  double last_mid = -1.0;
  for (std::size_t i = 0; i < tape.size(); ++i) {
    mm.on_event(ev[i], [&](const Fill& f) { fills_q.push_back(f); });
    if (mm.have_mid() && mm.mid() != last_mid) {
      mids.push_back(MidSample{ev[i].ts, mm.mid()});
      last_mid = mm.mid();
    }
  }

  // ---- pass 2: naive, its own book so the two models cannot interact ----
  HybridBook nb(win.lo, win.hi, 1u << 21, 1u << 20);
  struct NQ { bool active{false}; Price px{0}; Qty remaining{0}; };
  NQ nbid, nask;
  std::vector<Fill> fills_n;
  std::uint64_t naive_requotes = 0;
  for (std::size_t i = 0; i < tape.size(); ++i) {
    const BookEvent& e = ev[i];
    bool have_pre = false; Side pre_side = Side::Buy; Price pre_px = 0;
    if (e.type != EventType::Add) {
      if (const auto* o = nb.find_order(e.order_ref)) {
        if (o->level >= 0) { have_pre = true; pre_side = o->side; pre_px = nb.level_by_index(o->level).price; }
      }
    }
    apply(nb, e);
    if (have_pre && e.type == EventType::Execute) {
      auto hit = [&](NQ& q, Side side) {
        if (!q.active || pre_side != side || pre_px != q.px) return;
        const Qty f = std::min<Qty>(q.remaining, e.shares);
        if (!f) return;
        fills_n.push_back(Fill{e.ts, q.px, f, side, 0, false, false});
        q.remaining -= f;
        if (q.remaining == 0) q.active = false;
      };
      hit(nbid, Side::Buy);
      hit(nask, Side::Sell);
    }
    const Price bb = nb.best_bid(), ba = nb.best_ask();
    if (bb == kInvalidPrice || ba == kInvalidPrice || bb >= ba) continue;
    auto rq = [&](NQ& q, Price target) {
      if (q.active && q.px == target) return;
      q.active = true; q.px = target; q.remaining = quote_size; ++naive_requotes;
    };
    rq(nbid, bb);
    rq(nask, ba);
  }

  std::uint64_t sh_q = 0, sh_n = 0;
  for (const auto& f : fills_q) sh_q += f.qty;
  for (const auto& f : fills_n) sh_n += f.qty;

  std::printf("tape          : %s\n", path.c_str());
  std::printf("events        : %zu\n", tape.size());
  std::printf("quote size    : %u shares, joining the touch on both sides\n", quote_size);
  std::printf("re-quote latency: %" PRIu64 " ns\n", latency);
  std::printf("mid samples   : %zu\n", mids.size());

  std::printf("\n-- fill counts --\n");
  std::printf("  %-14s %10s %14s %12s\n", "model", "fills", "shares", "re-quotes");
  std::printf("  %-14s %10zu %14" PRIu64 " %12" PRIu64 "\n", "naive", fills_n.size(), sh_n, naive_requotes);
  std::printf("  %-14s %10zu %14" PRIu64 " %12" PRIu64 "\n", "queue-aware", fills_q.size(), sh_q, mm.requotes());
  if (sh_q)
    std::printf("\n  the naive model overstates filled volume by %.1fx\n",
                static_cast<double>(sh_n) / static_cast<double>(sh_q));
  // Model-assumption check, not a result: how often an order that arrived after
  // we joined traded while volume we believe is ahead of us still rested. Under
  // strict price-time priority this is impossible.
  std::printf("  strict-FIFO violations observed: %" PRIu64
              " (see docs/ADVERSE-SELECTION.md)\n", mm.behind_while_queued());

  report_markouts("naive", fills_n, mids);
  report_markouts("queue-aware", fills_q, mids);

  // Both models score the same session, so this is paired too.
  const Interval mdl = paired_block_bootstrap(to_obs(fills_q, mids, 10 * kSec),
                                              to_obs(fills_n, mids, 10 * kSec), kBlock);
  std::printf("\n  queue-aware minus naive, 10s markout: %+.3f [%+.3f,%+.3f] %s\n",
              mdl.point, mdl.lo, mdl.hi,
              mdl.excludes_zero() ? "SIGNIFICANT" : "not significant");
  report_by_queue_depth(fills_q, mids);
  report_by_kind(fills_q, mids);
  return 0;
}
