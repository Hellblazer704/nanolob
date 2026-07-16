#pragma once

#include <cstdint>

#include "nanolob/flat_hash_map.hpp"
#include "nanolob/pool.hpp"
#include "nanolob/types.hpp"

namespace nanolob {

struct PriceLevel;

// One resting order. Doubles as the intrusive node of its price level's FIFO
// list — no separate node allocation, and unlink-by-pointer makes cancel O(1).
// Padded to exactly one cache line so pool-adjacent orders never false-share
// and a fill touches a single line.
struct alignas(kCacheLine) Order {
  Order* prev = nullptr;  // toward the front (older) of the FIFO
  Order* next = nullptr;  // toward the back (newer) of the FIFO
  PriceLevel* level = nullptr;
  OrderId id = 0;
  Qty remaining = 0;
  Price price = 0;
  ParticipantId participant = 0;
  Side side = Side::Bid;
};
static_assert(sizeof(Order) == kCacheLine, "Order must stay a single cache line");

// One price level: FIFO queue of orders plus its neighbours in the side's
// price-sorted level list ("better" = closer to the touch).
struct alignas(kCacheLine) PriceLevel {
  PriceLevel* better = nullptr;
  PriceLevel* worse = nullptr;
  Order* front = nullptr;  // oldest order: first to fill
  Order* back = nullptr;   // newest order: last to fill
  Price price = 0;
  Qty total_qty = 0;
  std::uint32_t order_count = 0;
};
static_assert(sizeof(PriceLevel) == kCacheLine, "PriceLevel must stay a single cache line");

// One side of the book. Levels live in two structures at once:
//  - a flat hash map price -> level for O(1) lookup when an order arrives at
//    an existing level (the overwhelmingly common case), and
//  - an intrusive doubly-linked list sorted best-to-worst, walked from the
//    best level, for matching, BBO queries, and inserting brand-new levels.
// Real order flow concentrates near the touch, so the sorted-insert walk is
// a handful of hops in practice; in exchange the hot paths never touch a
// tree (std::map would be a cache-missing pointer chase + node allocation).
class BookSide {
 public:
  explicit BookSide(Side side, std::size_t expected_levels = 1024)
      : levels_by_price_(expected_levels), side_(side) {}

  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] PriceLevel* best() const noexcept { return best_; }
  [[nodiscard]] bool empty() const noexcept { return best_ == nullptr; }

  // "a is closer to the touch than b" for this side.
  [[nodiscard]] bool better(Price a, Price b) const noexcept {
    return side_ == Side::Bid ? a > b : a < b;
  }

  [[nodiscard]] PriceLevel* find_level(Price price) noexcept {
    PriceLevel** found = levels_by_price_.find(price);
    return found ? *found : nullptr;
  }

  PriceLevel* get_or_create_level(Price price, Pool<PriceLevel>& level_pool) {
    if (PriceLevel* existing = find_level(price)) return existing;
    PriceLevel* lvl = level_pool.alloc();
    lvl->price = price;
    if (best_ == nullptr || better(price, best_->price)) {
      lvl->worse = best_;
      if (best_ != nullptr) best_->better = lvl;
      best_ = lvl;
    } else {
      PriceLevel* pos = best_;  // last level better than `price`
      while (pos->worse != nullptr && better(pos->worse->price, price)) pos = pos->worse;
      lvl->better = pos;
      lvl->worse = pos->worse;
      if (pos->worse != nullptr) pos->worse->better = lvl;
      pos->worse = lvl;
    }
    levels_by_price_.insert(price, lvl);
    return lvl;
  }

  // Append at the back of the level FIFO (price-time priority: latest last).
  void push_order(PriceLevel* lvl, Order* order) noexcept {
    order->level = lvl;
    order->prev = lvl->back;
    order->next = nullptr;
    if (lvl->back != nullptr) {
      lvl->back->next = order;
    } else {
      lvl->front = order;
    }
    lvl->back = order;
    lvl->total_qty += order->remaining;
    ++lvl->order_count;
  }

  // Unlink an order; frees the level too if it empties. `order->remaining`
  // must already reflect any fills (it is subtracted from the level total).
  void remove_order(Order* order, Pool<PriceLevel>& level_pool) noexcept {
    PriceLevel* lvl = order->level;
    if (order->prev != nullptr) {
      order->prev->next = order->next;
    } else {
      lvl->front = order->next;
    }
    if (order->next != nullptr) {
      order->next->prev = order->prev;
    } else {
      lvl->back = order->prev;
    }
    lvl->total_qty -= order->remaining;
    --lvl->order_count;
    if (lvl->order_count == 0) remove_level(lvl, level_pool);
  }

  void remove_level(PriceLevel* lvl, Pool<PriceLevel>& level_pool) noexcept {
    if (lvl->better != nullptr) {
      lvl->better->worse = lvl->worse;
    } else {
      best_ = lvl->worse;
    }
    if (lvl->worse != nullptr) lvl->worse->better = lvl->better;
    levels_by_price_.erase(lvl->price);
    level_pool.dealloc(lvl);
  }

 private:
  PriceLevel* best_ = nullptr;
  FlatHashMap<Price, PriceLevel*> levels_by_price_;
  Side side_;
};

// Passive book structure: two sides + convenience queries. All order
// lifecycle logic (matching, STP, events) lives in MatchingEngine.
class OrderBook {
 public:
  explicit OrderBook(std::size_t expected_levels_per_side = 1024)
      : bids_(Side::Bid, expected_levels_per_side), asks_(Side::Ask, expected_levels_per_side) {}

  [[nodiscard]] BookSide& side(Side s) noexcept { return s == Side::Bid ? bids_ : asks_; }
  [[nodiscard]] const BookSide& side(Side s) const noexcept {
    return s == Side::Bid ? bids_ : asks_;
  }
  [[nodiscard]] BookSide& bids() noexcept { return bids_; }
  [[nodiscard]] BookSide& asks() noexcept { return asks_; }

  [[nodiscard]] const PriceLevel* best_bid() const noexcept { return bids_.best(); }
  [[nodiscard]] const PriceLevel* best_ask() const noexcept { return asks_.best(); }

  // Qty resting at an exact price on a side (0 if no level).
  [[nodiscard]] Qty depth_at(Side s, Price price) noexcept {
    PriceLevel* lvl = side(s).find_level(price);
    return lvl != nullptr ? lvl->total_qty : 0;
  }

 private:
  BookSide bids_;
  BookSide asks_;
};

}  // namespace nanolob
