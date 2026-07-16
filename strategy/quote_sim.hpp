#pragma once

// Virtual-quote fill simulation with queue-position modeling.
//
// Our quotes are NOT inserted into the mirrored book: diff quantities are
// absolute exchange totals, so a real insertion would double-count our size
// against the next update. Instead each quote is a virtual order whose FIFO
// queue position is estimated from observable flow:
//
//  - On placement we join the back: queue_ahead = current level quantity.
//  - Exchange trades at our price consume the front of the queue: they eat
//    queue_ahead first, any excess fills us.
//  - Trades *through* our price (aggressor got a better price than ours)
//    mean the whole level went: we are filled in full.
//  - Level-quantity drops beyond what trades explain are cancellations.
//    A cancellation's queue position is unobservable, so it is prorated:
//    ahead-of-us shrinks by cancel_qty * (queue_ahead / level_qty). This is
//    the standard neutral assumption; see BENCHMARKS/notebook discussion.
//  - Level growth joins BEHIND us and never improves our position.
//  - If the opposite best crosses our price (book moved through us without
//    a printed trade at our level), we are filled in full.
//
// Fills are assumed to happen at our quoted price; no fees or rebates.

#include <cstdint>
#include <optional>
#include <vector>

#include "nanolob/types.hpp"
#include "replay/binance_parser.hpp"

namespace nanolob::mm {

struct Fill {
  std::int64_t ts = 0;
  Side side = Side::Bid;  // side of OUR order (Bid = we bought)
  Price price = 0;
  Qty qty = 0;
  Price mid_at_fill = 0;    // mid immediately before the fill, in ticks*2 (see note)
  std::int64_t queue_wait_ms = 0;
};

struct VirtualQuote {
  bool active = false;
  Side side = Side::Bid;
  Price price = 0;
  Qty remaining = 0;
  double queue_ahead = 0.0;
  Qty level_qty_seen = 0;  // exchange qty at our level, maintained between diffs
  std::int64_t placed_ts = 0;
};

// One side's virtual order plus the fill bookkeeping shared by both sides.
class QuoteSimulator {
 public:
  // mid2 = best_bid + best_ask (twice the mid, kept integral). Stored per
  // fill so edge/markout math downstream never touches floating point ticks.
  void place(Side side, Price price, Qty size, Qty level_qty, std::int64_t ts) {
    VirtualQuote& q = quote(side);
    q.active = true;
    q.side = side;
    q.price = price;
    q.remaining = size;
    q.queue_ahead = static_cast<double>(level_qty);
    q.level_qty_seen = level_qty;
    q.placed_ts = ts;
  }

  void cancel(Side side) { quote(side).active = false; }

  [[nodiscard]] const VirtualQuote& get(Side side) const {
    return side == Side::Bid ? bid_ : ask_;
  }

  // Exchange trade print. `buyer_is_maker == true` means a sell aggressor
  // (it consumes bids); false means a buy aggressor (consumes asks).
  void on_trade(const replay::TradeRec& t, Price mid2) {
    if (t.buyer_is_maker) {
      consume(bid_, t, /*through=*/t.price < bid_.price, mid2);
    } else {
      consume(ask_, t, /*through=*/t.price > ask_.price, mid2);
    }
  }

  // Level update at our quote's price (call only when side+price match).
  void on_level_update(Side side, Qty new_qty) {
    VirtualQuote& q = quote(side);
    if (!q.active) return;
    const Qty old = q.level_qty_seen;
    if (new_qty < old && old > 0) {
      const auto cancelled = static_cast<double>(old - new_qty);
      q.queue_ahead -= cancelled * (q.queue_ahead / static_cast<double>(old));
      if (q.queue_ahead < 0) q.queue_ahead = 0;
    }
    q.level_qty_seen = new_qty;
  }

  // Opposite best moved through our price without a print at our level.
  void on_cross(Side side, std::int64_t ts, Price mid2) {
    VirtualQuote& q = quote(side);
    if (!q.active || q.remaining == 0) return;
    emit_fill(q, q.remaining, ts, mid2);
  }

  [[nodiscard]] const std::vector<Fill>& fills() const { return fills_; }
  std::vector<Fill>& fills() { return fills_; }

 private:
  VirtualQuote& quote(Side side) { return side == Side::Bid ? bid_ : ask_; }

  void consume(VirtualQuote& q, const replay::TradeRec& t, bool through, Price mid2) {
    if (!q.active || q.remaining == 0) return;
    if (through) {
      emit_fill(q, q.remaining, t.ts, mid2);
      return;
    }
    if (t.price != q.price) return;
    double consumed = static_cast<double>(t.qty);
    // Track the exchange level shrinking so cancel proration stays honest
    // between diffs.
    q.level_qty_seen = t.qty >= q.level_qty_seen ? 0 : q.level_qty_seen - t.qty;
    if (q.queue_ahead > 0) {
      const double eaten = consumed < q.queue_ahead ? consumed : q.queue_ahead;
      q.queue_ahead -= eaten;
      consumed -= eaten;
    }
    if (consumed <= 0) return;
    const Qty fill_qty =
        consumed >= static_cast<double>(q.remaining)
            ? q.remaining
            : static_cast<Qty>(consumed);
    emit_fill(q, fill_qty, t.ts, mid2);
  }

  void emit_fill(VirtualQuote& q, Qty qty, std::int64_t ts, Price mid2) {
    fills_.push_back(Fill{ts, q.side, q.price, qty, mid2, ts - q.placed_ts});
    q.remaining -= qty;
    if (q.remaining == 0) q.active = false;
  }

  VirtualQuote bid_{};
  VirtualQuote ask_{};
  std::vector<Fill> fills_;
};

}  // namespace nanolob::mm
