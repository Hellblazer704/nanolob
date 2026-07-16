#pragma once

#include <cstdint>

#include "nanolob/types.hpp"

namespace nanolob {

// Compact order-entry message: what flows over the SPSC ring from the feed /
// gateway thread into the engine thread. 32 bytes so two fit per cache line
// and ring indexing stays cheap.
struct FeedMsg {
  enum class Kind : std::uint8_t { AddLimit, AddMarket, Cancel, Modify };

  Kind kind = Kind::AddLimit;
  Side side = Side::Bid;
  TimeInForce tif = TimeInForce::GTC;
  std::uint8_t _pad = 0;
  ParticipantId participant = 0;
  OrderId id = 0;
  Price price = 0;
  Qty qty = 0;

  static FeedMsg add_limit(OrderId id, ParticipantId p, Side s, Price px, Qty q,
                           TimeInForce tif = TimeInForce::GTC) {
    return FeedMsg{Kind::AddLimit, s, tif, 0, p, id, px, q};
  }
  static FeedMsg add_market(OrderId id, ParticipantId p, Side s, Qty q) {
    return FeedMsg{Kind::AddMarket, s, TimeInForce::IOC, 0, p, id, 0, q};
  }
  static FeedMsg cancel(OrderId id) {
    return FeedMsg{Kind::Cancel, Side::Bid, TimeInForce::GTC, 0, 0, id, 0, 0};
  }
  static FeedMsg modify(OrderId id, Price px, Qty q) {
    return FeedMsg{Kind::Modify, Side::Bid, TimeInForce::GTC, 0, 0, id, px, q};
  }
};
static_assert(sizeof(FeedMsg) == 32);

// Dispatch a feed message into any engine exposing the MatchingEngine API.
template <typename Engine>
inline void apply(Engine& engine, const FeedMsg& m) {
  switch (m.kind) {
    case FeedMsg::Kind::AddLimit:
      engine.add_limit(m.id, m.participant, m.side, m.price, m.qty, m.tif);
      break;
    case FeedMsg::Kind::AddMarket:
      engine.add_market(m.id, m.participant, m.side, m.qty);
      break;
    case FeedMsg::Kind::Cancel:
      engine.cancel(m.id);
      break;
    case FeedMsg::Kind::Modify:
      engine.modify(m.id, m.price, m.qty);
      break;
  }
}

}  // namespace nanolob
