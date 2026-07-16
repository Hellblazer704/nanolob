#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>
#include <vector>

#include "nanolob/feed.hpp"
#include "nanolob/spsc_ring.hpp"

using nanolob::FeedMsg;
using nanolob::SpscRing;

TEST_CASE("spsc ring single-threaded fill, drain, and wraparound") {
  SpscRing<std::uint64_t, 8> ring;
  std::uint64_t out = 0;

  CHECK_FALSE(ring.try_pop(out));  // empty

  for (std::uint64_t i = 0; i < 8; ++i) CHECK(ring.try_push(i));
  CHECK_FALSE(ring.try_push(99));  // full
  CHECK(ring.size_approx() == 8);

  for (std::uint64_t i = 0; i < 8; ++i) {
    REQUIRE(ring.try_pop(out));
    CHECK(out == i);  // FIFO
  }
  CHECK_FALSE(ring.try_pop(out));

  // Push/pop interleaved across many wraparounds of the 8-slot buffer.
  std::uint64_t next_push = 0;
  std::uint64_t next_pop = 0;
  for (int round = 0; round < 1000; ++round) {
    for (int k = 0; k < 5; ++k) REQUIRE(ring.try_push(next_push++));
    for (int k = 0; k < 5; ++k) {
      REQUIRE(ring.try_pop(out));
      REQUIRE(out == next_pop++);
    }
  }
}

TEST_CASE("spsc ring transfers every item exactly once across threads") {
  constexpr std::uint64_t kItems = 2'000'000;
  SpscRing<std::uint64_t, 1024> ring;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!ring.try_push(i)) {
        // spin: SPSC rings are used on dedicated threads
      }
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t popped = 0;
  bool ordered = true;
  while (popped < kItems) {
    std::uint64_t v = 0;
    if (ring.try_pop(v)) {
      ordered = ordered && (v == expected);
      ++expected;
      ++popped;
    }
  }
  producer.join();

  CHECK(ordered);          // strict FIFO, no loss, no duplication
  CHECK(popped == kItems);
  CHECK(ring.size_approx() == 0);
}

TEST_CASE("spsc ring carries FeedMsg payloads intact") {
  SpscRing<FeedMsg, 16> ring;
  REQUIRE(ring.try_push(FeedMsg::add_limit(42, 7, nanolob::Side::Ask, 10050, 3)));
  REQUIRE(ring.try_push(FeedMsg::cancel(43)));

  FeedMsg m{};
  REQUIRE(ring.try_pop(m));
  CHECK(m.kind == FeedMsg::Kind::AddLimit);
  CHECK(m.id == 42);
  CHECK(m.participant == 7);
  CHECK(m.side == nanolob::Side::Ask);
  CHECK(m.price == 10050);
  CHECK(m.qty == 3);
  REQUIRE(ring.try_pop(m));
  CHECK(m.kind == FeedMsg::Kind::Cancel);
  CHECK(m.id == 43);
}
