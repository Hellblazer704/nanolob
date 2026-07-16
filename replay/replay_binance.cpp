// Replays a capture produced by scripts/download_binance.py through the
// matching engine and validates the reconstructed book against the
// exchange's own snapshots.
//
// Sequencing follows the Binance book-management protocol: seed from the
// first snapshot, drop diffs with u <= lastUpdateId, require the first
// applied diff to straddle lastUpdateId+1, then require U == prev_u + 1.
//
// Validation exploits the fact that diffs carry *absolute* quantities and
// are therefore idempotent: for each later snapshot, a shadow book is seeded
// from it and both books then consume the identical diff stream. The
// straddling diff re-applies a prefix the snapshot already contains — which
// is harmless — and after a settling window the two books must agree
// *exactly* on every level in the snapshot's price range. Any drift the
// main book accumulated earlier would persist as a difference on levels the
// diffs no longer touch, so this check catches real desync, not just
// recent-window agreement.
#include <chrono>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nanolob/engine.hpp"
#include "replay/binance_parser.hpp"
#include "replay/book_mirror.hpp"

namespace nanolob::replay {
namespace {

struct SinkHandler {
  std::uint64_t trades = 0;  // phantom matches during mirroring: anomalies
  void on_accept(OrderId, Side, Price, Qty) {}
  void on_trade(const Trade&) { ++trades; }
  void on_cancel(OrderId, CancelReason) {}
  void on_reject(OrderId, RejectReason) {}
};

using Engine = MatchingEngine<SinkHandler>;

struct MirroredBook {
  SinkHandler handler;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<BookMirror<Engine>> mirror;
  std::uint64_t last_u = 0;
  bool synced = false;  // saw the straddling diff after seeding

  void seed(const SnapshotRec& snap) {
    engine = std::make_unique<Engine>(handler, StpPolicy::None, std::size_t{1} << 16);
    mirror = std::make_unique<BookMirror<Engine>>(*engine);
    mirror->apply_snapshot(snap);
    last_u = snap.last_update_id;
    synced = false;
  }

  // Returns false if the diff was skipped (pre-snapshot) or gapped.
  enum class ApplyResult { Skipped, Applied, Gap };
  ApplyResult apply(const DiffRec& diff) {
    if (diff.last_id <= last_u) return ApplyResult::Skipped;
    ApplyResult result = ApplyResult::Applied;
    if (!synced) {
      // First applied diff must cover last_u + 1.
      if (diff.first_id > last_u + 1) result = ApplyResult::Gap;
      synced = true;
    } else if (diff.first_id != last_u + 1) {
      result = ApplyResult::Gap;
    }
    mirror->apply_diff(diff);
    last_u = diff.last_id;
    return result;
  }
};

// Validation depth: the diff stream only reports levels that *change*, so a
// level that predates the capture and sits outside the seed snapshot's
// range is invisible to the main book until touched. That is a structural
// property of diff-based reconstruction, not drift — so validation compares
// the top N levels per side, a region diffs always keep current (N=400 is
// ~$4 of BTC book each side).
constexpr std::size_t kCompareDepth = 400;

std::vector<std::pair<Price, Qty>> collect_levels(const Engine& engine, Side side, Price lo,
                                                  Price hi) {
  std::vector<std::pair<Price, Qty>> out;
  const PriceLevel* lvl = engine.book().side(side).best();
  while (lvl != nullptr && lvl->price >= lo && lvl->price <= hi &&
         out.size() < kCompareDepth) {
    out.emplace_back(lvl->price, lvl->total_qty);
    lvl = lvl->worse;
  }
  return out;
}

struct Validation {
  std::uint64_t snapshot_id = 0;
  std::size_t levels_compared = 0;
  std::size_t mismatches = 0;
  [[nodiscard]] bool passed() const { return mismatches == 0 && levels_compared > 0; }
};

Validation compare_books(const MirroredBook& main, const MirroredBook& shadow,
                         const SnapshotRec& snap) {
  Validation v;
  v.snapshot_id = snap.last_update_id;
  Price bid_lo = snap.bids.empty() ? 0 : snap.bids.front().price;
  Price ask_hi = snap.asks.empty() ? 0 : snap.asks.front().price;
  for (const LevelUpdate& lu : snap.bids) bid_lo = std::min(bid_lo, lu.price);
  for (const LevelUpdate& lu : snap.asks) ask_hi = std::max(ask_hi, lu.price);

  for (const Side side : {Side::Bid, Side::Ask}) {
    const Price lo = side == Side::Bid ? bid_lo : 0;
    const Price hi = side == Side::Bid ? std::numeric_limits<Price>::max() : ask_hi;
    const auto main_lvls = collect_levels(*main.engine, side, lo, hi);
    const auto shadow_lvls = collect_levels(*shadow.engine, side, lo, hi);
    const std::size_t n = std::max(main_lvls.size(), shadow_lvls.size());
    v.levels_compared += n;
    for (std::size_t i = 0; i < n; ++i) {
      if (i >= main_lvls.size() || i >= shadow_lvls.size() ||
          main_lvls[i] != shadow_lvls[i]) {
        ++v.mismatches;
      }
    }
  }
  return v;
}

int run(const std::string& path, int settle_diffs) {
  using clock = std::chrono::steady_clock;

  // ---- pass 1: parse ------------------------------------------------------
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 1;
  }
  std::vector<Record> records;
  std::size_t parse_errors = 0;
  std::string line;
  const auto t_parse0 = clock::now();
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (auto rec = parse_line(line)) {
      records.push_back(std::move(*rec));
    } else {
      ++parse_errors;
    }
  }
  const double parse_s = std::chrono::duration<double>(clock::now() - t_parse0).count();

  std::size_t n_snap = 0;
  std::size_t n_diff = 0;
  std::size_t n_trade = 0;
  for (const Record& r : records) {
    n_snap += r.kind == Record::Kind::Snapshot;
    n_diff += r.kind == Record::Kind::Diff;
    n_trade += r.kind == Record::Kind::Trade;
  }
  std::printf("parsed %zu records in %.2fs (%zu snapshots, %zu diffs, %zu trades, %zu bad lines)\n",
              records.size(), parse_s, n_snap, n_diff, n_trade, parse_errors);
  if (n_snap == 0 || n_diff == 0) {
    std::fprintf(stderr, "capture must contain at least one snapshot and one diff\n");
    return 1;
  }

  // ---- pass 2: replay + validate ------------------------------------------
  MirroredBook main_book;
  std::unique_ptr<MirroredBook> shadow;
  SnapshotRec shadow_snap;
  int shadow_settle = 0;
  // REST snapshots are fetched concurrently with the stream, so a snapshot
  // record can land in the file *after* diffs that postdate its
  // lastUpdateId. Keep a rolling window of recent diffs and replay it into
  // each fresh shadow; apply() drops the u <= lastUpdateId prefix itself.
  std::deque<const DiffRec*> recent_diffs;

  std::size_t applied = 0;
  std::size_t skipped = 0;
  std::size_t gaps = 0;
  std::vector<Validation> validations;
  double apply_s = 0.0;

  for (const Record& rec : records) {
    switch (rec.kind) {
      case Record::Kind::Snapshot: {
        if (!main_book.engine) {
          main_book.seed(rec.snapshot);
          break;
        }
        if (shadow) {
          // Next snapshot arrived before settling finished: validate now.
          validations.push_back(compare_books(main_book, *shadow, shadow_snap));
        }
        shadow = std::make_unique<MirroredBook>();
        shadow->seed(rec.snapshot);
        for (const DiffRec* d : recent_diffs) shadow->apply(*d);
        shadow_snap = rec.snapshot;
        shadow_settle = settle_diffs;
        break;
      }
      case Record::Kind::Diff: {
        recent_diffs.push_back(&rec.diff);
        if (recent_diffs.size() > 300) recent_diffs.pop_front();
        if (!main_book.engine) break;  // pre-first-snapshot diffs: nothing to do yet
        const auto t0 = clock::now();
        const auto result = main_book.apply(rec.diff);
        apply_s += std::chrono::duration<double>(clock::now() - t0).count();
        applied += result != MirroredBook::ApplyResult::Skipped;
        skipped += result == MirroredBook::ApplyResult::Skipped;
        gaps += result == MirroredBook::ApplyResult::Gap;
        if (shadow) {
          if (shadow->apply(rec.diff) != MirroredBook::ApplyResult::Skipped &&
              --shadow_settle <= 0) {
            validations.push_back(compare_books(main_book, *shadow, shadow_snap));
            shadow.reset();
          }
        }
        break;
      }
      case Record::Kind::Trade:
        break;  // consumed by the market-making simulator, not the mirror
    }
  }
  if (shadow) {
    validations.push_back(compare_books(main_book, *shadow, shadow_snap));
    shadow.reset();
  }

  // ---- report --------------------------------------------------------------
  const std::uint64_t level_updates = main_book.mirror ? main_book.mirror->levels_applied() : 0;
  std::printf("\nreplay: %zu diffs applied (%zu pre-snapshot skipped, %zu sequence gaps)\n",
              applied, skipped, gaps);
  std::printf("mirror: %llu level updates -> engine in %.3fs (%.2fM updates/s)\n",
              static_cast<unsigned long long>(level_updates), apply_s,
              apply_s > 0 ? static_cast<double>(level_updates) / apply_s / 1e6 : 0.0);
  std::printf("phantom matches while mirroring (crossed input anomalies): %llu\n",
              static_cast<unsigned long long>(main_book.handler.trades));

  const PriceLevel* bb = main_book.engine->best_bid();
  const PriceLevel* ba = main_book.engine->best_ask();
  if (bb && ba) {
    std::printf("final book: bid %.2f / ask %.2f, %zu live levels tracked\n",
                static_cast<double>(bb->price) / 100.0, static_cast<double>(ba->price) / 100.0,
                main_book.engine->open_orders());
  }

  std::printf("\nvalidation against %zu exchange snapshots:\n", validations.size());
  bool all_pass = true;
  for (const Validation& v : validations) {
    std::printf("  snapshot %llu: %zu levels compared, %zu mismatches -> %s\n",
                static_cast<unsigned long long>(v.snapshot_id), v.levels_compared, v.mismatches,
                v.passed() ? "PASS" : "FAIL");
    all_pass = all_pass && v.passed();
  }
  if (validations.empty()) {
    std::printf("  (capture contains no post-seed snapshots to validate against)\n");
    return 0;
  }
  std::printf("%s\n", all_pass ? "\nBOOK RECONSTRUCTION VALIDATED" : "\nVALIDATION FAILED");
  return all_pass ? 0 : 2;
}

}  // namespace
}  // namespace nanolob::replay

int main(int argc, char** argv) {
  std::string path = "data/btcusdt_capture.jsonl";
  int settle = 200;  // ~20s of 100ms diffs
  if (argc > 1) path = argv[1];
  if (argc > 2) settle = std::atoi(argv[2]);
  return nanolob::replay::run(path, settle);
}
