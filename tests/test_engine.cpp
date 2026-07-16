#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "nanolob/engine.hpp"
#include "recording_handler.hpp"

using namespace nanolob;
using test::Event;
using test::RecordingHandler;

namespace {

struct Fixture {
  RecordingHandler handler;
  MatchingEngine<RecordingHandler> engine;

  explicit Fixture(StpPolicy stp = StpPolicy::None) : engine(handler, stp) {}

  std::vector<Event>& events() { return handler.events; }
  void clear_events() { handler.events.clear(); }
};

}  // namespace

TEST_CASE("empty book has no BBO and rejects nothing valid") {
  Fixture f;
  CHECK(f.engine.best_bid() == nullptr);
  CHECK(f.engine.best_ask() == nullptr);
  CHECK(f.engine.open_orders() == 0);
}

TEST_CASE("resting limit orders set the BBO") {
  Fixture f;
  REQUIRE(f.engine.add_limit(1, 0, Side::Bid, 100, 10));
  REQUIRE(f.engine.add_limit(2, 0, Side::Ask, 105, 5));
  REQUIRE(f.engine.add_limit(3, 0, Side::Bid, 99, 7));

  REQUIRE(f.engine.best_bid() != nullptr);
  CHECK(f.engine.best_bid()->price == 100);
  CHECK(f.engine.best_bid()->total_qty == 10);
  REQUIRE(f.engine.best_ask() != nullptr);
  CHECK(f.engine.best_ask()->price == 105);
  CHECK(f.engine.best_ask()->total_qty == 5);
  CHECK(f.engine.open_orders() == 3);
  CHECK(f.engine.open_qty(1) == 10);
  CHECK(f.engine.book().depth_at(Side::Bid, 99) == 7);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(1, Side::Bid, 100, 10),
            Event::accept(2, Side::Ask, 105, 5),
            Event::accept(3, Side::Bid, 99, 7),
        });
}

TEST_CASE("aggressive limit fills better prices first") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 102, 5);
  f.engine.add_limit(2, 0, Side::Ask, 101, 5);
  f.clear_events();

  // Buy 8 @ 102: must lift 101 fully, then 3 lots of 102.
  REQUIRE(f.engine.add_limit(10, 1, Side::Bid, 102, 8));

  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 102, 8),
            Event::trade(10, 2, 101, 5, Side::Bid),
            Event::trade(10, 1, 102, 3, Side::Bid),
        });
  CHECK(f.engine.open_qty(1) == 2);
  CHECK(f.engine.open_qty(10) == 0);  // fully filled, nothing rests
  CHECK(f.engine.best_ask()->price == 102);
}

TEST_CASE("time priority within a level is FIFO") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 5);
  f.engine.add_limit(2, 0, Side::Ask, 101, 5);
  f.clear_events();

  f.engine.add_market(10, 1, Side::Bid, 7);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 0, 7),
            Event::trade(10, 1, 101, 5, Side::Bid),
            Event::trade(10, 2, 101, 2, Side::Bid),
        });
  CHECK(f.engine.open_qty(1) == 0);
  CHECK(f.engine.open_qty(2) == 3);
}

TEST_CASE("partial fill leaves the maker resting with reduced quantity") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Bid, 100, 10);
  f.clear_events();

  f.engine.add_limit(2, 1, Side::Ask, 100, 4);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(2, Side::Ask, 100, 4),
            Event::trade(2, 1, 100, 4, Side::Ask),
        });
  CHECK(f.engine.open_qty(1) == 6);
  CHECK(f.engine.best_bid()->total_qty == 6);
  CHECK(f.engine.open_orders() == 1);
}

TEST_CASE("unmatched GTC remainder rests at its limit price") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 5);
  f.clear_events();

  f.engine.add_limit(2, 1, Side::Bid, 103, 8);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(2, Side::Bid, 103, 8),
            Event::trade(2, 1, 101, 5, Side::Bid),
        });
  // Remainder rests at the limit price 103, not at the last trade price.
  REQUIRE(f.engine.best_bid() != nullptr);
  CHECK(f.engine.best_bid()->price == 103);
  CHECK(f.engine.best_bid()->total_qty == 3);
  CHECK(f.engine.best_ask() == nullptr);
}

TEST_CASE("IOC cancels its remainder instead of resting") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 5);
  f.clear_events();

  f.engine.add_limit(2, 1, Side::Bid, 101, 8, TimeInForce::IOC);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(2, Side::Bid, 101, 8),
            Event::trade(2, 1, 101, 5, Side::Bid),
            Event::cancel(2, CancelReason::IocRemainder),
        });
  CHECK(f.engine.best_bid() == nullptr);
  CHECK(f.engine.open_orders() == 0);
}

TEST_CASE("fully filled IOC emits no cancel") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 10);
  f.clear_events();

  f.engine.add_limit(2, 1, Side::Bid, 101, 10, TimeInForce::IOC);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(2, Side::Bid, 101, 10),
            Event::trade(2, 1, 101, 10, Side::Bid),
        });
}

TEST_CASE("non-crossing IOC cancels in full") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 105, 5);
  f.clear_events();

  f.engine.add_limit(2, 1, Side::Bid, 100, 8, TimeInForce::IOC);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(2, Side::Bid, 100, 8),
            Event::cancel(2, CancelReason::IocRemainder),
        });
}

TEST_CASE("market order sweeps multiple levels and cancels the remainder") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Bid, 100, 3);
  f.engine.add_limit(2, 0, Side::Bid, 99, 3);
  f.engine.add_limit(3, 0, Side::Bid, 98, 3);
  f.clear_events();

  f.engine.add_market(10, 1, Side::Ask, 12);

  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Ask, 0, 12),
            Event::trade(10, 1, 100, 3, Side::Ask),
            Event::trade(10, 2, 99, 3, Side::Ask),
            Event::trade(10, 3, 98, 3, Side::Ask),
            Event::cancel(10, CancelReason::MarketRemainder),
        });
  CHECK(f.engine.best_bid() == nullptr);
  CHECK(f.engine.open_orders() == 0);
}

TEST_CASE("market order on an empty book cancels in full") {
  Fixture f;
  f.engine.add_market(1, 0, Side::Bid, 5);
  CHECK(f.events() == std::vector<Event>{
            Event::accept(1, Side::Bid, 0, 5),
            Event::cancel(1, CancelReason::MarketRemainder),
        });
}

TEST_CASE("cancel removes the order and cleans up empty levels") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Bid, 100, 10);
  f.engine.add_limit(2, 0, Side::Bid, 99, 5);
  f.clear_events();

  REQUIRE(f.engine.cancel(1));
  CHECK(f.events() == std::vector<Event>{Event::cancel(1, CancelReason::User)});
  REQUIRE(f.engine.best_bid() != nullptr);
  CHECK(f.engine.best_bid()->price == 99);  // level 100 fully removed
  CHECK(f.engine.book().depth_at(Side::Bid, 100) == 0);
  CHECK(f.engine.open_orders() == 1);

  // Cancelling again (or an unknown id) rejects.
  f.clear_events();
  CHECK_FALSE(f.engine.cancel(1));
  CHECK_FALSE(f.engine.cancel(777));
  CHECK(f.events() == std::vector<Event>{
            Event::reject(1, RejectReason::UnknownOrderId),
            Event::reject(777, RejectReason::UnknownOrderId),
        });
}

TEST_CASE("cancel of a middle order keeps FIFO order intact") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 1);
  f.engine.add_limit(2, 0, Side::Ask, 101, 2);
  f.engine.add_limit(3, 0, Side::Ask, 101, 4);
  f.engine.cancel(2);
  f.clear_events();

  f.engine.add_market(10, 1, Side::Bid, 5);
  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 0, 5),
            Event::trade(10, 1, 101, 1, Side::Bid),
            Event::trade(10, 3, 101, 4, Side::Bid),
        });
}

TEST_CASE("modify reducing quantity keeps queue priority") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 10);
  f.engine.add_limit(2, 0, Side::Ask, 101, 10);
  REQUIRE(f.engine.modify(1, 101, 4));  // reduce in place
  CHECK(f.engine.open_qty(1) == 4);
  CHECK(f.engine.best_ask()->total_qty == 14);
  f.clear_events();

  f.engine.add_market(10, 1, Side::Bid, 4);
  // Order 1 still fills first: it kept its queue position.
  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 0, 4),
            Event::trade(10, 1, 101, 4, Side::Bid),
        });
}

TEST_CASE("modify increasing quantity loses queue priority") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Ask, 101, 5);
  f.engine.add_limit(2, 0, Side::Ask, 101, 5);
  REQUIRE(f.engine.modify(1, 101, 8));  // qty up => cancel/replace
  f.clear_events();

  f.engine.add_market(10, 1, Side::Bid, 5);
  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 0, 5),
            Event::trade(10, 2, 101, 5, Side::Bid),  // order 2 is now ahead
        });
  CHECK(f.engine.open_qty(1) == 8);
}

TEST_CASE("modify changing price can match immediately") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Bid, 99, 5);
  f.engine.add_limit(2, 1, Side::Ask, 101, 5);
  f.clear_events();

  // Reprice the bid through the ask: it must trade like a fresh order.
  REQUIRE(f.engine.modify(1, 101, 5));
  CHECK(f.events() == std::vector<Event>{Event::trade(1, 2, 101, 5, Side::Bid)});
  CHECK(f.engine.open_orders() == 0);
}

TEST_CASE("modify to zero quantity cancels") {
  Fixture f;
  f.engine.add_limit(1, 0, Side::Bid, 100, 10);
  f.clear_events();
  REQUIRE(f.engine.modify(1, 100, 0));
  CHECK(f.events() == std::vector<Event>{Event::cancel(1, CancelReason::User)});
  CHECK(f.engine.open_orders() == 0);
}

TEST_CASE("modify of unknown order rejects") {
  Fixture f;
  CHECK_FALSE(f.engine.modify(42, 100, 5));
  CHECK(f.events() == std::vector<Event>{Event::reject(42, RejectReason::UnknownOrderId)});
}

TEST_CASE("duplicate live order ids reject; ids of dead orders may be reused") {
  Fixture f;
  REQUIRE(f.engine.add_limit(1, 0, Side::Bid, 100, 10));
  CHECK_FALSE(f.engine.add_limit(1, 0, Side::Bid, 101, 5));
  CHECK(f.events().back() == Event::reject(1, RejectReason::DuplicateOrderId));

  REQUIRE(f.engine.cancel(1));
  REQUIRE(f.engine.add_limit(1, 0, Side::Bid, 101, 5));  // reuse after death is fine
  CHECK(f.engine.open_qty(1) == 5);
}

TEST_CASE("zero-quantity orders reject") {
  Fixture f;
  CHECK_FALSE(f.engine.add_limit(1, 0, Side::Bid, 100, 0));
  CHECK_FALSE(f.engine.add_market(2, 0, Side::Ask, 0));
  CHECK(f.events() == std::vector<Event>{
            Event::reject(1, RejectReason::ZeroQty),
            Event::reject(2, RejectReason::ZeroQty),
        });
}

TEST_CASE("STP CancelResting cancels own resting order and keeps matching") {
  Fixture f(StpPolicy::CancelResting);
  f.engine.add_limit(1, /*participant=*/7, Side::Ask, 101, 5);  // own
  f.engine.add_limit(2, /*participant=*/8, Side::Ask, 101, 5);  // other
  f.clear_events();

  // Participant 7 crosses its own quote: the resting order is pulled, the
  // incoming order then trades with participant 8 behind it.
  f.engine.add_limit(10, 7, Side::Bid, 101, 5);
  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 101, 5),
            Event::cancel(1, CancelReason::SelfTradePrevention),
            Event::trade(10, 2, 101, 5, Side::Bid),
        });
  CHECK(f.engine.open_orders() == 0);
}

TEST_CASE("STP CancelIncoming kills the incoming remainder, resting order stays") {
  Fixture f(StpPolicy::CancelIncoming);
  f.engine.add_limit(1, /*participant=*/8, Side::Ask, 100, 5);  // other, better px
  f.engine.add_limit(2, /*participant=*/7, Side::Ask, 101, 5);  // own, worse px
  f.clear_events();

  // Fills the better-priced stranger first, then hits its own order and dies.
  f.engine.add_limit(10, 7, Side::Bid, 101, 8);
  CHECK(f.events() == std::vector<Event>{
            Event::accept(10, Side::Bid, 101, 8),
            Event::trade(10, 1, 100, 5, Side::Bid),
            Event::cancel(10, CancelReason::SelfTradePrevention),
        });
  CHECK(f.engine.open_qty(2) == 5);  // own resting order untouched
  CHECK(f.engine.open_orders() == 1);
  CHECK(f.engine.best_bid() == nullptr);  // incoming remainder did not rest
}

TEST_CASE("STP None allows self-matching (replay mode)") {
  Fixture f(StpPolicy::None);
  f.engine.add_limit(1, 7, Side::Ask, 101, 5);
  f.clear_events();
  f.engine.add_limit(2, 7, Side::Bid, 101, 5);
  CHECK(f.events() == std::vector<Event>{
            Event::accept(2, Side::Bid, 101, 5),
            Event::trade(2, 1, 101, 5, Side::Bid),
        });
}

TEST_CASE("book rebuilds correctly across heavy add/cancel churn") {
  Fixture f;
  // Build a 10-level ladder each side, cancel every other order, verify depth.
  OrderId id = 1;
  for (int i = 0; i < 10; ++i) {
    f.engine.add_limit(id++, 0, Side::Bid, 100 - i, 10 + i);
    f.engine.add_limit(id++, 0, Side::Ask, 110 + i, 10 + i);
  }
  for (OrderId cancel_id = 1; cancel_id < id; cancel_id += 2) f.engine.cancel(cancel_id);

  // Odd ids were all bids: the whole bid side is gone, asks are intact.
  CHECK(f.engine.best_bid() == nullptr);
  REQUIRE(f.engine.best_ask() != nullptr);
  CHECK(f.engine.best_ask()->price == 110);
  for (int i = 0; i < 10; ++i) {
    CHECK(f.engine.book().depth_at(Side::Ask, 110 + i) == static_cast<Qty>(10 + i));
  }
  CHECK(f.engine.open_orders() == 10);
}
