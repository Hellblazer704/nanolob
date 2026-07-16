#pragma once

#include <ostream>
#include <vector>

#include "nanolob/types.hpp"

namespace nanolob::test {

// Flattened engine event, comparable field-by-field so tests can assert on
// exact event sequences.
struct Event {
  enum class Type : std::uint8_t { Accept, Trade, Cancel, Reject };

  Type type = Type::Accept;
  OrderId id = 0;     // subject order (taker for trades)
  OrderId maker = 0;  // trades only
  Price price = 0;    // accepts (limit price; 0 for market) and trades
  Qty qty = 0;        // accepts (full qty) and trades (fill qty)
  Side side = Side::Bid;
  CancelReason cancel_reason = CancelReason::User;
  RejectReason reject_reason = RejectReason::UnknownOrderId;

  friend bool operator==(const Event&, const Event&) = default;

  static Event accept(OrderId id, Side side, Price price, Qty qty) {
    return Event{.type = Type::Accept, .id = id, .price = price, .qty = qty, .side = side};
  }
  static Event trade(OrderId taker, OrderId maker, Price price, Qty qty, Side taker_side) {
    return Event{.type = Type::Trade,
                 .id = taker,
                 .maker = maker,
                 .price = price,
                 .qty = qty,
                 .side = taker_side};
  }
  static Event cancel(OrderId id, CancelReason reason) {
    return Event{.type = Type::Cancel, .id = id, .cancel_reason = reason};
  }
  static Event reject(OrderId id, RejectReason reason) {
    return Event{.type = Type::Reject, .id = id, .reject_reason = reason};
  }
};

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
  switch (e.type) {
    case Event::Type::Accept:
      return os << "Accept{id=" << e.id << " side=" << (e.side == Side::Bid ? "B" : "A")
                << " px=" << e.price << " qty=" << e.qty << "}";
    case Event::Type::Trade:
      return os << "Trade{taker=" << e.id << " maker=" << e.maker << " px=" << e.price
                << " qty=" << e.qty << " aggressor=" << (e.side == Side::Bid ? "B" : "A") << "}";
    case Event::Type::Cancel:
      return os << "Cancel{id=" << e.id << " reason=" << static_cast<int>(e.cancel_reason) << "}";
    case Event::Type::Reject:
      return os << "Reject{id=" << e.id << " reason=" << static_cast<int>(e.reject_reason) << "}";
  }
  return os;
}

struct RecordingHandler {
  std::vector<Event> events;

  void on_accept(OrderId id, Side side, Price price, Qty qty) {
    events.push_back(Event::accept(id, side, price, qty));
  }
  void on_trade(const Trade& t) {
    events.push_back(Event::trade(t.taker_id, t.maker_id, t.price, t.qty, t.taker_side));
  }
  void on_cancel(OrderId id, CancelReason reason) {
    events.push_back(Event::cancel(id, reason));
  }
  void on_reject(OrderId id, RejectReason reason) {
    events.push_back(Event::reject(id, reason));
  }
};

}  // namespace nanolob::test
