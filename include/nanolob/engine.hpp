#pragma once

#include <cstddef>

#include "nanolob/book.hpp"
#include "nanolob/flat_hash_map.hpp"
#include "nanolob/pool.hpp"
#include "nanolob/types.hpp"

namespace nanolob {

// Event sink that ignores everything. Also documents the handler concept:
// the engine is templated on the handler so event dispatch inlines away —
// no virtual calls on the hot path.
struct NullHandler {
  void on_accept(OrderId, Side, Price, Qty) {}
  void on_trade(const Trade&) {}
  void on_cancel(OrderId, CancelReason) {}
  void on_reject(OrderId, RejectReason) {}
};

// Price-time priority matching engine.
//
// Semantics:
//  - Limit GTC: match against the opposite side while crossing, rest the
//    remainder.
//  - Limit IOC: same matching, remainder is cancelled (CancelReason::IocRemainder).
//  - Market: matches at any price until filled or the opposite side is empty;
//    the unfilled remainder is cancelled (CancelReason::MarketRemainder).
//  - Cancel: O(1) unlink via the order-id map.
//  - Modify: reducing quantity at the same price keeps queue priority;
//    changing price or increasing quantity is cancel/replace — the order
//    loses priority and the replacement may match immediately.
//  - Self-trade prevention (per-engine policy): when the incoming order would
//    trade with a resting order of the same participant, either the resting
//    order or the incoming remainder is cancelled with
//    CancelReason::SelfTradePrevention. Fills already done stand.
//
// Order ids are caller-assigned and must be unique among *live* orders;
// reusing the id of a fully filled/cancelled order is allowed (as on most
// real venues, where uniqueness is per-day and enforced only on live ids).
template <typename Handler>
class MatchingEngine {
 public:
  explicit MatchingEngine(Handler& handler, StpPolicy stp = StpPolicy::None,
                          std::size_t expected_orders = 1 << 16)
      : orders_by_id_(expected_orders * 2), handler_(handler), stp_(stp) {
    order_pool_.reserve(expected_orders);
    level_pool_.reserve(4096);
  }

  // --- order entry -------------------------------------------------------

  bool add_limit(OrderId id, ParticipantId participant, Side side, Price price, Qty qty,
                 TimeInForce tif = TimeInForce::GTC) {
    if (!validate_new(id, qty)) return false;
    handler_.on_accept(id, side, price, qty);
    const Qty remaining = match_incoming(id, participant, side, qty, price, /*is_market=*/false);
    if (remaining == 0) return true;
    if (tif == TimeInForce::IOC) {
      handler_.on_cancel(id, CancelReason::IocRemainder);
      return true;
    }
    rest_order(id, participant, side, price, remaining);
    return true;
  }

  bool add_market(OrderId id, ParticipantId participant, Side side, Qty qty) {
    if (!validate_new(id, qty)) return false;
    handler_.on_accept(id, side, 0, qty);
    const Qty remaining = match_incoming(id, participant, side, qty, 0, /*is_market=*/true);
    if (remaining > 0) handler_.on_cancel(id, CancelReason::MarketRemainder);
    return true;
  }

  bool cancel(OrderId id) {
    Order** found = orders_by_id_.find(id);
    if (found == nullptr) {
      handler_.on_reject(id, RejectReason::UnknownOrderId);
      return false;
    }
    remove_resting(*found);
    handler_.on_cancel(id, CancelReason::User);
    return true;
  }

  // Set a live order's price and remaining quantity. Quantity reduction at
  // the same price keeps queue position; anything else is cancel/replace.
  bool modify(OrderId id, Price new_price, Qty new_qty) {
    Order** found = orders_by_id_.find(id);
    if (found == nullptr) {
      handler_.on_reject(id, RejectReason::UnknownOrderId);
      return false;
    }
    Order* order = *found;
    if (new_qty == 0) {
      remove_resting(order);
      handler_.on_cancel(id, CancelReason::User);
      return true;
    }
    if (new_price == order->price && new_qty <= order->remaining) {
      const Qty delta = order->remaining - new_qty;
      order->remaining = new_qty;
      order->level->total_qty -= delta;
      return true;
    }
    // Cancel/replace: preserve participant/side, lose time priority, and let
    // the replacement match if it now crosses.
    const ParticipantId participant = order->participant;
    const Side side = order->side;
    remove_resting(order);
    const Qty remaining =
        match_incoming(id, participant, side, new_qty, new_price, /*is_market=*/false);
    if (remaining > 0) rest_order(id, participant, side, new_price, remaining);
    return true;
  }

  // --- queries ------------------------------------------------------------

  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
  [[nodiscard]] OrderBook& book() noexcept { return book_; }
  [[nodiscard]] const PriceLevel* best_bid() const noexcept { return book_.best_bid(); }
  [[nodiscard]] const PriceLevel* best_ask() const noexcept { return book_.best_ask(); }
  [[nodiscard]] std::size_t open_orders() const noexcept { return orders_by_id_.size(); }

  // Remaining qty of a live order, or 0 if unknown.
  [[nodiscard]] Qty open_qty(OrderId id) noexcept {
    Order** found = orders_by_id_.find(id);
    return found != nullptr ? (*found)->remaining : 0;
  }

 private:
  bool validate_new(OrderId id, Qty qty) {
    if (qty == 0) {
      handler_.on_reject(id, RejectReason::ZeroQty);
      return false;
    }
    if (orders_by_id_.find(id) != nullptr) {
      handler_.on_reject(id, RejectReason::DuplicateOrderId);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool crosses(Side taker_side, Price taker_price, Price maker_price,
                             bool is_market) const noexcept {
    if (is_market) return true;
    return taker_side == Side::Bid ? taker_price >= maker_price : taker_price <= maker_price;
  }

  // Match an incoming order against the opposite side; returns unmatched qty.
  Qty match_incoming(OrderId id, ParticipantId participant, Side side, Qty qty, Price price,
                     bool is_market) {
    BookSide& opp = book_.side(opposite(side));
    Qty remaining = qty;
    while (remaining > 0) {
      PriceLevel* lvl = opp.best();
      if (lvl == nullptr || !crosses(side, price, lvl->price, is_market)) break;
      Order* maker = lvl->front;
      const Price lvl_price = lvl->price;
      while (maker != nullptr && remaining > 0) {
        Order* next = maker->next;
        if (maker->participant == participant && stp_ != StpPolicy::None) {
          if (stp_ == StpPolicy::CancelIncoming) {
            handler_.on_cancel(id, CancelReason::SelfTradePrevention);
            return 0;  // remainder is gone; nothing rests
          }
          // CancelResting: pull the resting order, keep matching.
          const OrderId maker_id = maker->id;
          remove_resting(maker);
          handler_.on_cancel(maker_id, CancelReason::SelfTradePrevention);
          maker = next;
          continue;
        }
        const Qty fill = remaining < maker->remaining ? remaining : maker->remaining;
        remaining -= fill;
        maker->remaining -= fill;
        lvl->total_qty -= fill;
        handler_.on_trade(Trade{id, maker->id, lvl_price, fill, side});
        if (maker->remaining == 0) {
          orders_by_id_.erase(maker->id);
          opp.remove_order(maker, level_pool_);  // frees the level if empty,
          order_pool_.dealloc(maker);            // invalidating lvl — see below
        }
        maker = next;
      }
      // `lvl` may have been freed when its last order filled; re-read from
      // best() at the top of the loop rather than touching it again.
    }
    return remaining;
  }

  void rest_order(OrderId id, ParticipantId participant, Side side, Price price, Qty qty) {
    Order* order = order_pool_.alloc();
    order->id = id;
    order->participant = participant;
    order->side = side;
    order->price = price;
    order->remaining = qty;
    BookSide& own = book_.side(side);
    PriceLevel* lvl = own.get_or_create_level(price, level_pool_);
    own.push_order(lvl, order);
    orders_by_id_.insert(id, order);
  }

  void remove_resting(Order* order) {
    orders_by_id_.erase(order->id);
    book_.side(order->side).remove_order(order, level_pool_);
    order_pool_.dealloc(order);
  }

  OrderBook book_;
  Pool<Order> order_pool_;
  Pool<PriceLevel> level_pool_;
  FlatHashMap<OrderId, Order*> orders_by_id_;
  Handler& handler_;
  StpPolicy stp_;
};

}  // namespace nanolob
