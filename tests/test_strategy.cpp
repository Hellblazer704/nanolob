#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "replay/binance_parser.hpp"
#include "strategy/quote_sim.hpp"
#include "strategy/strategies.hpp"

using namespace nanolob;
using namespace nanolob::mm;
using Catch::Approx;

namespace {
replay::TradeRec sell_aggressor(Price px, Qty qty, std::int64_t ts = 1000) {
  return replay::TradeRec{ts, px, qty, /*buyer_is_maker=*/true};
}
replay::TradeRec buy_aggressor(Price px, Qty qty, std::int64_t ts = 1000) {
  return replay::TradeRec{ts, px, qty, /*buyer_is_maker=*/false};
}
}  // namespace

TEST_CASE("queue sim: trades consume the queue ahead before filling us") {
  QuoteSimulator sim;
  sim.place(Side::Bid, 10000, /*size=*/100, /*level_qty=*/500, /*ts=*/0);

  // 300 trades at our price: all absorbed by the 500 ahead of us.
  sim.on_trade(sell_aggressor(10000, 300), 20001);
  CHECK(sim.fills().empty());
  CHECK(sim.get(Side::Bid).queue_ahead == Approx(200.0));

  // 250 more: 200 finish the queue, 50 fill us partially.
  sim.on_trade(sell_aggressor(10000, 250, 2000), 20001);
  REQUIRE(sim.fills().size() == 1);
  CHECK(sim.fills()[0].qty == 50);
  CHECK(sim.fills()[0].price == 10000);
  CHECK(sim.fills()[0].side == Side::Bid);
  CHECK(sim.fills()[0].queue_wait_ms == 2000);
  CHECK(sim.get(Side::Bid).remaining == 50);
  CHECK(sim.get(Side::Bid).active);
}

TEST_CASE("queue sim: trade through our price fills the full remainder") {
  QuoteSimulator sim;
  sim.place(Side::Bid, 10000, 100, 500, 0);
  // Sell aggressor printed BELOW our bid: the whole 10000 level was gone.
  sim.on_trade(sell_aggressor(9999, 10), 19995);
  REQUIRE(sim.fills().size() == 1);
  CHECK(sim.fills()[0].qty == 100);
  CHECK_FALSE(sim.get(Side::Bid).active);
}

TEST_CASE("queue sim: trades on the other side or price do not touch us") {
  QuoteSimulator sim;
  sim.place(Side::Bid, 10000, 100, 500, 0);
  sim.on_trade(buy_aggressor(10002, 1000), 20002);   // ask side
  sim.on_trade(sell_aggressor(10001, 1000), 20001);  // bid side, worse level than ours? no:
  // 10001 > our 10000 bid: a sell aggressor hitting a better bid above us —
  // consumes that level, not ours.
  CHECK(sim.fills().empty());
  CHECK(sim.get(Side::Bid).queue_ahead == Approx(500.0));
}

TEST_CASE("queue sim: cancellations shrink the queue ahead pro rata") {
  QuoteSimulator sim;
  sim.place(Side::Ask, 10010, 100, 400, 0);
  // Level drops 400 -> 100 with no trades in between: 300 cancelled, our
  // estimated share ahead shrinks proportionally (400 ahead * 100/400).
  sim.on_level_update(Side::Ask, 100);
  CHECK(sim.get(Side::Ask).queue_ahead == Approx(100.0));
  // Level grows: joins behind us, queue ahead unchanged.
  sim.on_level_update(Side::Ask, 600);
  CHECK(sim.get(Side::Ask).queue_ahead == Approx(100.0));
}

TEST_CASE("queue sim: trades and cancels interleave consistently") {
  QuoteSimulator sim;
  sim.place(Side::Ask, 10010, 100, 400, 0);
  // A buy aggressor takes 150 from the front.
  sim.on_trade(buy_aggressor(10010, 150), 20020);
  CHECK(sim.get(Side::Ask).queue_ahead == Approx(250.0));
  CHECK(sim.get(Side::Ask).level_qty_seen == 250);
  // Next diff shows the level at 200: 50 more disappeared = cancels,
  // prorated against the 250 ahead of us.
  sim.on_level_update(Side::Ask, 200);
  CHECK(sim.get(Side::Ask).queue_ahead == Approx(250.0 - 50.0 * (250.0 / 250.0)));
}

TEST_CASE("queue sim: book crossing our quote fills it") {
  QuoteSimulator sim;
  sim.place(Side::Ask, 10010, 100, 50, 0);
  sim.on_cross(Side::Ask, 5000, 20021);
  REQUIRE(sim.fills().size() == 1);
  CHECK(sim.fills()[0].qty == 100);
  CHECK(sim.fills()[0].side == Side::Ask);
  CHECK_FALSE(sim.get(Side::Ask).active);
}

TEST_CASE("variance estimator tracks the scale of mid moves") {
  VarianceEstimator vol(5.0);
  // 1-tick moves every 100ms => r^2/dt = 1/0.1 = 10 ticks^2/s.
  double mid = 10000.0;
  std::int64_t ts = 0;
  vol.update(mid, ts);
  for (int i = 0; i < 600; ++i) {
    ts += 100;
    mid += (i % 2 == 0) ? 1.0 : -1.0;
    vol.update(mid, ts);
  }
  CHECK(vol.ready());
  CHECK(vol.var_rate() == Approx(10.0).margin(1.0));
}

TEST_CASE("A-S reservation price skews against inventory") {
  AvellanedaStoikov as(/*gamma=*/1e-3, /*k=*/0.5, /*tau_s=*/10.0, /*q_max=*/10.0);
  MarketState flat{10000.0, 9995, 10005, /*var_rate=*/100.0, /*q_units=*/0.0};
  MarketState longish = flat;
  longish.q_units = 3.0;
  MarketState shortish = flat;
  shortish.q_units = -3.0;

  const QuotePair q_flat = as.quotes(flat);
  const QuotePair q_long = as.quotes(longish);
  const QuotePair q_short = as.quotes(shortish);

  REQUIRE(q_flat.bid.has_value());
  REQUIRE(q_flat.ask.has_value());
  // Long inventory: both quotes shift DOWN (eager to sell, reluctant to buy).
  CHECK(*q_long.bid < *q_flat.bid);
  CHECK(*q_long.ask < *q_flat.ask);
  // Short inventory: mirror image.
  CHECK(*q_short.bid > *q_flat.bid);
  CHECK(*q_short.ask > *q_flat.ask);
  CHECK(as.reservation_skew(longish) < 0);
  CHECK(as.reservation_skew(shortish) > 0);
}

TEST_CASE("A-S spread widens with volatility") {
  AvellanedaStoikov as(1e-3, 0.5, 10.0, 10.0);
  MarketState calm{10000.0, 9990, 10010, /*var_rate=*/10.0, 0.0};
  MarketState wild = calm;
  wild.var_rate = 1000.0;
  const QuotePair q_calm = as.quotes(calm);
  const QuotePair q_wild = as.quotes(wild);
  CHECK(*q_wild.ask - *q_wild.bid > *q_calm.ask - *q_calm.bid);
}

TEST_CASE("strategies never cross the market and respect the inventory cap") {
  NaiveFixedSpread naive(/*half_spread_ticks=*/1.0, /*q_max=*/2.0);
  // Wide raw quotes vs a tight market: clamped inside the touch.
  MarketState tight{10000.0, 9999, 10001, 10.0, 0.0};
  const QuotePair q = naive.quotes(tight);
  REQUIRE(q.bid.has_value());
  REQUIRE(q.ask.has_value());
  CHECK(*q.bid <= 10000);
  CHECK(*q.ask >= 10000);
  CHECK(*q.bid < *q.ask);

  MarketState too_long = tight;
  too_long.q_units = 2.0;
  CHECK_FALSE(naive.quotes(too_long).bid.has_value());  // stop buying
  CHECK(naive.quotes(too_long).ask.has_value());

  MarketState too_short = tight;
  too_short.q_units = -2.0;
  CHECK(naive.quotes(too_short).bid.has_value());
  CHECK_FALSE(naive.quotes(too_short).ask.has_value());  // stop selling
}
