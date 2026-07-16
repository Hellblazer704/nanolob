// Market-making simulator: replays a Binance capture through the book
// mirror, quotes with the selected strategy, and simulates fills with
// queue-position modeling (see quote_sim.hpp for the fill model and its
// assumptions). Writes two CSVs for the plotting script / notebook:
//   <prefix>_timeseries.csv  one row per 100ms diff step
//   <prefix>_fills.csv       one row per simulated fill
//
// PnL accounting (all USDT): pnl = cash + inventory * mid. Spread capture is
// summed per fill as qty * (mid_at_fill - price) signed toward us; the
// inventory/adverse-selection component is the exact residual
// pnl - spread_pnl, i.e. the mark-to-market drift of held inventory. A fill
// that instantly looks like spread capture and then bleeds as the mid moves
// through the quote shows up as spread_pnl up, inv_pnl down — which is
// precisely adverse selection (quantified per-fill by the markout analysis
// in the notebook).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "nanolob/engine.hpp"
#include "replay/binance_parser.hpp"
#include "replay/book_mirror.hpp"
#include "strategy/quote_sim.hpp"
#include "strategy/strategies.hpp"

namespace nanolob::mm {
namespace {

using replay::DiffRec;
using replay::Record;
using replay::SnapshotRec;

// tick = 0.01 USDT, lot = 1e-5 BTC => 1 tick*lot = 1e-7 USDT.
constexpr double kTickLotToUsdt = 1e-7;
constexpr double kTickToUsdt = 0.01;

struct SinkHandler {
  std::uint64_t trades = 0;
  void on_accept(OrderId, Side, Price, Qty) {}
  void on_trade(const Trade&) { ++trades; }
  void on_cancel(OrderId, CancelReason) {}
  void on_reject(OrderId, RejectReason) {}
};
using Engine = MatchingEngine<SinkHandler>;

struct Params {
  std::string capture = "data/btcusdt_capture.jsonl";
  std::string strategy = "as";  // "as" | "naive"
  std::string out_prefix = "results/as";
  Qty size = 2000;              // 0.02 BTC
  double gamma = 2e-6;
  double k = 0.5;
  double tau_s = 10.0;
  double q_max = 5.0;           // inventory cap, in quote-size units
  double naive_half_spread = 4.0;
  double warmup_s = 60.0;       // no quoting until the vol estimator settles
};

struct Account {
  double cash_usdt = 0.0;
  std::int64_t inventory_lots = 0;  // signed
  double spread_pnl_usdt = 0.0;

  void on_fill(const Fill& f) {
    const double px_usdt = static_cast<double>(f.price) * kTickToUsdt;
    const double qty_btc = static_cast<double>(f.qty) * 1e-5;
    const double mid_ticks = static_cast<double>(f.mid_at_fill) / 2.0;
    const double edge_ticks = f.side == Side::Bid
                                  ? mid_ticks - static_cast<double>(f.price)
                                  : static_cast<double>(f.price) - mid_ticks;
    if (f.side == Side::Bid) {
      cash_usdt -= px_usdt * qty_btc;
      inventory_lots += static_cast<std::int64_t>(f.qty);
    } else {
      cash_usdt += px_usdt * qty_btc;
      inventory_lots -= static_cast<std::int64_t>(f.qty);
    }
    spread_pnl_usdt += edge_ticks * static_cast<double>(f.qty) * kTickLotToUsdt;
  }

  [[nodiscard]] double pnl_usdt(double mid_ticks) const {
    return cash_usdt +
           static_cast<double>(inventory_lots) * 1e-5 * mid_ticks * kTickToUsdt;
  }
};

int run(const Params& p) {
  std::ifstream in(p.capture);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", p.capture.c_str());
    return 1;
  }

  SinkHandler handler;
  Engine engine(handler, StpPolicy::None, std::size_t{1} << 16);
  replay::BookMirror<Engine> mirror(engine);
  bool seeded = false;

  VarianceEstimator vol(30.0);
  NaiveFixedSpread naive(p.naive_half_spread, p.q_max);
  AvellanedaStoikov as(p.gamma, p.k, p.tau_s, p.q_max);
  const bool use_as = p.strategy == "as";

  QuoteSimulator sim;
  Account acct;
  std::size_t fills_seen = 0;

  std::ofstream ts_out(p.out_prefix + "_timeseries.csv");
  std::ofstream fills_out(p.out_prefix + "_fills.csv");
  ts_out << "ts_ms,mid_ticks,best_bid,best_ask,sigma_ticks_per_sqrt_s,inventory_lots,"
            "cash_usdt,pnl_usdt,spread_pnl_usdt,inv_pnl_usdt,bid_px,ask_px,"
            "bid_queue_ahead,ask_queue_ahead\n";
  fills_out << "ts_ms,side,price_ticks,qty_lots,mid2_at_fill,edge_ticks,queue_wait_ms\n";

  std::int64_t t0_ms = 0;
  std::string line;
  std::size_t diffs = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto rec = replay::parse_line(line);
    if (!rec) continue;

    switch (rec->kind) {
      case Record::Kind::Snapshot:
        if (!seeded) {
          mirror.apply_snapshot(rec->snapshot);
          seeded = true;
        }
        break;

      case Record::Kind::Trade: {
        if (!seeded) break;
        const PriceLevel* bb = engine.best_bid();
        const PriceLevel* ba = engine.best_ask();
        if (bb && ba) sim.on_trade(rec->trade, bb->price + ba->price);
        break;
      }

      case Record::Kind::Diff: {
        if (!seeded) break;
        mirror.apply_diff(rec->diff);
        ++diffs;

        const PriceLevel* bb = engine.best_bid();
        const PriceLevel* ba = engine.best_ask();
        if (!bb || !ba) break;
        const std::int64_t ts = rec->diff.ts;
        if (t0_ms == 0) t0_ms = ts;
        const double mid = static_cast<double>(bb->price + ba->price) / 2.0;
        vol.update(mid, ts);

        // Update our virtual quotes against the new book state.
        for (const Side side : {Side::Bid, Side::Ask}) {
          const VirtualQuote& q = sim.get(side);
          if (!q.active) continue;
          sim.on_level_update(side, engine.book().depth_at(side, q.price));
          const bool crossed = side == Side::Bid ? ba->price <= q.price : bb->price >= q.price;
          if (crossed) sim.on_cross(side, ts, bb->price + ba->price);
        }

        // Requote.
        const bool warm = vol.ready() && static_cast<double>(ts - t0_ms) / 1000.0 >= p.warmup_s;
        MarketState state{mid, bb->price, ba->price, vol.var_rate(),
                          static_cast<double>(acct.inventory_lots) / static_cast<double>(p.size)};
        const QuotePair want =
            warm ? (use_as ? as.quotes(state) : naive.quotes(state)) : QuotePair{};
        for (const Side side : {Side::Bid, Side::Ask}) {
          const auto desired = side == Side::Bid ? want.bid : want.ask;
          const VirtualQuote& q = sim.get(side);
          if (!desired.has_value()) {
            if (q.active) sim.cancel(side);
            continue;
          }
          // Same price: stay put and keep queue position.
          if (q.active && q.price == *desired) continue;
          sim.cancel(side);
          sim.place(side, *desired, p.size, engine.book().depth_at(side, *desired), ts);
        }

        // Account for any fills produced this step.
        for (; fills_seen < sim.fills().size(); ++fills_seen) {
          const Fill& f = sim.fills()[fills_seen];
          acct.on_fill(f);
          const double fmid = static_cast<double>(f.mid_at_fill) / 2.0;
          const double edge = f.side == Side::Bid ? fmid - static_cast<double>(f.price)
                                                  : static_cast<double>(f.price) - fmid;
          fills_out << f.ts << ',' << (f.side == Side::Bid ? 'B' : 'S') << ',' << f.price << ','
                    << f.qty << ',' << f.mid_at_fill << ',' << edge << ',' << f.queue_wait_ms
                    << '\n';
        }

        const double pnl = acct.pnl_usdt(mid);
        const VirtualQuote& qb = sim.get(Side::Bid);
        const VirtualQuote& qa = sim.get(Side::Ask);
        ts_out << ts << ',' << mid << ',' << bb->price << ',' << ba->price << ','
               << std::sqrt(vol.var_rate()) << ',' << acct.inventory_lots << ','
               << acct.cash_usdt << ',' << pnl << ',' << acct.spread_pnl_usdt << ','
               << pnl - acct.spread_pnl_usdt << ',' << (qb.active ? qb.price : 0) << ','
               << (qa.active ? qa.price : 0) << ',' << (qb.active ? qb.queue_ahead : 0.0) << ','
               << (qa.active ? qa.queue_ahead : 0.0) << '\n';
        break;
      }
    }
  }

  const PriceLevel* bb = engine.best_bid();
  const PriceLevel* ba = engine.best_ask();
  const double mid = bb && ba ? static_cast<double>(bb->price + ba->price) / 2.0 : 0.0;
  std::printf("%s: %zu diffs, %zu fills, final inventory %lld lots, "
              "pnl %.4f USDT (spread %.4f, inventory %.4f)\n",
              p.strategy.c_str(), diffs, sim.fills().size(),
              static_cast<long long>(acct.inventory_lots), acct.pnl_usdt(mid),
              acct.spread_pnl_usdt, acct.pnl_usdt(mid) - acct.spread_pnl_usdt);
  return 0;
}

}  // namespace
}  // namespace nanolob::mm

int main(int argc, char** argv) {
  nanolob::mm::Params p;
  for (int i = 1; i < argc - 1; ++i) {
    const std::string a = argv[i];
    const char* v = argv[i + 1];
    if (a == "--capture") p.capture = v;
    else if (a == "--strategy") p.strategy = v;
    else if (a == "--out-prefix") p.out_prefix = v;
    else if (a == "--size") p.size = std::strtoull(v, nullptr, 10);
    else if (a == "--gamma") p.gamma = std::strtod(v, nullptr);
    else if (a == "--k") p.k = std::strtod(v, nullptr);
    else if (a == "--tau") p.tau_s = std::strtod(v, nullptr);
    else if (a == "--qmax") p.q_max = std::strtod(v, nullptr);
    else if (a == "--half-spread") p.naive_half_spread = std::strtod(v, nullptr);
    else if (a == "--warmup") p.warmup_s = std::strtod(v, nullptr);
  }
  return nanolob::mm::run(p);
}
