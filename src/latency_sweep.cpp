// What does re-quote latency actually cost a passive market maker?
//
// This is the measurement that connects the systems half of this repo to the
// trading half. Every nanosecond shaved off a tick-to-trade path exists to
// reduce one number: the delay between seeing the touch move and having your
// quote in the right place. This sweeps that number and reports what it buys.
//
// Two distinct mechanisms are at work, and they push in opposite directions:
//
//   1. You join later, so more participants are ahead of you in the queue.
//      Fewer fills.
//   2. Your OLD quote is still resting at a stale price while the replacement
//      is in flight. Those fills are the ones you least want -- the market has
//      already moved and you are the last stale quote standing. More fills, and
//      badly selected ones.
//
// Markout at a 10s horizon is used as the P&L proxy. Note it already includes
// the spread capture: buying at the bid with an unchanged mid gives a positive
// markout of half the spread. So a negative mean markout means adverse
// selection has eaten the entire edge.
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/sim/market_maker.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
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
  return it == s.begin() ? -1.0 : (it - 1)->mid;
}

struct Row {
  Ts latency;
  std::uint64_t fills{0}, shares{0};
  std::uint64_t stale_fills{0}, stale_shares{0};
  std::uint64_t swept_fills{0};
  double markout_bps{0};        // share-weighted
  double stale_markout_bps{0};
  double fresh_markout_bps{0};
  double pnl_dollars{0};
  std::uint64_t requotes{0};
};

} // namespace

int main(int argc, char** argv) {
  std::string path;
  Qty size = 100;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--size" && i + 1 < argc) size = static_cast<Qty>(std::atoi(argv[++i]));
    else path = a;
  }
  if (path.empty()) { std::fprintf(stderr, "usage: latency_sweep <SYM.tape> [--size N]\n"); return 2; }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();
  const BookEvent* ev = tape.events();

  // A mid series is needed to mark fills out. It does not depend on our quotes
  // (we never enter the book), so it is built once from a zero-latency run and
  // reused for every latency point -- which also keeps the comparison exact.
  std::vector<MidSample> mids;
  mids.reserve(tape.size() / 4);
  {
    MarketMaker mm(win.lo, win.hi, size, 0);
    double last = -1.0;
    for (std::size_t i = 0; i < tape.size(); ++i) {
      mm.on_event(ev[i], [](const Fill&) {});
      if (mm.have_mid() && mm.mid() != last) { mids.push_back({ev[i].ts, mm.mid()}); last = mm.mid(); }
    }
  }

  const Ts lat[] = {0, 1'000, 5'000, 10'000, 50'000, 100'000,
                    500'000, 1'000'000, 5'000'000, 10'000'000};
  std::vector<Row> rows;

  for (Ts L : lat) {
    Row r; r.latency = L;
    MarketMaker mm(win.lo, win.hi, size, L);
    double wsum = 0, wstale = 0, wfresh = 0, dollars = 0;
    std::uint64_t wn = 0, wns = 0, wnf = 0;

    for (std::size_t i = 0; i < tape.size(); ++i) {
      mm.on_event(ev[i], [&](const Fill& f) {
        r.fills += 1; r.shares += f.qty;
        if (f.stale) { r.stale_fills += 1; r.stale_shares += f.qty; }
        if (f.swept) r.swept_fills += 1;

        const double m = mid_at(mids, f.ts + 10 * kSec);
        if (m < 0) return;
        const double sign = f.side == Side::Buy ? 1.0 : -1.0;
        const double diff = sign * (m - static_cast<double>(f.price));
        const double bps = 1e4 * diff / m;
        wsum += bps * f.qty; wn += f.qty;
        if (f.stale) { wstale += bps * f.qty; wns += f.qty; }
        else         { wfresh += bps * f.qty; wnf += f.qty; }
        dollars += diff * static_cast<double>(f.qty) / 1e4;   // prices are 1/10000 dollar
      });
    }
    r.requotes = mm.requotes();
    r.markout_bps       = wn  ? wsum   / static_cast<double>(wn)  : 0.0;
    r.stale_markout_bps = wns ? wstale / static_cast<double>(wns) : 0.0;
    r.fresh_markout_bps = wnf ? wfresh / static_cast<double>(wnf) : 0.0;
    r.pnl_dollars = dollars;
    rows.push_back(r);
  }

  std::printf("tape        : %s\n", path.c_str());
  std::printf("events      : %zu\n", tape.size());
  std::printf("quote size  : %u shares\n", size);
  std::printf("P&L proxy   : 10s markout x filled shares (includes spread capture)\n");

  std::printf("\n  %-10s %9s %11s %10s %9s %11s %11s %11s\n",
              "latency", "fills", "shares", "stale%", "swept", "markout", "fresh mo", "P&L ($)");
  for (const Row& r : rows) {
    char lbl[32];
    if (r.latency == 0) std::snprintf(lbl, sizeof lbl, "0");
    else if (r.latency < 1'000'000) std::snprintf(lbl, sizeof lbl, "%" PRIu64 " us", r.latency / 1000);
    else std::snprintf(lbl, sizeof lbl, "%" PRIu64 " ms", r.latency / 1'000'000);
    std::printf("  %-10s %9" PRIu64 " %11" PRIu64 " %9.1f%% %9" PRIu64 " %11.3f %11.3f %11.2f\n",
                lbl, r.fills, r.shares,
                r.shares ? 100.0 * static_cast<double>(r.stale_shares) / static_cast<double>(r.shares) : 0.0,
                r.swept_fills, r.markout_bps, r.fresh_markout_bps, r.pnl_dollars);
  }

  if (!rows.empty() && rows.front().pnl_dollars != 0.0) {
    std::printf("\n  relative to zero latency:\n");
    const double base = rows.front().pnl_dollars;
    for (const Row& r : rows) {
      if (r.latency == 0) continue;
      char lbl[32];
      if (r.latency < 1'000'000) std::snprintf(lbl, sizeof lbl, "%" PRIu64 " us", r.latency / 1000);
      else std::snprintf(lbl, sizeof lbl, "%" PRIu64 " ms", r.latency / 1'000'000);
      std::printf("    %-8s  P&L %+8.2f  (%+6.1f%%)   stale markout %8.3f bps\n",
                  lbl, r.pnl_dollars, 100.0 * (r.pnl_dollars - base) / (base < 0 ? -base : base),
                  r.stale_markout_bps);
    }
  }
  return 0;
}
