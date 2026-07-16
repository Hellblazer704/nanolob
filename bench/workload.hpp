#pragma once

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "nanolob/feed.hpp"
#include "nanolob/types.hpp"

namespace nanolob::bench {

// A pre-generated, deterministic operation stream. Generation cost stays out
// of the measured region, and every engine variant replays byte-identical
// input. `measure[i]` marks ops included in latency percentiles.
struct Workload {
  std::vector<FeedMsg> prefill;  // build the initial book (not measured)
  std::vector<FeedMsg> ops;      // replayed in the measured region
  std::vector<std::uint8_t> measure;
};

inline constexpr Price kMid = 1'000'000;
inline constexpr int kLevelsPerSide = 200;
// Prefill ids live in a high range so per-iteration id offsets on `ops`
// never collide with them.
inline constexpr OrderId kPrefillIdBase = 1ULL << 40;

enum class MeasureKind { Adds, Cancels, All };

// Steady-state book maintenance: every op pair adds one non-crossing order
// and cancels the oldest live one, so book depth stays flat no matter how
// many times the stream is replayed. This isolates the pure add/cancel hot
// path: no matching, no level-list walks past the touch.
inline Workload make_rest_heavy(std::size_t n_pairs, MeasureKind kind,
                                std::uint64_t seed = 0xBEEF) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<Price> off(1, kLevelsPerSide);
  std::uniform_int_distribution<Qty> qty(1, 100);

  Workload w;
  std::deque<OrderId> live;

  OrderId next_prefill = kPrefillIdBase;
  for (int i = 0; i < 50'000; ++i) {
    const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
    const Price px = side == Side::Bid ? kMid - off(rng) : kMid + off(rng);
    w.prefill.push_back(FeedMsg::add_limit(next_prefill++, 0, side, px, qty(rng)));
  }

  OrderId next_id = 1;
  for (std::size_t i = 0; i < n_pairs; ++i) {
    const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
    const Price px = side == Side::Bid ? kMid - off(rng) : kMid + off(rng);
    w.ops.push_back(FeedMsg::add_limit(next_id, 0, side, px, qty(rng)));
    w.measure.push_back(kind != MeasureKind::Cancels ? 1 : 0);
    live.push_back(next_id++);

    // Steady state: cancel the oldest once we are past a working set.
    if (live.size() > 10'000) {
      w.ops.push_back(FeedMsg::cancel(live.front()));
      live.pop_front();
      w.measure.push_back(kind != MeasureKind::Adds ? 1 : 0);
    }
  }
  return w;
}

// Realistic mixed flow: mostly passive adds and cancels, some aggressive
// limits and markets that cross and consume liquidity. The generator tracks
// its own view of live ids; ids consumed by matching that it later cancels
// are simply rejected by the engine (cheap, and realistic — venues reject
// cancels racing fills all day long).
inline Workload make_mixed(std::size_t n_ops, std::uint64_t seed = 0xFACE) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int> pct(0, 99);
  std::uniform_int_distribution<Price> off(1, kLevelsPerSide);
  std::uniform_int_distribution<Qty> qty(1, 100);

  Workload w;
  std::vector<OrderId> live;

  OrderId next_prefill = kPrefillIdBase;
  for (int i = 0; i < 50'000; ++i) {
    const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
    const Price px = side == Side::Bid ? kMid - off(rng) : kMid + off(rng);
    w.prefill.push_back(FeedMsg::add_limit(next_prefill++, 0, side, px, qty(rng)));
  }

  OrderId next_id = 1;
  for (std::size_t i = 0; i < n_ops; ++i) {
    const int p = pct(rng);
    if (p < 40) {  // passive add
      const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
      const Price px = side == Side::Bid ? kMid - off(rng) : kMid + off(rng);
      w.ops.push_back(FeedMsg::add_limit(next_id, 0, side, px, qty(rng)));
      live.push_back(next_id++);
    } else if (p < 45) {  // aggressive IOC crossing the touch
      const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
      const Price px = side == Side::Bid ? kMid + 2 : kMid - 2;
      w.ops.push_back(
          FeedMsg::add_limit(next_id++, 1, side, px, qty(rng), TimeInForce::IOC));
    } else if (p < 50) {  // market order
      const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
      w.ops.push_back(FeedMsg::add_market(next_id++, 1, side, qty(rng)));
    } else if (p < 90) {  // cancel
      if (!live.empty()) {
        const std::size_t j = rng() % live.size();
        w.ops.push_back(FeedMsg::cancel(live[j]));
        live[j] = live.back();
        live.pop_back();
      } else {
        w.ops.push_back(FeedMsg::cancel(next_id + (1ULL << 20)));
      }
    } else {  // modify: mostly qty reductions (priority-keeping path)
      if (!live.empty()) {
        const OrderId id = live[rng() % live.size()];
        const Side side = (rng() & 1) ? Side::Bid : Side::Ask;
        const Price px = side == Side::Bid ? kMid - off(rng) : kMid + off(rng);
        w.ops.push_back(FeedMsg::modify(id, px, qty(rng)));
      } else {
        w.ops.push_back(FeedMsg::modify(next_id + (1ULL << 20), kMid, 1));
      }
    }
    w.measure.push_back(1);
  }
  return w;
}

}  // namespace nanolob::bench
