#include <catch2/catch_test_macros.hpp>
#include <string>

#include "nanolob/engine.hpp"
#include "recording_handler.hpp"
#include "replay/binance_parser.hpp"
#include "replay/book_mirror.hpp"

using namespace nanolob;
using namespace nanolob::replay;

TEST_CASE("parse_fixed scales decimal strings to integer ticks") {
  CHECK(parse_fixed("117234.56000000", 2) == 11723456);
  CHECK(parse_fixed("117234.5", 2) == 11723450);   // short fraction padded
  CHECK(parse_fixed("117234", 2) == 11723400);     // no fraction
  CHECK(parse_fixed("0.00812000", 5) == 812);
  CHECK(parse_fixed("0.00000000", 5) == 0);
  CHECK(parse_fixed("2.71828999", 5) == 271828);   // extra digits truncated
  CHECK_FALSE(parse_fixed("abc", 2).has_value());
  CHECK_FALSE(parse_fixed("-5.00", 2).has_value());
}

TEST_CASE("parser handles the three capture record types") {
  const std::string snap_line =
      R"({"type":"snapshot","ts":1752600000000,"data":{"lastUpdateId":777,)"
      R"("bids":[["100.50","1.00000000"],["100.49","0.50000000"]],)"
      R"("asks":[["100.52","2.00000000"]]}})";
  auto snap = parse_line(snap_line);
  REQUIRE(snap.has_value());
  REQUIRE(snap->kind == Record::Kind::Snapshot);
  CHECK(snap->snapshot.last_update_id == 777);
  REQUIRE(snap->snapshot.bids.size() == 2);
  CHECK(snap->snapshot.bids[0].price == 10050);
  CHECK(snap->snapshot.bids[0].qty == 100000);
  CHECK(snap->snapshot.bids[1].price == 10049);
  CHECK(snap->snapshot.bids[1].qty == 50000);
  REQUIRE(snap->snapshot.asks.size() == 1);
  CHECK(snap->snapshot.asks[0].price == 10052);

  // Local capture ts deliberately differs from the exchange event time E by
  // 1400ms (the dev box's real clock skew) so that reading the wrong field is
  // a test failure rather than a coincidence.
  const std::string diff_line =
      R"({"type":"diff","ts":1752599998700,"data":{"e":"depthUpdate","E":1752600000100,)"
      R"("s":"BTCUSDT","U":778,"u":780,"b":[["100.50","0.00000000"]],)"
      R"("a":[["100.53","0.75000000"],["100.52","0.00000000"]]}})";
  auto diff = parse_line(diff_line);
  REQUIRE(diff.has_value());
  REQUIRE(diff->kind == Record::Kind::Diff);
  CHECK(diff->diff.first_id == 778);
  CHECK(diff->diff.last_id == 780);
  // Diffs must carry the exchange event time "E", not the wrapper's local
  // capture "ts" — trades use exchange time, and the capture host's clock is
  // not the exchange's. Here they differ so the wrong one is detectable.
  CHECK(diff->diff.ts == 1752600000100);
  REQUIRE(diff->diff.bids.size() == 1);
  CHECK(diff->diff.bids[0].qty == 0);
  REQUIRE(diff->diff.asks.size() == 2);

  const std::string trade_line =
      R"({"type":"trade","ts":1752600000200,"data":{"e":"trade","E":1752600000200,)"
      R"("s":"BTCUSDT","t":42,"p":"100.51","q":"0.10000000","T":1752600000199,"m":true}})";
  auto trade = parse_line(trade_line);
  REQUIRE(trade.has_value());
  REQUIRE(trade->kind == Record::Kind::Trade);
  CHECK(trade->trade.price == 10051);
  CHECK(trade->trade.qty == 10000);
  CHECK(trade->trade.ts == 1752600000199);
  CHECK(trade->trade.buyer_is_maker);

  CHECK_FALSE(parse_line("garbage").has_value());
  CHECK_FALSE(parse_line(R"({"type":"unknown","ts":1,"data":{}})").has_value());
}

TEST_CASE("book mirror reconstructs levels through snapshot and diffs") {
  test::RecordingHandler handler;
  MatchingEngine<test::RecordingHandler> engine(handler);
  BookMirror<MatchingEngine<test::RecordingHandler>> mirror(engine);

  SnapshotRec snap;
  snap.last_update_id = 100;
  snap.bids = {{10050, 100}, {10049, 200}};
  snap.asks = {{10052, 300}, {10053, 400}};
  mirror.apply_snapshot(snap);

  REQUIRE(engine.best_bid() != nullptr);
  CHECK(engine.best_bid()->price == 10050);
  CHECK(engine.best_bid()->total_qty == 100);
  CHECK(engine.best_ask()->price == 10052);
  CHECK(engine.book().depth_at(Side::Ask, 10053) == 400);

  // Diff: bid level reduced, best ask removed, new ask added.
  DiffRec diff;
  diff.first_id = 101;
  diff.last_id = 105;
  diff.bids = {{10050, 60}};
  diff.asks = {{10052, 0}, {10054, 500}};
  mirror.apply_diff(diff);

  CHECK(engine.best_bid()->total_qty == 60);
  CHECK(engine.best_ask()->price == 10053);
  CHECK(engine.book().depth_at(Side::Ask, 10054) == 500);
  CHECK(engine.book().depth_at(Side::Ask, 10052) == 0);

  // Idempotency: absolute quantities make re-applying the same diff a no-op.
  mirror.apply_diff(diff);
  CHECK(engine.best_bid()->total_qty == 60);
  CHECK(engine.best_ask()->price == 10053);
  CHECK(engine.book().depth_at(Side::Ask, 10054) == 500);

  // No phantom matches occurred at any point.
  for (const auto& e : handler.events) {
    CHECK(e.type != test::Event::Type::Trade);
  }
}

TEST_CASE("book mirror applies removals before additions to avoid crossing") {
  test::RecordingHandler handler;
  MatchingEngine<test::RecordingHandler> engine(handler);
  BookMirror<MatchingEngine<test::RecordingHandler>> mirror(engine);

  SnapshotRec snap;
  snap.last_update_id = 1;
  snap.bids = {{10050, 100}};
  snap.asks = {{10052, 100}};
  mirror.apply_snapshot(snap);

  // The mid moved up in one 100ms window: ask 10052 pulled, bid appears at
  // 10052, new ask at 10054. In raw order (bids first) the new bid would
  // cross the stale ask and phantom-match. Removals-first prevents that.
  DiffRec diff;
  diff.first_id = 2;
  diff.last_id = 9;
  diff.bids = {{10052, 50}};
  diff.asks = {{10052, 0}, {10054, 80}};
  mirror.apply_diff(diff);

  CHECK(engine.best_bid()->price == 10052);
  CHECK(engine.best_ask()->price == 10054);
  for (const auto& e : handler.events) {
    CHECK(e.type != test::Event::Type::Trade);
  }
}
