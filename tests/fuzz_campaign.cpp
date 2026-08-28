// Long-running fuzz campaign. The test suite runs a few thousand seeded
// iterations to stay fast; this runs as many as you like, and is the thing to
// point at a machine overnight.
//
// Build it in the ASAN configuration -- without a sanitizer the ITCH target
// mostly proves the parser terminates, which is the least interesting of its
// properties:
//
//   ./build-asan/tests/fuzz_campaign --iterations 200000 --seed 1
//
// A failure prints the seed and iteration, which is enough to reproduce it
// exactly: the generators are deterministic given a seed.
#include "fuzz_core.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char** argv) {
  std::uint64_t iterations = 20000;
  std::uint64_t seed = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
  }

  std::printf("fuzz campaign: %" PRIu64 " iterations from seed %" PRIu64 "\n", iterations, seed);
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t failures = 0, bytes = 0;

  for (std::uint64_t i = 0; i < iterations; ++i) {
    hotpath::fuzz::Rng r(seed + i * 0x9E3779B97F4A7C15ull);
    const int which = static_cast<int>(r.below(3));
    std::vector<std::uint8_t> input;
    const char* kind = "";
    switch (which) {
      case 0: input = hotpath::fuzz::random_bytes(r, r.below(1024) + 1); kind = "random"; break;
      case 1: input = hotpath::fuzz::mutated_itch(r, 1 + r.below(48));   kind = "mutated-itch"; break;
      default: input = hotpath::fuzz::random_bytes(r, r.below(4096) + 8); kind = "book-script"; break;
    }
    bytes += input.size();

    try {
      if (which == 2) hotpath::fuzz::fuzz_book_differential(input.data(), input.size());
      else            (void)hotpath::fuzz::fuzz_itch_parse(input.data(), input.size());
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL  iteration=%" PRIu64 " seed=%" PRIu64 " kind=%s size=%zu\n        %s\n",
                  i, static_cast<std::uint64_t>(seed + i * 0x9E3779B97F4A7C15ull),
                  kind, input.size(), e.what());
      if (failures >= 10) { std::printf("stopping after 10 failures\n"); break; }
    }
    if ((i & 0xFFFF) == 0xFFFF)
      std::printf("  ... %" PRIu64 " iterations, %" PRIu64 " failures\n", i + 1, failures);
  }

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("\n%" PRIu64 " iterations, %.1f MB of input, %.1fs (%.0f iter/s)\n",
              iterations, static_cast<double>(bytes) / 1e6, secs,
              static_cast<double>(iterations) / (secs > 0 ? secs : 1));
  std::printf("failures: %" PRIu64 "\n", failures);
  return failures ? 1 : 0;
}
