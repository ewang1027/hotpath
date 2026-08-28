// Structural statistics for a per-symbol tape.
//
// These numbers are quoted in docs/, so they live in a committed tool rather
// than a throwaway script. Two of them explain results elsewhere:
//
//   * book depth and level churn explain why the sorted-vector level index in
//     the intrusive design loses (docs/PERFORMANCE.md finding 1);
//   * the inter-event time distribution explains why microsecond-scale re-quote
//     latency already matters (docs/ADVERSE-SELECTION.md) -- executions arrive
//     in bursts far tighter than the median event spacing.
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/intrusive_book.hpp"
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

using namespace hotpath;
using namespace hotpath::book;

namespace {
std::uint64_t quantile(std::vector<std::uint64_t>& v, double f) {
  if (v.empty()) return 0;
  return v[static_cast<std::size_t>(f * static_cast<double>(v.size() - 1))];
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: tape_stat <SYM.tape>\n"); return 2; }
  request_performance_cores();
  Tape tape(argv[1]);
  const auto win = tape.price_window();
  const BookEvent* ev = tape.events();

  // ---- inter-event timing ----
  std::vector<std::uint64_t> gaps, gaps_exec;
  Ts prev = 0; bool first = true;
  std::uint64_t per_type[5]{};
  for (std::size_t i = 0; i < tape.size(); ++i) {
    const BookEvent& e = ev[i];
    ++per_type[static_cast<int>(e.type)];
    if (!first && e.ts >= prev) {
      gaps.push_back(e.ts - prev);
      if (e.type == EventType::Execute) gaps_exec.push_back(e.ts - prev);
    }
    prev = e.ts; first = false;
  }
  std::sort(gaps.begin(), gaps.end());
  std::sort(gaps_exec.begin(), gaps_exec.end());
  std::uint64_t sub_us = 0, sub_10us = 0;
  for (auto g : gaps) { if (g < 1000) ++sub_us; if (g < 10000) ++sub_10us; }

  // ---- book depth and level churn ----
  IntrusiveBook ib(1u << 21, 1u << 20);
  std::uint64_t sum_levels = 0, max_levels = 0;
  for (std::size_t i = 0; i < tape.size(); ++i) {
    apply(ib, ev[i]);
    const std::size_t lv = ib.bid_levels() + ib.ask_levels();
    sum_levels += lv;
    if (lv > max_levels) max_levels = lv;
  }

  const double n = static_cast<double>(tape.size());
  std::printf("tape              : %s\n", argv[1]);
  std::printf("events            : %zu\n", tape.size());
  std::printf("price window      : $%.2f .. $%.2f (%zu penny ticks, %.2f%% of adds inside)\n",
              win.lo / 1e4, win.hi / 1e4,
              static_cast<std::size_t>((win.hi - win.lo) / 100) + 1, 100.0 * win.covered);

  std::printf("\n-- event mix --\n");
  const char* names[5] = {"Add", "Execute", "Cancel", "Delete", "Replace"};
  for (int i = 0; i < 5; ++i)
    std::printf("  %-9s %12" PRIu64 "  %5.2f%%\n", names[i], per_type[i], 100.0 * static_cast<double>(per_type[i]) / n);

  std::printf("\n-- inter-event time (ns) --\n");
  std::printf("  all events        p10=%-10" PRIu64 " p50=%-10" PRIu64 " p90=%" PRIu64 "\n",
              quantile(gaps, 0.10), quantile(gaps, 0.50), quantile(gaps, 0.90));
  std::printf("  preceding EXECUTE p10=%-10" PRIu64 " p50=%-10" PRIu64 " p90=%" PRIu64 "\n",
              quantile(gaps_exec, 0.10), quantile(gaps_exec, 0.50), quantile(gaps_exec, 0.90));
  std::printf("  gaps < 1us: %.1f%%   < 10us: %.1f%%\n",
              100.0 * static_cast<double>(sub_us) / static_cast<double>(gaps.size()),
              100.0 * static_cast<double>(sub_10us) / static_cast<double>(gaps.size()));
  std::printf("  => executions cluster far tighter than the median event spacing,\n");
  std::printf("     which is why microsecond re-quote latency already bites.\n");

  std::printf("\n-- book depth / level churn --\n");
  std::printf("  mean live levels  : %.1f   max %" PRIu64 "\n", static_cast<double>(sum_levels) / n, max_levels);
  std::printf("  level creates     : %" PRIu64 "  (%.3f per event)\n",
              ib.level_creates(), static_cast<double>(ib.level_creates()) / n);
  std::printf("  elements shifted  : %" PRIu64 "  (%.1f per create/destroy)\n",
              ib.elements_shifted(),
              static_cast<double>(ib.elements_shifted()) /
                  static_cast<double>(ib.level_creates() + ib.level_destroys()));
  std::printf("  bytes memmoved    : %.1f MB  (%.2f KB per event)\n",
              static_cast<double>(ib.elements_shifted()) * 4.0 / 1e6,
              static_cast<double>(ib.elements_shifted()) * 4.0 / n / 1e3);
  return 0;
}
