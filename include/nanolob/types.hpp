#pragma once

#include <cstddef>
#include <cstdint>

namespace nanolob {

// Prices are integer ticks, quantities integer lots. All fixed-point scaling
// (e.g. BTCUSDT 0.01 tick / 1e-8 lot) happens at the feed boundary, never
// inside the engine.
using Price = std::int64_t;
using Qty = std::uint64_t;
using OrderId = std::uint64_t;
using ParticipantId = std::uint32_t;

inline constexpr std::size_t kCacheLine = 64;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Bid ? Side::Ask : Side::Bid;
}

enum class TimeInForce : std::uint8_t {
  GTC = 0,  // rest any unmatched remainder on the book
  IOC = 1,  // cancel any unmatched remainder immediately
};

// Self-trade prevention policy, applied when an incoming order would match a
// resting order from the same participant.
enum class StpPolicy : std::uint8_t {
  None = 0,            // allow the self-match (useful for replay/simulation)
  CancelResting = 1,   // cancel the resting order, keep matching the incoming
  CancelIncoming = 2,  // cancel the incoming order's remainder, resting stays
};

enum class CancelReason : std::uint8_t {
  User = 0,             // explicit cancel request
  IocRemainder = 1,     // IOC remainder after matching
  MarketRemainder = 2,  // market order remainder, opposite side exhausted
  SelfTradePrevention = 3,
};

enum class RejectReason : std::uint8_t {
  DuplicateOrderId = 0,
  UnknownOrderId = 1,
  ZeroQty = 2,
};

struct Trade {
  OrderId taker_id;
  OrderId maker_id;
  Price price;  // execution price = maker's resting price
  Qty qty;
  Side taker_side;  // aggressor side
};

}  // namespace nanolob
