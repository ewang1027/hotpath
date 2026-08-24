#include "fixture.hpp"

#include "hotpath/bench/counters.hpp"
#include "hotpath/bench/timing.hpp"
#include "hotpath/itch/byteorder.hpp"
#include "hotpath/itch/messages.hpp"
#include "hotpath/itch/reader.hpp"
#include "hotpath/itch/symbol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

using namespace hotpath;
using namespace hotpath::itch;
using hotpath::test::ItchBuilder;

TEST_CASE("byteorder: big-endian reads", "[itch]") {
  const std::uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  REQUIRE(rd_u16(b) == 0x0102u);
  REQUIRE(rd_u32(b) == 0x01020304u);
  REQUIRE(rd_u64(b) == 0x0102030405060708ull);
  REQUIRE(rd_u48(b) == 0x010203040506ull);
}

TEST_CASE("symbol: 8-byte space padded round trip", "[itch]") {
  REQUIRE(Symbol::from_text("AAPL").text() == "AAPL");
  REQUIRE(Symbol::from_text("A").text() == "A");
  REQUIRE(Symbol::from_text("ABCDEFGH").text() == "ABCDEFGH");
  REQUIRE(Symbol::from_text("AAPL") == Symbol::from_text("AAPL"));
  REQUIRE(Symbol::from_text("AAPL") != Symbol::from_text("MSFT"));
}

// Every offset in messages.hpp is a chance to be off by one. Build a message
// with distinct values in every field and assert each one reads back.
TEST_CASE("itch: AddOrder field offsets", "[itch]") {
  ItchBuilder b;
  b.add_order(/*ref=*/0x1122334455667788ull, Side::Buy, /*shares=*/12345,
              "AAPL", /*px=*/1502500, /*locate=*/7, /*ts=*/0x0000AABBCCDDull);

  Reader r(b.bytes().data(), b.bytes().size());
  RawMessage m{};
  REQUIRE(r.next(m));
  REQUIRE(m.type() == 'A');
  REQUIRE(m.length == spec_length('A'));

  const AddOrderView v{m.body};
  REQUIRE(v.locate() == 7);
  REQUIRE(v.timestamp() == 0x0000AABBCCDDull);
  REQUIRE(v.order_ref() == 0x1122334455667788ull);
  REQUIRE(v.side() == Side::Buy);
  REQUIRE(v.shares() == 12345u);
  REQUIRE(v.stock() == Symbol::from_text("AAPL"));
  REQUIRE(v.price() == 1502500u);
  REQUIRE_FALSE(r.next(m));
}

TEST_CASE("itch: execute / cancel / delete / replace offsets", "[itch]") {
  ItchBuilder b;
  b.order_executed(/*ref=*/111, /*shares=*/50, /*match=*/0xDEADBEEFull);
  b.order_cancel(/*ref=*/222, /*shares=*/25);
  b.order_delete(/*ref=*/333);
  b.order_replace(/*orig=*/444, /*neu=*/555, /*shares=*/77, /*px=*/999999);

  Reader r(b.bytes().data(), b.bytes().size());
  RawMessage m{};

  REQUIRE(r.next(m));
  REQUIRE(m.type() == 'E');
  const OrderExecutedView e{m.body};
  REQUIRE(e.order_ref() == 111u);
  REQUIRE(e.executed_shares() == 50u);
  REQUIRE(e.match_number() == 0xDEADBEEFull);

  REQUIRE(r.next(m));
  REQUIRE(m.type() == 'X');
  const OrderCancelView c{m.body};
  REQUIRE(c.order_ref() == 222u);
  REQUIRE(c.cancelled_shares() == 25u);

  REQUIRE(r.next(m));
  REQUIRE(m.type() == 'D');
  REQUIRE(OrderDeleteView{m.body}.order_ref() == 333u);

  REQUIRE(r.next(m));
  REQUIRE(m.type() == 'U');
  const OrderReplaceView u{m.body};
  REQUIRE(u.original_ref() == 444u);
  REQUIRE(u.new_ref() == 555u);
  REQUIRE(u.shares() == 77u);
  REQUIRE(u.price() == 999999u);

  REQUIRE_FALSE(r.next(m));
}

TEST_CASE("itch: spec_length table agrees with what the builder emits", "[itch]") {
  ItchBuilder b;
  b.system_event('O');
  b.add_order(1, Side::Sell, 100, "MSFT", 3000000);
  b.order_executed(1, 10, 1);
  b.order_cancel(1, 10, 1);
  b.order_delete(1);
  b.order_replace(1, 2, 5, 10);

  Reader r(b.bytes().data(), b.bytes().size());
  RawMessage m{};
  int seen = 0;
  while (r.next(m)) {
    REQUIRE(spec_length(m.type()) != 0);
    REQUIRE(m.length == spec_length(m.type()));
    ++seen;
  }
  REQUIRE(seen == 6);
  REQUIRE(r.stats().length_mismatch == 0);
  REQUIRE(r.stats().unknown_type == 0);
  REQUIRE(r.stats().truncated == 0);
}

TEST_CASE("itch: truncated tail is detected, not walked off the end", "[itch]") {
  ItchBuilder b;
  b.add_order(1, Side::Buy, 100, "AAPL", 1000000);
  auto bytes = b.bytes();
  bytes.resize(bytes.size() - 4);            // chop the last 4 bytes

  Reader r(bytes.data(), bytes.size());
  RawMessage m{};
  REQUIRE_FALSE(r.next(m));
  REQUIRE(r.stats().truncated == 1);
  REQUIRE(r.stats().messages == 0);
}

TEST_CASE("itch: unknown message type is skipped via the length prefix", "[itch]") {
  ItchBuilder b;
  b.add_order(1, Side::Buy, 100, "AAPL", 1000000);
  // A type the spec table does not know, 5 bytes of body. Framing must carry us
  // past it without desynchronising.
  b.add({std::uint8_t('~'), 0, 0, 0, 0});
  b.order_delete(1);

  Reader r(b.bytes().data(), b.bytes().size());
  RawMessage m{};
  REQUIRE(r.next(m)); REQUIRE(m.type() == 'A');
  REQUIRE(r.next(m)); REQUIRE(m.type() == '~');
  REQUIRE(r.next(m)); REQUIRE(m.type() == 'D');
  REQUIRE_FALSE(r.next(m));
  REQUIRE(r.stats().unknown_type == 1);
  REQUIRE(r.stats().length_mismatch == 0);
}

TEST_CASE("itch: the parse loop allocates nothing", "[itch][invariant]") {
  REQUIRE(bench::alloc_counting_active());
  REQUIRE(bench::syscall_counting_active());

  ItchBuilder b;
  for (int i = 1; i <= 5000; ++i) {
    b.add_order(std::uint64_t(i), i % 2 ? Side::Buy : Side::Sell,
                std::uint32_t(100 + i), "AAPL", std::uint32_t(1000000 + i));
  }
  const auto& bytes = b.bytes();

  Reader r(bytes.data(), bytes.size());
  RawMessage m{};
  std::uint64_t acc = 0;

  bench::CounterScope scope;               // snapshot AFTER all setup
  while (r.next(m)) {
    const AddOrderView v{m.body};
    acc += v.price() + v.shares();
  }
  const auto d = scope.delta();

  REQUIRE(acc > 0);                        // the loop actually ran
  REQUIRE(d.allocations == 0);
  REQUIRE(d.syscalls == 0);
}

// Guards the guard. If instrumentation ever stops taking effect -- a static
// link, a dropped dylib, a dead-stripped __interpose section -- every
// zero-allocation assertion in this suite would start passing vacuously.
TEST_CASE("instrumentation actually counts", "[invariant]") {
  REQUIRE(bench::alloc_counting_active());
  REQUIRE(bench::syscall_counting_active());

  SECTION("allocations are observed") {
    bench::CounterScope s;
    void* p = ::operator new(1024);
    bench::do_not_optimize(p);
    ::operator delete(p);
    REQUIRE(s.delta().allocations >= 1);
  }

  SECTION("syscalls are observed") {
    bench::CounterScope s;
    (void)::close(-1);
    REQUIRE(s.delta().syscalls >= 1);
  }
}
