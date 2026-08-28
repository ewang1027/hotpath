#include "hotpath/sim/queue_model.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace hotpath;
using namespace hotpath::sim;

// Orders resting at a level when we join carry insertion stamps below the one
// we capture; anything arriving later carries a higher stamp. These tests use
// seq 1..9 for "already there" and 100+ for "arrived after us".
constexpr std::uint32_t kJoin = 10;
constexpr std::uint32_t kAfter = 100;   // arrived after we joined

TEST_CASE("queue: executions ahead of us do not fill us", "[sim][queue]") {
  QueuePosition q;
  q.join(500, kJoin);
  REQUIRE(q.on_execution(kAfter, 200, 100) == 0);
  REQUIRE(q.ahead() == 300);
  REQUIRE(q.on_execution(kAfter, 250, 100) == 0);
  REQUIRE(q.ahead() == 50);
}

TEST_CASE("queue: we fill only once volume ahead is exhausted", "[sim][queue]") {
  QueuePosition q;
  q.join(100, kJoin);
  // 150 trades: 100 clears the queue, the remaining 50 reaches us.
  REQUIRE(q.on_execution(kAfter, 150, 100) == 50);
  REQUIRE(q.ahead() == 0);
  // Now at the front: the next execution fills us up to our remaining size.
  REQUIRE(q.on_execution(kAfter, 30, 50) == 30);
}

TEST_CASE("queue: a fill is capped at our own remaining size", "[sim][queue]") {
  QueuePosition q;
  q.join(0, kJoin);
  REQUIRE(q.on_execution(kAfter, 10'000, 100) == 100);   // not 10000
}

// The behaviour that separates this from a naive model: the queue shortens
// when orders ahead are CANCELLED, with no trade at all. On this data adds and
// deletes are 85% of messages and executions only 1.7%, so most of the queue
// ahead of you disappears this way.
TEST_CASE("queue: cancels ahead of us advance our position", "[sim][queue]") {
  QueuePosition q;
  q.join(500, kJoin);
  q.on_cancel(/*seq=*/1, 150);
  REQUIRE(q.ahead() == 350);
  q.on_delete(/*seq=*/2, 300);
  REQUIRE(q.ahead() == 50);
  q.on_delete(/*seq=*/1, 50);
  REQUIRE(q.ahead() == 0);
}

TEST_CASE("queue: cancels BEHIND us do not advance our position", "[sim][queue]") {
  QueuePosition q;
  q.join(300, kJoin);
  // seq 100 > kJoin: this order arrived after we did, so it is behind us and
  // its disappearance does nothing for our position.
  q.on_cancel(100, 250);
  REQUIRE(q.ahead() == 300);
  q.on_delete(100, 250);
  REQUIRE(q.ahead() == 300);
}

TEST_CASE("queue: the ahead/behind boundary is exact", "[sim][queue]") {
  QueuePosition q;
  q.join(100, kJoin);
  REQUIRE(q.is_ahead(kJoin - 1));     // resting when we arrived
  REQUIRE_FALSE(q.is_ahead(kJoin));   // the stamp OUR order would have taken
  REQUIRE_FALSE(q.is_ahead(kJoin + 1));
}

TEST_CASE("queue: over-cancel cannot underflow the position", "[sim][queue]") {
  QueuePosition q;
  q.join(100, kJoin);
  q.on_cancel(1, 5000);            // more than was there
  REQUIRE(q.ahead() == 0);
}

TEST_CASE("queue: re-joining resets us to the back", "[sim][queue]") {
  QueuePosition q;
  q.join(100, kJoin);
  // Consumes exactly the volume ahead of us, so nothing trades through to
  // our order: the queue empties but we are not filled.
  REQUIRE(q.on_execution(kAfter, 100, 100) == 0);
  REQUIRE(q.ahead() == 0);

  q.join(800, 200);                // touch moved; we re-quoted
  REQUIRE(q.ahead() == 800);
  REQUIRE(q.on_execution(kAfter, 700, 100) == 0);
  // An order that was ahead of our OLD join is behind our new one.
  REQUIRE(q.is_ahead(150));
  REQUIRE_FALSE(q.is_ahead(250));
}

// The queue advances on volume consumed at the level, NOT on whether the
// specific order executed was ahead of us. Requiring "ahead" here looks
// principled and is provably wrong: we join at the back, so once everything
// ahead is gone the only orders left arrived after us, and gating on the stamp
// drops the fill count to exactly zero on every symbol.
TEST_CASE("queue: an order that arrived after us can still fill us", "[sim][queue]") {
  QueuePosition q;
  q.join(100, kJoin);
  REQUIRE(q.on_execution(kAfter, 100, 100) == 0);   // clears the queue ahead
  REQUIRE(q.ahead() == 0);
  REQUIRE(q.on_execution(kAfter, 40, 100) == 40);   // now it reaches us
}

TEST_CASE("queue: out-of-FIFO executions are counted, not silently absorbed",
          "[sim][queue]") {
  QueuePosition q;
  q.join(500, kJoin);
  REQUIRE(q.behind_while_queued() == 0);
  // An order that arrived after us trading while 500 shares still rest ahead
  // violates strict price-time priority. Real ITCH does this on 1.3-4.5% of
  // executions, so it is recorded rather than assumed away.
  (void)q.on_execution(kAfter, 50, 100);
  REQUIRE(q.behind_while_queued() == 1);
  // An order that was genuinely ahead of us is not suspicious.
  (void)q.on_execution(kJoin - 1, 50, 100);
  REQUIRE(q.behind_while_queued() == 1);
}
