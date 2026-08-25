#include "fixture.hpp"
#include "fuzz_core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace hotpath;

using fuzz::Rng;
using fuzz::random_bytes;
using fuzz::mutated_itch;


// Regression corpus: inputs that once broke something. Kept as literal bytes so
// they run on every build regardless of the random seed.
TEST_CASE("fuzz: regression corpus", "[fuzz]") {
  SECTION("short message claiming a long type") {
    // A 4-byte body claiming to be an Add ('A', spec length 36). Every typed
    // accessor reads at a fixed offset, so handing this body to AddOrderView
    // read 28 bytes past the buffer -- a heap-buffer-overflow confirmed under
    // ASan. The Reader now refuses to hand out an under-length body.
    const std::uint8_t bytes[] = {0x00, 0x04, 'A', 0x00, 0x00, 0x00};
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(bytes, sizeof bytes));
  }
  SECTION("short message of an UNKNOWN type") {
    // Found by the fuzz campaign, not by reasoning. The first fix only rejected
    // under-length bodies of types with a known spec length; a 3-byte message
    // of an unrecognised type had no spec length to check against and was still
    // handed out, so any caller reading Header::timestamp() (6 bytes at offset
    // 5) read past the buffer. Every ITCH message carries an 11-byte common
    // header, so 11 is the floor for any type at all.
    const std::uint8_t bytes[] = {0x00, 0x03, 0x7E, 0x11, 0x22};
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(bytes, sizeof bytes));
  }
  SECTION("every length below the 11-byte header, for every type byte") {
    for (int t = 0; t < 256; ++t) {
      for (std::uint16_t len = 1; len < 11; ++len) {
        std::vector<std::uint8_t> v{static_cast<std::uint8_t>(len >> 8),
                                    static_cast<std::uint8_t>(len & 0xff),
                                    static_cast<std::uint8_t>(t)};
        v.resize(2 + len, 0xAB);
        INFO("type=" << t << " len=" << len);
        REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(v.data(), v.size()));
      }
    }
  }
  SECTION("length prefix promising more than the buffer holds") {
    const std::uint8_t bytes[] = {0xFF, 0xFF, 'A', 0x01};
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(bytes, sizeof bytes));
  }
  SECTION("zero-length frame mid-stream") {
    const std::uint8_t bytes[] = {0x00, 0x0C, 'S', 0,0,0,0, 0,0,0,0,0,0, 'O',
                                  0x00, 0x00,
                                  0x00, 0x0C, 'S', 0,0,0,0, 0,0,0,0,0,0, 'O'};
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(bytes, sizeof bytes));
  }
  SECTION("empty and one-byte inputs") {
    const std::uint8_t one[] = {0x41};
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(nullptr, 0));
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(one, 1));
    REQUIRE_NOTHROW(fuzz::fuzz_book_differential(one, 1));
  }
}

// Deterministic seeds so a failure is reproducible from the test name alone.
// Under AddressSanitizer these have teeth: an out-of-bounds read anywhere in
// the parser fails the build rather than returning a plausible number.
TEST_CASE("fuzz: ITCH parser survives arbitrary bytes", "[fuzz]") {
  Rng r(0xC0FFEE);
  for (int i = 0; i < 3000; ++i) {
    const auto v = random_bytes(r, r.below(512) + 1);
    INFO("iteration " << i << ", " << v.size() << " random bytes");
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(v.data(), v.size()));
  }
}

TEST_CASE("fuzz: ITCH parser survives corrupted valid streams", "[fuzz]") {
  Rng r(0xBADC0DE);
  for (int i = 0; i < 3000; ++i) {
    const auto v = mutated_itch(r, 1 + r.below(24));
    INFO("iteration " << i << ", " << v.size() << " bytes of mutated ITCH");
    REQUIRE_NOTHROW(fuzz::fuzz_itch_parse(v.data(), v.size()));
  }
}

// The automated form of the cross-validation gate. The hand-written version
// caught the intrusive book silently dropping orders when its level pool
// filled; this one re-runs that experiment on thousands of generated scripts.
TEST_CASE("fuzz: four book designs agree on generated event scripts", "[fuzz]") {
  Rng r(0x5EED);
  for (int i = 0; i < 400; ++i) {
    const auto v = random_bytes(r, r.below(2048) + 8);
    INFO("iteration " << i << ", " << v.size() << " bytes");
    REQUIRE_NOTHROW(fuzz::fuzz_book_differential(v.data(), v.size()));
  }
}
