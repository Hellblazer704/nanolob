#pragma once

// Quoting strategies. All price math is in integer ticks (doubles only for
// the intermediate optimization arithmetic); quantities in integer lots.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

#include "nanolob/types.hpp"

namespace nanolob::mm {

// EWMA variance of mid-price increments, normalized to ticks^2 per second.
// Irregular sampling is handled by scaling each squared increment by 1/dt.
class VarianceEstimator {
 public:
  explicit VarianceEstimator(double halflife_s = 30.0) : halflife_s_(halflife_s) {}

  void update(double mid_ticks, std::int64_t ts_ms) {
    if (have_last_) {
      const double dt = static_cast<double>(ts_ms - last_ts_ms_) / 1000.0;
      if (dt > 0) {
        const double r = mid_ticks - last_mid_;
        const double rate = (r * r) / dt;  // ticks^2 / s
        const double alpha = 1.0 - std::exp(-dt * std::numbers::ln2 / halflife_s_);
        var_rate_ = warmed_up_ ? (1.0 - alpha) * var_rate_ + alpha * rate : rate;
        warmed_up_ = true;
      }
    }
    last_mid_ = mid_ticks;
    last_ts_ms_ = ts_ms;
    have_last_ = true;
  }

  // Floored so downstream spread math never degenerates to zero width.
  [[nodiscard]] double var_rate() const { return std::max(var_rate_, 1e-6); }
  [[nodiscard]] bool ready() const { return warmed_up_; }

 private:
  double halflife_s_;
  double var_rate_ = 0.0;
  double last_mid_ = 0.0;
  std::int64_t last_ts_ms_ = 0;
  bool have_last_ = false;
  bool warmed_up_ = false;
};

struct MarketState {
  double mid_ticks = 0.0;
  Price best_bid = 0;
  Price best_ask = 0;
  double var_rate = 0.0;  // ticks^2 / s
  double q_units = 0.0;   // inventory in multiples of the quote size (signed)
};

struct QuotePair {
  std::optional<Price> bid;
  std::optional<Price> ask;
};

namespace detail {

// Never cross the market (these are passive quotes) and never quote a side
// that would push inventory past the cap.
inline QuotePair clamp(double raw_bid, double raw_ask, const MarketState& s, double q_max) {
  QuotePair q;
  Price bid = static_cast<Price>(std::llround(raw_bid));
  Price ask = static_cast<Price>(std::llround(raw_ask));
  bid = std::min(bid, s.best_ask - 1);
  ask = std::max(ask, s.best_bid + 1);
  if (ask <= bid) ask = bid + 1;
  if (s.q_units < q_max) q.bid = bid;
  if (s.q_units > -q_max) q.ask = ask;
  return q;
}

}  // namespace detail

// Fixed symmetric spread around the mid: the control group. Same clamping
// and inventory cap as A-S so the only difference is the quoting *policy*.
class NaiveFixedSpread {
 public:
  NaiveFixedSpread(double half_spread_ticks, double q_max)
      : half_spread_(half_spread_ticks), q_max_(q_max) {}

  [[nodiscard]] QuotePair quotes(const MarketState& s) const {
    return detail::clamp(s.mid_ticks - half_spread_, s.mid_ticks + half_spread_, s, q_max_);
  }

 private:
  double half_spread_;
  double q_max_;
};

// Avellaneda-Stoikov (2008) with a rolling horizon.
//
//   reservation r = m - q * gamma * sigma^2 * tau
//   half-spread  = (gamma * sigma^2 * tau) / 2 + (1/gamma) * ln(1 + gamma/k)
//
// sigma^2 is the live EWMA variance rate, tau a fixed horizon (the classic
// finite-horizon T-t collapses the book at end-of-day; for a continuously
// operating desk the standard adaptation is a constant tau). Inventory q is
// measured in units of the quote size. gamma is risk aversion (1/ticks per
// unit^2 here); k is the fill-intensity decay of arriving market orders
// (1/ticks) — larger k (fills die off quickly with distance) narrows the
// optimal spread.
class AvellanedaStoikov {
 public:
  AvellanedaStoikov(double gamma, double k, double tau_s, double q_max)
      : gamma_(gamma), k_(k), tau_(tau_s), q_max_(q_max) {}

  [[nodiscard]] QuotePair quotes(const MarketState& s) const {
    const double inv_term = gamma_ * s.var_rate * tau_;
    const double reservation = s.mid_ticks - s.q_units * inv_term;
    const double half_spread = 0.5 * inv_term + std::log1p(gamma_ / k_) / gamma_;
    return detail::clamp(reservation - half_spread, reservation + half_spread, s, q_max_);
  }

  [[nodiscard]] double reservation_skew(const MarketState& s) const {
    return -s.q_units * gamma_ * s.var_rate * tau_;
  }

 private:
  double gamma_;
  double k_;
  double tau_;
  double q_max_;
};

}  // namespace nanolob::mm
