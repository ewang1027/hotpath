// Decompose the cost of the full-day parse.
//
// itch_stat reports ~35 ns/message end to end, but that number is three things
// added together and optimising it blind is guesswork. This separates them:
//
//   framing   -- walk the length prefixes only, touch no fields
//   decode    -- framing plus reading every field of every book message
//   index     -- decode plus maintaining the live-order hash map
//
// The gap between decode and index is the cost of one random probe into a
// multi-hundred-megabyte table per message, which is where the time actually
// is.
#include "hotpath/bench/timing.hpp"
#include "hotpath/core/open_hash.hpp"
#include "hotpath/core/platform.hpp"
#include "hotpath/itch/mapped_file.hpp"
#include "hotpath/itch/reader.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hotpath;
using namespace hotpath::itch;
using namespace hotpath::bench;

namespace {

struct LiveOrder { Qty shares; };

// The key a message will probe the order index with, or 0 if it does not
// probe one. Cheap enough to compute twice: it is two byte-swapped loads.
inline std::uint64_t probe_key(const RawMessage& m) noexcept {
  switch (m.type()) {
    case 'A': case 'F': return AddOrderView{m.body}.order_ref();
    case 'E': case 'C': return OrderExecutedView{m.body}.order_ref();
    case 'X':           return OrderCancelView{m.body}.order_ref();
    case 'D':           return OrderDeleteView{m.body}.order_ref();
    case 'U':           return OrderReplaceView{m.body}.original_ref();
    default:            return 0;
  }
}

double time_pass(const MappedFile& f, int mode, std::size_t table_pow2,
                 std::uint64_t& messages, std::size_t& peak, int lookahead = 0) {
  Reader reader(f.data(), f.size());
  RawMessage m{};
  OpenHashMap<LiveOrder> live(table_pow2);
  std::uint64_t sink = 0;
  peak = 0;

  // Software-pipelined variant: keep `lookahead` messages in flight, issuing a
  // prefetch for each as it enters the window and processing it only once it
  // leaves. The miss is then started ~lookahead messages before the load that
  // needs it, which is the whole trick -- the stream is sequential, so reading
  // ahead is free, while the table probe is random and is not.
  // Power-of-two window so the wrap is a mask. The first version used
  // `% window.size()` on a runtime value -- an integer division per message,
  // which is 20-40 cycles on this core and made the "optimisation" 2.7x SLOWER
  // than no prefetch at all (28.4 -> 76.5 ns/msg).
  std::size_t wcap = 1;
  while (wcap < static_cast<std::size_t>(lookahead > 0 ? lookahead : 1)) wcap <<= 1;
  const std::size_t wmask = wcap - 1;
  std::vector<RawMessage> window(wcap);
  std::size_t filled = 0, head = 0;

  const std::uint64_t t0 = now_ticks();
  while (true) {
    if (lookahead > 0) {
      while (filled < wcap) {
        RawMessage nxt{};
        if (!reader.next(nxt)) break;
        const std::size_t slot = (head + filled) & wmask;
        window[slot] = nxt;
        if (mode == 2) {
          const std::uint64_t k = probe_key(nxt);
          if (k) live.prefetch(k);
        }
        ++filled;
      }
      if (filled == 0) break;
      m = window[head];
      head = (head + 1) & wmask;
      --filled;
    } else {
      if (!reader.next(m)) break;
    }
    if (mode == 0) { sink += m.length; continue; }          // framing only
    switch (m.type()) {
      case 'A': case 'F': {
        const AddOrderView v{m.body};
        sink += v.order_ref() + v.shares() + v.price();
        if (mode == 2) {
          live.insert(v.order_ref(), LiveOrder{v.shares()});
          if (live.size() > peak) peak = live.size();
        }
        break;
      }
      case 'E': case 'C': {
        const OrderExecutedView v{m.body};
        sink += v.order_ref() + v.executed_shares();
        if (mode == 2) {
          if (LiveOrder* o = live.find(v.order_ref()))
            if ((o->shares -= (v.executed_shares() < o->shares ? v.executed_shares() : o->shares)) == 0)
              live.erase(v.order_ref());
        }
        break;
      }
      case 'X': {
        const OrderCancelView v{m.body};
        sink += v.order_ref() + v.cancelled_shares();
        if (mode == 2) {
          if (LiveOrder* o = live.find(v.order_ref()))
            if ((o->shares -= (v.cancelled_shares() < o->shares ? v.cancelled_shares() : o->shares)) == 0)
              live.erase(v.order_ref());
        }
        break;
      }
      case 'D': {
        const OrderDeleteView v{m.body};
        sink += v.order_ref();
        if (mode == 2) live.erase(v.order_ref());
        break;
      }
      case 'U': {
        const OrderReplaceView v{m.body};
        sink += v.original_ref() + v.new_ref() + v.shares() + v.price();
        if (mode == 2) {
          live.erase(v.original_ref());
          live.insert(v.new_ref(), LiveOrder{v.shares()});
        }
        break;
      }
      default: break;
    }
  }
  const double ns = ticks_to_ns(now_ticks() - t0);
  do_not_optimize(sink);
  messages = reader.stats().messages;
  return ns / static_cast<double>(messages);
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  std::size_t table_pow2 = 1u << 24;
  int trials = 3;
  int lookahead = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--table" && i + 1 < argc) table_pow2 = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--trials" && i + 1 < argc) trials = std::atoi(argv[++i]);
    else if (a == "--lookahead" && i + 1 < argc) lookahead = std::atoi(argv[++i]);
    else path = a;
  }
  if (path.empty()) { std::fprintf(stderr, "usage: bench_parse <file.NASDAQ_ITCH50> [--table N]\n"); return 2; }

  request_performance_cores();
  MappedFile f(path);
  std::printf("file  : %s (%.2f GiB)\n", path.c_str(),
              static_cast<double>(f.size()) / (1024.0 * 1024 * 1024));
  std::printf("look  : %d messages of prefetch lookahead\n", lookahead);
  std::printf("table : %zu slots (%.0f MB)\n", table_pow2,
              static_cast<double>(table_pow2) * sizeof(OpenHashMap<LiveOrder>::Slot) / 1e6);

  const char* names[3] = {"framing only", "+ field decode", "+ order index"};
  std::uint64_t msgs = 0;
  std::size_t peak = 0;

  // One discarded pass to fault the whole 7.7 GB mapping into the page cache.
  // Without it the first timed pass is measuring disk, and the difference is
  // large enough to reverse conclusions -- an early version of this benchmark
  // reported the index stage at 20.7 ns and then 11.1 ns for the same table.
  (void)time_pass(f, 0, 1024, msgs, peak);

  double prev = 0;
  for (int mode = 0; mode < 3; ++mode) {
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(trials));
    for (int k = 0; k < trials; ++k)
      t.push_back(time_pass(f, mode, table_pow2, msgs, peak, lookahead));
    const Stats st = summarize(t);
    std::printf("  %-16s %7.2f ns/msg  +/-%.2f  %6.2f M msg/s", names[mode],
                st.mean, st.ci95(), 1e3 / st.mean);
    if (mode) std::printf("   (+%.2f ns for this stage)", st.mean - prev);
    std::printf("\n");
    prev = st.mean;
  }
  std::printf("  messages: %" PRIu64 "   peak live orders: %zu (load %.3f)\n",
              msgs, peak, static_cast<double>(peak) / static_cast<double>(table_pow2));
  return 0;
}
