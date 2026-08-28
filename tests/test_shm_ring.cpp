#include "hotpath/bench/timing.hpp"
#include "hotpath/ipc/shm_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hotpath::ipc;

namespace {
std::string tmp_ring(const char* tag) {
  return "/tmp/hotpath_test_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".ring";
}
struct Cleanup {
  std::string path;
  ~Cleanup() { ::unlink(path.c_str()); }
};
} // namespace

TEST_CASE("shm ring: round trip through the mapping", "[shm]") {
  const auto path = tmp_ring("rt"); Cleanup c{path};
  auto pub = ShmRing::create(path, 64, sizeof(std::uint64_t));
  auto sub = ShmRing::open(path);

  std::uint64_t next = 0, gap = 0, got = 0;
  REQUIRE(sub.try_read(next, &got, sizeof got, gap) == ShmRing::Status::Empty);

  for (std::uint64_t i = 1; i <= 10; ++i) pub.publish(&i, sizeof i);
  for (std::uint64_t i = 1; i <= 10; ++i) {
    REQUIRE(sub.try_read(next, &got, sizeof got, gap) == ShmRing::Status::Ok);
    REQUIRE(got == i);
  }
  REQUIRE(next == 10);
  REQUIRE(sub.try_read(next, &got, sizeof got, gap) == ShmRing::Status::Empty);
}

TEST_CASE("shm ring: rejects a bad file and a bad capacity", "[shm]") {
  REQUIRE_THROWS(ShmRing::create("/tmp/hotpath_badcap.ring", 100, 8));
  REQUIRE_THROWS(ShmRing::open("/tmp/definitely_not_a_ring_file_12345"));
}

// The property that distinguishes this from SpscRing. A consumer that falls
// behind is NOT allowed to quietly read stale or torn data, and the producer is
// NOT allowed to slow down for it.
TEST_CASE("shm ring: a lapped consumer is told, not silently corrupted", "[shm]") {
  const auto path = tmp_ring("gap"); Cleanup c{path};
  constexpr std::uint64_t kCap = 16;
  auto pub = ShmRing::create(path, kCap, sizeof(std::uint64_t));
  auto sub = ShmRing::open(path);

  // Publish four laps' worth without reading a single message.
  for (std::uint64_t i = 0; i < kCap * 4; ++i) pub.publish(&i, sizeof i);

  std::uint64_t next = 0, gap = 0, got = 0;
  const auto st = sub.try_read(next, &got, sizeof got, gap);
  REQUIRE(st == ShmRing::Status::Gap);
  REQUIRE(gap > 0);
  // Resynchronised to something still resident, not to garbage.
  REQUIRE(next >= kCap * 3);
  REQUIRE(next <= kCap * 4);

  // And it can carry on from there without further loss.
  const auto st2 = sub.try_read(next, &got, sizeof got, gap);
  REQUIRE(st2 == ShmRing::Status::Ok);
  REQUIRE(got == next - 1);
}

TEST_CASE("shm ring: every message is either delivered or counted as a gap",
          "[shm][thread]") {
  const auto path = tmp_ring("acct"); Cleanup c{path};
  constexpr std::uint64_t kN = 200000;
  auto pub = ShmRing::create(path, 1024, sizeof(std::uint64_t));
  auto sub = ShmRing::open(path);

  std::atomic<bool> done{false};
  std::uint64_t received = 0, gapped = 0, out_of_order = 0;

  std::thread consumer([&] {
    std::uint64_t next = 0, gap = 0, got = 0, last = 0;
    while (next < kN) {
      const auto st = sub.try_read(next, &got, sizeof got, gap);
      if (st == ShmRing::Status::Ok) {
        ++received;
        if (got != next - 1) ++out_of_order;   // payload must match its sequence
        last = got;
      } else if (st == ShmRing::Status::Gap) {
        gapped += gap;
      } else if (done.load(std::memory_order_acquire) && next >= sub.published()) {
        break;
      }
    }
    (void)last;
  });

  for (std::uint64_t i = 0; i < kN; ++i) pub.publish(&i, sizeof i);
  done.store(true, std::memory_order_release);
  consumer.join();

  REQUIRE(out_of_order == 0);
  // Nothing may be invented or double-counted: delivered + missed accounts for
  // everything the consumer advanced past.
  REQUIRE(received + gapped == kN);
}

TEST_CASE("shm ring: the producer does not slow down for a slow consumer",
          "[shm][thread]") {
  const auto path = tmp_ring("nb"); Cleanup c{path};
  constexpr std::uint64_t kN = 500000;

  auto measure = [&](bool with_slow_consumer) {
    ::unlink(path.c_str());
    auto pub = ShmRing::create(path, 256, sizeof(std::uint64_t));
    std::atomic<bool> stop{false};
    std::thread consumer;
    if (with_slow_consumer) {
      consumer = std::thread([&] {
        auto sub = ShmRing::open(path);
        std::uint64_t next = 0, gap = 0, got = 0;
        while (!stop.load(std::memory_order_acquire)) {
          (void)sub.try_read(next, &got, sizeof got, gap);
          for (int k = 0; k < 200; ++k)                     // deliberately sluggish
            hotpath::bench::do_not_optimize(k);
        }
      });
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < kN; ++i) pub.publish(&i, sizeof i);
    const double ns = std::chrono::duration<double, std::nano>(
                          std::chrono::steady_clock::now() - t0).count();
    stop.store(true, std::memory_order_release);
    if (consumer.joinable()) consumer.join();
    return ns / static_cast<double>(kN);
  };

  const double alone = measure(false);
  const double behind = measure(true);
  INFO("ns/publish alone=" << alone << " with a slow consumer=" << behind);
  // A blocking ring would stall here; this one overwrites. Allow generous
  // headroom for scheduling noise -- the claim is "does not stall", not
  // "identical to the nanosecond".
  REQUIRE(behind < alone * 5.0 + 200.0);
}
