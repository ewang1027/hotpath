// Phase 4: queue-position-aware fill simulation and adverse-selection measurement.
//
// A passive market maker joins the touch on both sides and re-quotes when the
// touch moves. Two fill models run side by side on identical order flow:
//
//   naive  -- fills whenever the price trades, up to the quoted size. This is
//             what most candidate backtests do, implicitly.
//   queue  -- fills only once cumulative executed volume exceeds the volume
//             resting ahead of us, with the queue also shortening when orders
//             AHEAD of us are cancelled.
//
// Then every fill is marked out against the mid 1s / 10s / 60s later. A
// passive maker that is being adversely selected shows negative markouts: the
// fills you get are disproportionately the ones you did not want.
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/sim/queue_model.hpp"

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

struct Fill {
  Ts    ts;
  Price price;
  Qty   qty;
  Side  side;
  Qty   queue_ahead_at_join;   // how deep we were when we joined this level
};

struct MidSample { Ts ts; double mid; };

struct Quote {
  bool  active{false};
  Price px{0};
  Qty   remaining{0};
  Qty   ahead_at_join{0};
};

struct ModelStats {
  std::uint64_t fills{0};
  std::uint64_t shares{0};
  std::uint64_t requotes{0};
};

// mid at or before `t`; -1 if we have no sample that late (fill too near the close)
double mid_at(const std::vector<MidSample>& s, Ts t) {
  if (s.empty() || t > s.back().ts) return -1.0;
  const auto it = std::upper_bound(s.begin(), s.end(), t,
                                   [](Ts v, const MidSample& m) { return v < m.ts; });
  if (it == s.begin()) return -1.0;
  return (it - 1)->mid;
}

struct Markout {
  double sum_bps{0};
  double sum_ticks{0};
  std::uint64_t n{0};
  void add(double bps, double ticks) { sum_bps += bps; sum_ticks += ticks; ++n; }
  [[nodiscard]] double mean_bps() const { return n ? sum_bps / static_cast<double>(n) : 0.0; }
  [[nodiscard]] double mean_ticks() const { return n ? sum_ticks / static_cast<double>(n) : 0.0; }
};

void report_markouts(const char* label, const std::vector<Fill>& fills,
                     const std::vector<MidSample>& mids) {
  const Ts horizons[3] = {1 * kSec, 10 * kSec, 60 * kSec};
  const char* names[3] = {"1s", "10s", "60s"};

  std::printf("\n  %s -- markout by horizon (positive = we were right, negative = adversely selected)\n", label);
  std::printf("    %-6s %10s %12s %12s %12s %12s\n",
              "horiz", "fills", "all (bps)", "buys (bps)", "sells (bps)", "all (ticks)");
  for (int h = 0; h < 3; ++h) {
    Markout all, buys, sells;
    for (const Fill& f : fills) {
      const double m = mid_at(mids, f.ts + horizons[h]);
      if (m < 0) continue;
      const double sign = f.side == Side::Buy ? 1.0 : -1.0;
      const double diff = sign * (m - static_cast<double>(f.price));
      const double bps = 1e4 * diff / m;
      const double ticks = diff / static_cast<double>(HybridBook::kTick);
      all.add(bps, ticks);
      (f.side == Side::Buy ? buys : sells).add(bps, ticks);
    }
    std::printf("    %-6s %10" PRIu64 " %12.3f %12.3f %12.3f %12.4f\n",
                names[h], all.n, all.mean_bps(), buys.mean_bps(), sells.mean_bps(),
                all.mean_ticks());
  }
}

// Markout split by how deep in the queue we were when we joined. If queue
// position carries information, shallow joins and deep joins should not look
// the same.
void report_by_queue_depth(const std::vector<Fill>& fills,
                           const std::vector<MidSample>& mids) {
  struct Bucket { const char* name; Qty lo, hi; Markout m; };
  Bucket b[] = {
      {"0 (alone)",     0,      0},
      {"1-100",         1,      100},
      {"101-500",       101,    500},
      {"501-2000",      501,    2000},
      {">2000",         2001,   0xFFFFFFFFu},
  };
  std::printf("\n  queue-aware -- 10s markout by depth of queue ahead at join\n");
  std::printf("    %-12s %10s %12s\n", "ahead", "fills", "markout(bps)");
  for (const Fill& f : fills) {
    const double m = mid_at(mids, f.ts + 10 * kSec);
    if (m < 0) continue;
    const double sign = f.side == Side::Buy ? 1.0 : -1.0;
    const double diff = sign * (m - static_cast<double>(f.price));
    for (auto& bk : b) {
      if (f.queue_ahead_at_join >= bk.lo && f.queue_ahead_at_join <= bk.hi) {
        bk.m.add(1e4 * diff / m, diff / HybridBook::kTick);
        break;
      }
    }
  }
  for (const auto& bk : b)
    std::printf("    %-12s %10" PRIu64 " %12.3f\n", bk.name, bk.m.n, bk.m.mean_bps());
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  Qty quote_size = 100;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--size" && i + 1 < argc) quote_size = static_cast<Qty>(std::atoi(argv[++i]));
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: sim_mm <SYM.tape> [--size SHARES]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();
  HybridBook book(win.lo, win.hi, 1u << 21, 1u << 20);

  Quote qb, qa, nb, na;                       // queue-aware and naive quotes
  QueuePosition pos_bid, pos_ask;
  ModelStats st_q, st_n;
  std::vector<Fill> fills_q, fills_n;
  std::vector<MidSample> mids;
  mids.reserve(tape.size() / 4);

  double last_mid = -1.0;
  const BookEvent* ev = tape.events();

  for (std::size_t i = 0; i < tape.size(); ++i) {
    const BookEvent& e = ev[i];

    // Resolve the affected order BEFORE mutating the book: after apply() the
    // order may be gone, and we need its side and price to know whether the
    // event touched a level we are quoting on.
    bool have_pre = false;
    Side pre_side = Side::Buy;
    Price pre_px = 0;
    if (e.type != EventType::Add) {
      if (const auto* o = book.find_order(e.order_ref)) {
        if (o->level >= 0) {
          have_pre = true;
          pre_side = o->side;
          pre_px = book.level_by_index(o->level).price;
        }
      }
    }

    apply(book, e);

    // ---- fills against our resting quotes ----
    if (have_pre) {
      auto handle = [&](Quote& q, QueuePosition& pos, Side side, ModelStats& st,
                        std::vector<Fill>& out) {
        if (!q.active || pre_side != side || pre_px != q.px) return;
        switch (e.type) {
          case EventType::Execute: {
            const Qty f = pos.on_execution(e.order_ref, e.shares, q.remaining);
            if (f) {
              out.push_back(Fill{e.ts, q.px, f, side, q.ahead_at_join});
              st.fills += 1; st.shares += f;
              q.remaining -= f;
              if (q.remaining == 0) { q.active = false; pos.leave(); }
            }
            break;
          }
          case EventType::Cancel:  pos.on_cancel(e.order_ref, e.shares); break;
          case EventType::Delete:  pos.on_delete(e.order_ref); break;
          case EventType::Replace: pos.on_delete(e.order_ref); break;
          default: break;
        }
      };
      handle(qb, pos_bid, Side::Buy, st_q, fills_q);
      handle(qa, pos_ask, Side::Sell, st_q, fills_q);

      // Naive model: any execution at our price fills us, queue ignored.
      if (e.type == EventType::Execute) {
        auto naive = [&](Quote& q, Side side) {
          if (!q.active || pre_side != side || pre_px != q.px) return;
          const Qty f = std::min<Qty>(q.remaining, e.shares);
          if (!f) return;
          fills_n.push_back(Fill{e.ts, q.px, f, side, 0});
          st_n.fills += 1; st_n.shares += f;
          q.remaining -= f;
          if (q.remaining == 0) q.active = false;
        };
        naive(nb, Side::Buy);
        naive(na, Side::Sell);
      }
    }

    // ---- re-quote to the touch ----
    const Price bb = book.best_bid();
    const Price ba = book.best_ask();
    if (bb == kInvalidPrice || ba == kInvalidPrice || bb >= ba) continue;

    auto requote_queue = [&](Quote& q, QueuePosition& pos, Side side, Price target) {
      if (q.active && q.px == target) return;
      // Re-quoting means going to the BACK of the new level's queue. Losing
      // queue position on every touch move is most of why passive fills are
      // hard, and a model that keeps its position across re-quotes is cheating.
      const auto* L = book.level_for(side, target);
      const Qty resting = L ? L->qty : 0;
      pos.join(resting);
      if (L) {
        for (std::int32_t oi = L->head; oi >= 0; oi = book.order_at(oi).next)
          pos.note_ahead(book.order_at(oi).id, book.order_at(oi).qty);
      }
      q.active = true; q.px = target; q.remaining = quote_size; q.ahead_at_join = resting;
      ++st_q.requotes;
    };
    auto requote_naive = [&](Quote& q, Price target, ModelStats& st) {
      if (q.active && q.px == target) return;
      q.active = true; q.px = target; q.remaining = quote_size;
      ++st.requotes;
    };

    requote_queue(qb, pos_bid, Side::Buy, bb);
    requote_queue(qa, pos_ask, Side::Sell, ba);
    requote_naive(nb, bb, st_n);
    requote_naive(na, ba, st_n);

    const double mid = (static_cast<double>(bb) + static_cast<double>(ba)) / 2.0;
    if (mid != last_mid) { mids.push_back(MidSample{e.ts, mid}); last_mid = mid; }
  }

  std::printf("tape          : %s\n", path.c_str());
  std::printf("events        : %zu\n", tape.size());
  std::printf("quote size    : %u shares, joining the touch on both sides\n", quote_size);
  std::printf("mid samples   : %zu\n", mids.size());
  if (!mids.empty())
    std::printf("session       : %.2f h .. %.2f h after midnight\n",
                mids.front().ts / 3.6e12, mids.back().ts / 3.6e12);

  std::printf("\n-- fill counts --\n");
  std::printf("  %-14s %10s %14s %12s\n", "model", "fills", "shares", "re-quotes");
  std::printf("  %-14s %10" PRIu64 " %14" PRIu64 " %12" PRIu64 "\n",
              "naive", st_n.fills, st_n.shares, st_n.requotes);
  std::printf("  %-14s %10" PRIu64 " %14" PRIu64 " %12" PRIu64 "\n",
              "queue-aware", st_q.fills, st_q.shares, st_q.requotes);
  if (st_q.shares)
    std::printf("\n  the naive model overstates filled volume by %.1fx\n",
                static_cast<double>(st_n.shares) / static_cast<double>(st_q.shares));

  report_markouts("naive", fills_n, mids);
  report_markouts("queue-aware", fills_q, mids);
  report_by_queue_depth(fills_q, mids);
  return 0;
}
