#include "hotpath/core/open_hash.hpp"

#include <catch2/catch_test_macros.hpp>
#include <random>
#include <unordered_map>
#include <vector>

using hotpath::OpenHashMap;

TEST_CASE("open hash: basic insert/find/erase", "[hash]") {
  OpenHashMap<std::uint32_t> m(1024);
  REQUIRE(m.size() == 0);
  REQUIRE(m.find(42) == nullptr);

  REQUIRE(m.insert(42, 7) != nullptr);
  REQUIRE(m.size() == 1);
  REQUIRE(m.find(42) != nullptr);
  REQUIRE(*m.find(42) == 7);

  REQUIRE(m.insert(42, 9) != nullptr);      // replace in place
  REQUIRE(m.size() == 1);
  REQUIRE(*m.find(42) == 9);

  REQUIRE(m.erase(42));
  REQUIRE(m.size() == 0);
  REQUIRE(m.find(42) == nullptr);
  REQUIRE_FALSE(m.erase(42));
}

TEST_CASE("open hash: rejects non-power-of-two capacity", "[hash]") {
  REQUIRE_THROWS(OpenHashMap<int>(1000));
  REQUIRE_THROWS(OpenHashMap<int>(0));
}

TEST_CASE("open hash: refuses to exceed load factor rather than growing", "[hash]") {
  OpenHashMap<std::uint32_t> m(16);           // 70% of 16 == 11 usable
  std::size_t inserted = 0;
  for (std::uint64_t k = 1; k <= 32; ++k) {
    if (m.insert(k, static_cast<std::uint32_t>(k))) ++inserted;
  }
  REQUIRE(inserted < 32);
  REQUIRE(m.size() == inserted);
  // Everything that reported success must still be findable.
  std::size_t found = 0;
  for (std::uint64_t k = 1; k <= 32; ++k) if (m.find(k)) ++found;
  REQUIRE(found == inserted);
}

// The backward-shift deletion is the part most likely to be subtly wrong: a
// bad shift silently strands entries that are still present but no longer
// reachable, which would show up much later as phantom "orphan" ITCH messages.
// Differential-test it against std::unordered_map over a long random workload.
TEST_CASE("open hash: backward-shift deletion matches a reference map", "[hash][stress]") {
  constexpr std::size_t kSlots = 4096;
  OpenHashMap<std::uint64_t> m(kSlots);
  std::unordered_map<std::uint64_t, std::uint64_t> ref;

  std::mt19937_64 rng(12345);
  std::vector<std::uint64_t> live;

  for (int iter = 0; iter < 200000; ++iter) {
    const int op = static_cast<int>(rng() % 100);
    if (op < 55 || live.empty()) {
      const std::uint64_t k = (rng() % 100000) + 1;   // never 0: that is the sentinel
      const std::uint64_t v = rng();
      if (ref.size() < (kSlots * 6) / 10) {
        if (m.insert(k, v)) {
          if (ref.find(k) == ref.end()) live.push_back(k);
          ref[k] = v;
        }
      }
    } else {
      const std::size_t idx = rng() % live.size();
      const std::uint64_t k = live[idx];
      live[idx] = live.back();
      live.pop_back();
      const bool a = m.erase(k);
      const bool b = ref.erase(k) > 0;
      REQUIRE(a == b);
    }

    if (iter % 5000 == 0) {
      REQUIRE(m.size() == ref.size());
      for (const auto& [k, v] : ref) {
        const std::uint64_t* got = m.find(k);
        REQUIRE(got != nullptr);
        REQUIRE(*got == v);
      }
    }
  }

  REQUIRE(m.size() == ref.size());
  for (const auto& [k, v] : ref) {
    const std::uint64_t* got = m.find(k);
    REQUIRE(got != nullptr);
    REQUIRE(*got == v);
  }
}
