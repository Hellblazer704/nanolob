#pragma once

#include <cstdint>
#include <deque>
#include <stdexcept>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nanolob/types.hpp"

namespace nanolob::test {

// Deliberately naive matching engine (std::map levels, std::deque FIFOs,
// linear scans) with semantics identical to nanolob::MatchingEngine. Exists
// purely as the oracle for randomized differential testing: same inputs must
// produce byte-identical event sequences and book state.
template <typename Handler>
class ReferenceEngine {
 public:
  explicit ReferenceEngine(Handler& handler, StpPolicy stp = StpPolicy::None)
      : handler_(handler), stp_(stp) {}

  bool add_limit(OrderId id, ParticipantId participant, Side side, Price price, Qty qty,
                 TimeInForce tif = TimeInForce::GTC) {
    if (!validate_new(id, qty)) return false;
    handler_.on_accept(id, side, price, qty);
    const Qty remaining = match(id, participant, side, qty, price, /*is_market=*/false);
    if (remaining == 0) return true;
    if (tif == TimeInForce::IOC) {
      handler_.on_cancel(id, CancelReason::IocRemainder);
      return true;
    }
    rest(id, participant, side, price, remaining);
    return true;
  }

  bool add_market(OrderId id, ParticipantId participant, Side side, Qty qty) {
    if (!validate_new(id, qty)) return false;
    handler_.on_accept(id, side, 0, qty);
    const Qty remaining = match(id, participant, side, qty, 0, /*is_market=*/true);
    if (remaining > 0) handler_.on_cancel(id, CancelReason::MarketRemainder);
    return true;
  }

  bool cancel(OrderId id) {
    auto it = live_.find(id);
    if (it == live_.end()) {
      handler_.on_reject(id, RejectReason::UnknownOrderId);
      return false;
    }
    unrest(id);
    handler_.on_cancel(id, CancelReason::User);
    return true;
  }

  bool modify(OrderId id, Price new_price, Qty new_qty) {
    auto it = live_.find(id);
    if (it == live_.end()) {
      handler_.on_reject(id, RejectReason::UnknownOrderId);
      return false;
    }
    if (new_qty == 0) {
      unrest(id);
      handler_.on_cancel(id, CancelReason::User);
      return true;
    }
    const LiveRef ref = it->second;
    RefOrder& order = find_order(id, ref);
    if (new_price == ref.price && new_qty <= order.remaining) {
      order.remaining = new_qty;
      return true;
    }
    const ParticipantId participant = order.participant;
    const Side side = ref.side;
    unrest(id);
    const Qty remaining = match(id, participant, side, new_qty, new_price, /*is_market=*/false);
    if (remaining > 0) rest(id, participant, side, new_price, remaining);
    return true;
  }

  // --- state queries (for cross-checking against the real engine) ---------

  [[nodiscard]] std::size_t open_orders() const { return live_.size(); }

  [[nodiscard]] Qty open_qty(OrderId id) const {
    auto it = live_.find(id);
    if (it == live_.end()) return 0;
    return const_cast<ReferenceEngine*>(this)->find_order(id, it->second).remaining;
  }

  [[nodiscard]] std::optional<std::pair<Price, Qty>> best(Side side) const {
    if (side == Side::Bid) {
      if (bids_.empty()) return std::nullopt;
      return std::make_pair(bids_.begin()->first, level_qty(bids_.begin()->second));
    }
    if (asks_.empty()) return std::nullopt;
    return std::make_pair(asks_.begin()->first, level_qty(asks_.begin()->second));
  }

  [[nodiscard]] Qty depth_at(Side side, Price price) const {
    if (side == Side::Bid) {
      auto it = bids_.find(price);
      return it == bids_.end() ? 0 : level_qty(it->second);
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : level_qty(it->second);
  }

  [[nodiscard]] std::vector<OrderId> live_ids() const {
    std::vector<OrderId> ids;
    ids.reserve(live_.size());
    for (const auto& [id, ref] : live_) ids.push_back(id);
    return ids;
  }

 private:
  struct RefOrder {
    OrderId id;
    ParticipantId participant;
    Qty remaining;
  };
  using Level = std::deque<RefOrder>;
  struct LiveRef {
    Side side;
    Price price;
  };

  bool validate_new(OrderId id, Qty qty) {
    if (qty == 0) {
      handler_.on_reject(id, RejectReason::ZeroQty);
      return false;
    }
    if (live_.contains(id)) {
      handler_.on_reject(id, RejectReason::DuplicateOrderId);
      return false;
    }
    return true;
  }

  static Qty level_qty(const Level& lvl) {
    Qty total = 0;
    for (const RefOrder& o : lvl) total += o.remaining;
    return total;
  }

  RefOrder& find_order(OrderId id, const LiveRef& ref) {
    Level& lvl = ref.side == Side::Bid ? bids_.at(ref.price) : asks_.at(ref.price);
    for (RefOrder& o : lvl) {
      if (o.id == id) return o;
    }
    throw std::logic_error("reference engine: live map points at missing order");
  }

  Qty match(OrderId id, ParticipantId participant, Side side, Qty qty, Price price,
            bool is_market) {
    Qty remaining = qty;
    while (remaining > 0) {
      Price lvl_price = 0;
      Level* lvl = nullptr;
      if (side == Side::Bid) {
        if (asks_.empty()) break;
        lvl_price = asks_.begin()->first;
        if (!is_market && lvl_price > price) break;
        lvl = &asks_.begin()->second;
      } else {
        if (bids_.empty()) break;
        lvl_price = bids_.begin()->first;
        if (!is_market && lvl_price < price) break;
        lvl = &bids_.begin()->second;
      }
      while (!lvl->empty() && remaining > 0) {
        RefOrder& maker = lvl->front();
        if (maker.participant == participant && stp_ != StpPolicy::None) {
          if (stp_ == StpPolicy::CancelIncoming) {
            handler_.on_cancel(id, CancelReason::SelfTradePrevention);
            return 0;
          }
          const OrderId maker_id = maker.id;
          live_.erase(maker_id);
          lvl->pop_front();
          handler_.on_cancel(maker_id, CancelReason::SelfTradePrevention);
          continue;
        }
        const Qty fill = remaining < maker.remaining ? remaining : maker.remaining;
        remaining -= fill;
        maker.remaining -= fill;
        handler_.on_trade(Trade{id, maker.id, lvl_price, fill, side});
        if (maker.remaining == 0) {
          live_.erase(maker.id);
          lvl->pop_front();
        }
      }
      if (lvl->empty()) {
        if (side == Side::Bid) {
          asks_.erase(lvl_price);
        } else {
          bids_.erase(lvl_price);
        }
      }
    }
    return remaining;
  }

  void rest(OrderId id, ParticipantId participant, Side side, Price price, Qty qty) {
    Level& lvl = side == Side::Bid ? bids_[price] : asks_[price];
    lvl.push_back(RefOrder{id, participant, qty});
    live_.emplace(id, LiveRef{side, price});
  }

  void unrest(OrderId id) {
    const LiveRef ref = live_.at(id);
    Level& lvl = ref.side == Side::Bid ? bids_.at(ref.price) : asks_.at(ref.price);
    for (auto it = lvl.begin(); it != lvl.end(); ++it) {
      if (it->id == id) {
        lvl.erase(it);
        break;
      }
    }
    if (lvl.empty()) {
      if (ref.side == Side::Bid) {
        bids_.erase(ref.price);
      } else {
        asks_.erase(ref.price);
      }
    }
    live_.erase(id);
  }

  std::map<Price, Level, std::greater<>> bids_;  // best (highest) first
  std::map<Price, Level> asks_;                  // best (lowest) first
  std::unordered_map<OrderId, LiveRef> live_;
  Handler& handler_;
  StpPolicy stp_;
};

}  // namespace nanolob::test
