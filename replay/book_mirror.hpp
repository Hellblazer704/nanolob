#pragma once

// Translates Binance L2 *level* updates into *order* operations against the
// matching engine: each live price level is represented by one synthetic
// resting order whose quantity equals the level's aggregate quantity.
//
//  - quantity decrease  -> modify (in-place reduction; keeps priority)
//  - quantity increase  -> modify (engine treats as cancel/replace)
//  - quantity to zero   -> cancel
//  - new price level    -> add_limit
//
// Within one diff, removals and reductions are applied across BOTH sides
// before any additions/increases. Diff batches are 100ms windows; applying
// raw order (bids then asks) can transiently cross the book mid-batch and
// trigger phantom matches. Removals-first eliminates that for any diff whose
// end state is uncrossed (i.e., every real exchange diff). If the engine
// still reports a trade during mirroring, the input itself was crossed and
// the event is counted as an anomaly.

#include <cstdint>
#include <vector>

#include "nanolob/flat_hash_map.hpp"
#include "nanolob/types.hpp"
#include "replay/binance_parser.hpp"

namespace nanolob::replay {

template <typename Engine>
class BookMirror {
 public:
  explicit BookMirror(Engine& engine, OrderId id_base = 1) : engine_(engine), next_id_(id_base) {}

  void apply_snapshot(const SnapshotRec& snap) {
    for (const LevelUpdate& lu : snap.bids) apply_level(Side::Bid, lu);
    for (const LevelUpdate& lu : snap.asks) apply_level(Side::Ask, lu);
  }

  void apply_diff(const DiffRec& diff) {
    // Pass 1: removals and reductions on both sides.
    for (const LevelUpdate& lu : diff.bids) {
      if (is_reduction(Side::Bid, lu)) apply_level(Side::Bid, lu);
    }
    for (const LevelUpdate& lu : diff.asks) {
      if (is_reduction(Side::Ask, lu)) apply_level(Side::Ask, lu);
    }
    // Pass 2: additions and increases.
    for (const LevelUpdate& lu : diff.bids) {
      if (!is_reduction(Side::Bid, lu)) apply_level(Side::Bid, lu);
    }
    for (const LevelUpdate& lu : diff.asks) {
      if (!is_reduction(Side::Ask, lu)) apply_level(Side::Ask, lu);
    }
  }

  [[nodiscard]] std::uint64_t levels_applied() const noexcept { return levels_applied_; }

 private:
  [[nodiscard]] FlatHashMap<Price, OrderId>& ids(Side side) noexcept {
    return side == Side::Bid ? bid_ids_ : ask_ids_;
  }

  [[nodiscard]] bool is_reduction(Side side, const LevelUpdate& lu) noexcept {
    OrderId* id = ids(side).find(lu.price);
    if (id == nullptr) return false;                // brand-new level: an addition
    return lu.qty < engine_.open_qty(*id);          // includes qty == 0 removals
  }

  void apply_level(Side side, const LevelUpdate& lu) {
    ++levels_applied_;
    OrderId* existing = ids(side).find(lu.price);
    if (existing == nullptr) {
      if (lu.qty == 0) return;  // removal of a level we never tracked
      const OrderId id = next_id_++;
      engine_.add_limit(id, kMirrorParticipant, side, lu.price, lu.qty);
      ids(side).insert(lu.price, id);
      return;
    }
    if (lu.qty == 0) {
      engine_.cancel(*existing);
      ids(side).erase(lu.price);
      return;
    }
    if (lu.qty == engine_.open_qty(*existing)) return;  // no-op update
    engine_.modify(*existing, lu.price, lu.qty);
  }

  static constexpr ParticipantId kMirrorParticipant = 0xFEED;

  Engine& engine_;
  FlatHashMap<Price, OrderId> bid_ids_;
  FlatHashMap<Price, OrderId> ask_ids_;
  OrderId next_id_;
  std::uint64_t levels_applied_ = 0;
};

}  // namespace nanolob::replay
