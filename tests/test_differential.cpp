// Randomized differential test: the optimized engine and the naive reference
// engine receive the identical operation stream and must produce identical
// event sequences and identical book state throughout. This is the strongest
// correctness evidence in the suite — any divergence in matching, priority,
// STP, modify, or cleanup logic shows up within a few thousand operations.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "nanolob/engine.hpp"
#include "recording_handler.hpp"
#include "reference_engine.hpp"

using namespace nanolob;
using test::Event;
using test::RecordingHandler;
using test::ReferenceEngine;

namespace {

constexpr int kOpsPerRun = 20000;

void run_differential(std::uint64_t seed, StpPolicy stp) {
  std::mt19937_64 rng(seed);

  RecordingHandler engine_events;
  RecordingHandler ref_events;
  MatchingEngine<RecordingHandler> engine(engine_events, stp);
  ReferenceEngine<RecordingHandler> ref(ref_events, stp);

  Price mid = 10000;
  OrderId next_id = 1;
  std::size_t event_cursor = 0;

  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<Price> px_offset(-40, 40);
  std::uniform_int_distribution<Qty> qty_dist(1, 100);
  std::uniform_int_distribution<ParticipantId> part_dist(0, 3);  // few => STP hits often

  for (int i = 0; i < kOpsPerRun; ++i) {
    const int op = op_dist(rng);
    std::string op_desc;

    if (op < 45) {  // add limit
      const OrderId id = next_id++;
      const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
      const Price px = mid + px_offset(rng);
      const Qty qty = qty_dist(rng);
      const ParticipantId part = part_dist(rng);
      const TimeInForce tif = (op < 5) ? TimeInForce::IOC : TimeInForce::GTC;
      op_desc = "add_limit id=" + std::to_string(id);
      engine.add_limit(id, part, side, px, qty, tif);
      ref.add_limit(id, part, side, px, qty, tif);
    } else if (op < 55) {  // add market
      const OrderId id = next_id++;
      const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
      const Qty qty = qty_dist(rng);
      const ParticipantId part = part_dist(rng);
      op_desc = "add_market id=" + std::to_string(id);
      engine.add_market(id, part, side, qty);
      ref.add_market(id, part, side, qty);
    } else if (op < 85) {  // cancel (mostly live ids, sometimes junk)
      OrderId id = next_id + 1000;  // default: unknown id
      const std::vector<OrderId> live = ref.live_ids();
      if (!live.empty() && op < 82) id = live[rng() % live.size()];
      op_desc = "cancel id=" + std::to_string(id);
      engine.cancel(id);
      ref.cancel(id);
    } else {  // modify (reduce, increase, or reprice)
      OrderId id = next_id + 1000;
      const std::vector<OrderId> live = ref.live_ids();
      if (!live.empty()) id = live[rng() % live.size()];
      const Price px = mid + px_offset(rng);
      const Qty qty = qty_dist(rng) - 1;  // occasionally 0 => cancel path
      op_desc = "modify id=" + std::to_string(id);
      engine.modify(id, px, qty);
      ref.modify(id, px, qty);
    }

    if (i % 500 == 0) mid += px_offset(rng);  // drift so levels churn

    // Compare the newly appended event suffix.
    INFO("seed=" << seed << " stp=" << static_cast<int>(stp) << " op#" << i << " " << op_desc);
    REQUIRE(engine_events.events.size() == ref_events.events.size());
    const bool suffix_equal = std::equal(engine_events.events.begin() + event_cursor,
                                         engine_events.events.end(),
                                         ref_events.events.begin() + event_cursor);
    if (!suffix_equal) {
      std::ostringstream oss;
      for (std::size_t j = event_cursor; j < engine_events.events.size(); ++j) {
        oss << "\n  engine: " << engine_events.events[j] << "\n  ref:    " << ref_events.events[j];
      }
      INFO("diverging events:" << oss.str());
      REQUIRE(suffix_equal);
    }
    event_cursor = engine_events.events.size();

    // Periodic full state cross-check.
    if (i % 64 == 0) {
      REQUIRE(engine.open_orders() == ref.open_orders());
      const auto check_side = [&](Side s) {
        const auto ref_best = ref.best(s);
        const PriceLevel* eng_best =
            s == Side::Bid ? engine.best_bid() : engine.best_ask();
        if (ref_best.has_value()) {
          REQUIRE(eng_best != nullptr);
          REQUIRE(eng_best->price == ref_best->first);
          REQUIRE(eng_best->total_qty == ref_best->second);
        } else {
          REQUIRE(eng_best == nullptr);
        }
        for (Price px = mid - 45; px <= mid + 45; px += 9) {
          REQUIRE(engine.book().depth_at(s, px) == ref.depth_at(s, px));
        }
      };
      check_side(Side::Bid);
      check_side(Side::Ask);
      const std::vector<OrderId> live = ref.live_ids();
      for (std::size_t j = 0; j < std::min<std::size_t>(live.size(), 8); ++j) {
        const OrderId id = live[rng() % live.size()];
        REQUIRE(engine.open_qty(id) == ref.open_qty(id));
      }
    }
  }
}

}  // namespace

TEST_CASE("differential: engine matches reference under random flow (STP off)") {
  run_differential(1, StpPolicy::None);
  run_differential(2, StpPolicy::None);
  run_differential(3, StpPolicy::None);
}

TEST_CASE("differential: engine matches reference with STP CancelResting") {
  run_differential(11, StpPolicy::CancelResting);
  run_differential(12, StpPolicy::CancelResting);
}

TEST_CASE("differential: engine matches reference with STP CancelIncoming") {
  run_differential(21, StpPolicy::CancelIncoming);
  run_differential(22, StpPolicy::CancelIncoming);
}
