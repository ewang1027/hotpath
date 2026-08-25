// Do the microstructure signals actually predict anything?
//
// This runs before any strategy work on purpose. If queue imbalance and order
// flow imbalance do not forecast the short-horizon mid, then skewing quotes on
// them is theatre, and it is better to find that out from a table than from a
// backtest that "works".
//
// Sampling is on a fixed time grid, not per event. Events arrive in bursts, so
// per-event sampling would weight busy microseconds hundreds of times more than
// quiet seconds and turn autocorrelation into an apparent signal.
#include "hotpath/book/hybrid_book.hpp"
#include "hotpath/book/tape.hpp"
#include "hotpath/core/platform.hpp"
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

struct Sample {
  Ts     ts;
  double mid;
  double imbalance;
  double micro_tilt;
  double ofi;
};

double pearson(const std::vector<double>& a, const std::vector<double>& b) {
  const std::size_t n = a.size();
  if (n < 2) return 0.0;
  double ma = 0, mb = 0;
  for (std::size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
  ma /= static_cast<double>(n); mb /= static_cast<double>(n);
  double num = 0, da = 0, db = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = a[i] - ma, y = b[i] - mb;
    num += x * y; da += x * x; db += y * y;
  }
  return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
}

// Forward mid return in bps, or NaN when the horizon runs past the session.
double fwd_return_bps(const std::vector<Sample>& s, std::size_t i, Ts horizon) {
  const Ts target = s[i].ts + horizon;
  if (target > s.back().ts) return std::nan("");
  const auto it = std::lower_bound(s.begin() + static_cast<std::ptrdiff_t>(i), s.end(), target,
                                   [](const Sample& x, Ts v) { return x.ts < v; });
  if (it == s.end()) return std::nan("");
  return 1e4 * (it->mid - s[i].mid) / s[i].mid;
}

void report(const char* name, const std::vector<Sample>& s,
            double (*get)(const Sample&), const Ts* hz, const char** hzname, int nh) {
  std::printf("\n-- %s --\n", name);
  std::printf("  %-8s %10s %10s   %s\n", "horizon", "n", "corr", "decile response (mean fwd return, bps)");
  for (int h = 0; h < nh; ++h) {
    std::vector<double> sig, ret;
    sig.reserve(s.size()); ret.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
      const double r = fwd_return_bps(s, i, hz[h]);
      if (std::isnan(r)) continue;
      sig.push_back(get(s[i]));
      ret.push_back(r);
    }
    if (sig.size() < 100) { std::printf("  %-8s (too few samples)\n", hzname[h]); continue; }

    const double c = pearson(sig, ret);

    // Decile response: sort by signal, average the forward return in each tenth.
    // A monotone staircase is far more convincing than a correlation, because a
    // single fat tail can manufacture a correlation but not a staircase.
    std::vector<std::size_t> idx(sig.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return sig[a] < sig[b]; });

    std::printf("  %-8s %10zu %10.4f  ", hzname[h], sig.size(), c);
    const std::size_t per = idx.size() / 10;
    for (int d = 0; d < 10; ++d) {
      const std::size_t lo = static_cast<std::size_t>(d) * per;
      const std::size_t hi = (d == 9) ? idx.size() : lo + per;
      double m = 0;
      for (std::size_t k = lo; k < hi; ++k) m += ret[idx[k]];
      m /= static_cast<double>(hi - lo);
      std::printf("%+6.3f ", m);
    }
    std::printf("\n");
  }
}

} // namespace

int main(int argc, char** argv) {
  std::string path;
  Ts grid_ns = 100'000'000;   // 100 ms
  double tau_ns = 1e9;        // OFI decay
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--grid-ms" && i + 1 < argc) grid_ns = std::strtoull(argv[++i], nullptr, 10) * 1'000'000ull;
    else if (a == "--tau-ms" && i + 1 < argc) tau_ns = std::strtod(argv[++i], nullptr) * 1e6;
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: signal_study <SYM.tape> [--grid-ms N] [--tau-ms N]\n");
    return 2;
  }

  request_performance_cores();
  Tape tape(path);
  const auto win = tape.price_window();
  HybridBook book(win.lo, win.hi, 1u << 21, 1u << 20);
  OrderFlowImbalance ofi(tau_ns);

  std::vector<Sample> samples;
  samples.reserve(tape.size() / 8);
  Ts next_sample = 0;

  const BookEvent* ev = tape.events();
  for (std::size_t i = 0; i < tape.size(); ++i) {
    apply(book, ev[i]);

    const Price bb = book.best_bid(), ba = book.best_ask();
    if (bb == kInvalidPrice || ba == kInvalidPrice || bb >= ba) continue;
    const auto* lb = book.level_for(Side::Buy, bb);
    const auto* la = book.level_for(Side::Sell, ba);
    if (!lb || !la) continue;

    const Touch t{bb, ba, lb->qty, la->qty};
    if (!t.valid()) continue;
    ofi.update(t, ev[i].ts);

    if (next_sample == 0) next_sample = ev[i].ts + grid_ns;
    if (ev[i].ts >= next_sample) {
      samples.push_back(Sample{ev[i].ts, t.mid(), t.imbalance(), t.micro_tilt(), ofi.normalized()});
      // Skip forward rather than emitting a burst if the book was quiet.
      while (next_sample <= ev[i].ts) next_sample += grid_ns;
    }
  }

  std::printf("tape        : %s\n", path.c_str());
  std::printf("events      : %zu\n", tape.size());
  std::printf("samples     : %zu on a %" PRIu64 " ms grid\n", samples.size(), grid_ns / 1'000'000);
  std::printf("OFI tau     : %.0f ms\n", tau_ns / 1e6);
  if (samples.size() < 200) { std::printf("\nnot enough samples to say anything\n"); return 1; }
  std::printf("session     : %.2f h .. %.2f h\n",
              samples.front().ts / 3.6e12, samples.back().ts / 3.6e12);
  std::printf("\nDecile columns run from the most negative signal to the most positive.\n"
              "A real signal shows a monotone staircase, not just a correlation.\n");

  const Ts hz[] = {100'000'000ull, 1'000'000'000ull, 10'000'000'000ull};
  const char* hzn[] = {"100ms", "1s", "10s"};
  report("queue imbalance  (Qb-Qa)/(Qb+Qa)", samples,
         [](const Sample& s) { return s.imbalance; }, hz, hzn, 3);
  report("microprice tilt  (micro-mid)/halfspread", samples,
         [](const Sample& s) { return s.micro_tilt; }, hz, hzn, 3);
  report("order flow imbalance (normalised)", samples,
         [](const Sample& s) { return s.ofi; }, hz, hzn, 3);
  return 0;
}
