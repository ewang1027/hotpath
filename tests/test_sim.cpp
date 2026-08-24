#include "hotpath/sim/queue_model.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace hotpath;
using namespace hotpath::sim;

TEST_CASE("queue: executions ahead of us do not fill us", "[sim][queue]") {
  QueuePosition q;
  q.join(500);
  q.note_ahead(1, 200);
  q.note_ahead(2, 300);

  REQUIRE(q.on_execution(1, 200, 100) == 0);   // consumed order 1
  REQUIRE(q.ahead() == 300);
  REQUIRE(q.on_execution(2, 250, 100) == 0);   // partially consumed order 2
  REQUIRE(q.ahead() == 50);
}

TEST_CASE("queue: we fill only once volume ahead is exhausted", "[sim][queue]") {
  QueuePosition q;
  q.join(100);
  q.note_ahead(1, 100);

  // 150 trades: 100 clears the queue, the remaining 50 reaches us.
  REQUIRE(q.on_execution(1, 150, 100) == 50);
  REQUIRE(q.ahead() == 0);
  // Now we are at the front: the next execution fills us up to our remaining size.
  REQUIRE(q.on_execution(9, 30, 50) == 30);
}

TEST_CASE("queue: a fill is capped at our own remaining size", "[sim][queue]") {
  QueuePosition q;
  q.join(0);
  REQUIRE(q.on_execution(1, 10'000, 100) == 100);   // not 10000
}

// The behaviour that separates this from a naive model: the queue shortens
// when orders ahead are CANCELLED, with no trade at all. On this data adds and
// deletes are 85% of messages and executions only 1.7%, so most of the queue
// ahead of you disappears this way.
TEST_CASE("queue: cancels ahead of us advance our position", "[sim][queue]") {
  QueuePosition q;
  q.join(500);
  q.note_ahead(1, 200);
  q.note_ahead(2, 300);

  q.on_cancel(1, 150);
  REQUIRE(q.ahead() == 350);
  q.on_delete(2);
  REQUIRE(q.ahead() == 50);
  q.on_delete(1);                  // remaining 50 of order 1
  REQUIRE(q.ahead() == 0);
}

TEST_CASE("queue: cancels BEHIND us do not advance our position", "[sim][queue]") {
  QueuePosition q;
  q.join(300);
  q.note_ahead(1, 300);
  // Order 7 arrived after we joined, so it was never noted as ahead.
  q.on_cancel(7, 250);
  REQUIRE(q.ahead() == 300);
  q.on_delete(7);
  REQUIRE(q.ahead() == 300);
}

TEST_CASE("queue: over-cancel cannot underflow the position", "[sim][queue]") {
  QueuePosition q;
  q.join(100);
  q.note_ahead(1, 100);
  q.on_cancel(1, 5000);            // more than was there
  REQUIRE(q.ahead() == 0);
}

TEST_CASE("queue: re-joining resets us to the back", "[sim][queue]") {
  QueuePosition q;
  q.join(100);
  q.note_ahead(1, 100);
  q.on_execution(1, 100, 100);
  REQUIRE(q.ahead() == 0);

  q.join(800);                     // touch moved; we re-quoted
  REQUIRE(q.ahead() == 800);
  REQUIRE(q.on_execution(2, 700, 100) == 0);
}
