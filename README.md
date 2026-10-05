# hotpath

C++20 code that replays one day of NASDAQ TotalView-ITCH 5.0 data (2019-12-30,
8.25 GB, 268,744,780 messages) and runs experiments on it: four order book
designs compared on identical input, a passive market-making simulation with a
FIFO queue-position fill model, and some lock-free ring and memory-ordering
tests. It is an offline replay on a laptop (Apple M5 Pro), not a live trading
system. There is no network code, so "tick-to-trade" in the docs means
parse-to-decision inside one process.

The results below are regenerated from the raw file by `./scripts/regenerate.sh`.

## Results

| Experiment | Result | Details |
|---|---|---|
| Pooled intrusive book (sorted level vector) vs `std::map` | slower on 7 of 25 symbols, up to 1.9x (AMZN) | [PERFORMANCE](docs/PERFORMANCE.md) |
| What predicts that ratio | elements shifted per event (level creates/event × mean depth): Pearson r = -0.927 on a log scale | [PERFORMANCE](docs/PERFORMANCE.md#the-predictor-across-25-symbols) |
| Price grid + per-level intrusive FIFO vs `std::map` | faster on 25 of 25, 1.96x to 3.89x (median 2.62x) | [PERFORMANCE](docs/PERFORMANCE.md) |
| Naive fill model (fill whenever the price trades) vs FIFO queue tracking | overstates passive volume 3.4x to 15.6x (median 5.7x), worst where real fills are rarest | [ADVERSE-SELECTION](docs/ADVERSE-SELECTION.md) |
| 10s markout of simulated passive fills | negative on 25 of 25 symbols; SPY's is far smaller than AAPL's and its interval includes zero | [ADVERSE-SELECTION](docs/ADVERSE-SELECTION.md) |
| Quoting one side based on queue imbalance | it predicts the 1s mid move, but quoting with it vs against it splits 16 to 9 across symbols (sign test p = 0.23) | [SIGNALS](docs/SIGNALS.md) |
| 3-thread pipeline over SPSC rings vs one thread | 1.30x to 1.62x slower on 4 symbols, identical fills | [PERFORMANCE](docs/PERFORMANCE.md#threaded-tick-to-trade-pipeline) |

How far to trust these:

- It is all one trading day. The 25 symbols share that session, so they are not
  independent samples, and the sign tests and block-bootstrap intervals only
  describe variation within the day.
- Speeds are mean ns/event over repeated full replays. The M5 Pro's userspace
  clock ticks every 41.7 ns, so no per-event latency percentiles are reported;
  [METHODOLOGY](docs/METHODOLOGY.md) explains what is and isn't measured.
- The fill model has no self-impact, assumes strict FIFO from the displayed
  book, and uses a fixed re-quote delay with no jitter.
- The grid books allocate nothing for prices inside their window. Prices outside
  it go to a `std::map` overflow, which does allocate (3,868 times over the AAPL
  replay), so they are not allocation-free overall.

## Memory ordering

The SPSC ring takes its memory ordering as a template parameter, and
`litmus_ring` runs a deliberately wrong `relaxed` version next to
release/acquire. Release/acquire has been clean in every run. How often the
relaxed version visibly breaks depends on the machine: 1,463 premature and 544
torn reads per 10.5M messages on the M5 Pro, 0 to 15 per 5.2M on GitHub's
macos-14 arm64 runners (5 of 18 CI runs were clean, including the latest), and
0 on the x86-64 CI runners, where TSO hides it. A clean run proves nothing:
with relaxed ordering the payload write and read are a data race, which is
undefined behavior in C++ on any CPU.

## Correctness checks

- The four book designs are compared on the ten-deep snapshot after every event:
  0 divergences over 18.8M events across 25 symbols. This is how the intrusive
  book was caught silently dropping orders when its level pool filled.
- Full-day parse: 0 orphaned order references over 268.7M messages.
- Replay is deterministic: identical snapshot digests over 20 runs at `-O3` and
  one at `-O0`, on 4 tapes.
- CI builds with `-Werror` on AppleClang, GCC and Clang, runs the tests under
  ASan/UBSan and TSan, and runs a short fuzz campaign. The market data is too
  large for CI, so the real-data checks run locally through `regenerate.sh`.
- `rtl/itch_parse.sv` is a SystemVerilog version of the parser, co-simulated
  against the C++ one under Verilator (0 mismatches on 2,057,603 real messages).
  It is simulated only, not synthesized ([RTL](docs/RTL.md)).

## Build

```bash
brew install cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/hotpath_tests

./scripts/fetch_data.sh 12302019      # ~3.5 GB gz -> 8.25 GB raw, resumable
./scripts/regenerate.sh               # every number in docs/
```

Market data lives outside the repo (`$HOTPATH_DATA_DIR`, default
`~/market-data/itch`). Linux builds are covered in [PORTING](docs/PORTING.md).
