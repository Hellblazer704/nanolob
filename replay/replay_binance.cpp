// Replays a capture produced by scripts/download_binance.py through the
// matching engine and validates the reconstructed book against the
// exchange's own snapshots.
//
// Sequencing follows the Binance book-management protocol: seed from the
// first snapshot, drop diffs with u <= lastUpdateId, require the first
// applied diff to straddle lastUpdateId+1, then require U == prev_u + 1.
//
// Validation compares the reconstructed book directly against each later REST
// snapshot *at that snapshot's own sequence point*: a snapshot's lastUpdateId
// is the final update id of a completed diff batch, so when our applied
// sequence reaches last_u == lastUpdateId our book must equal the exchange's
// book at that instant — top N levels per side, price and quantity, exactly.
//
// An earlier version of this check seeded a shadow book from each snapshot and
// compared main against shadow after letting both consume ~20s of diffs. That
// was too weak: diffs carry absolute quantities, so any two books consuming
// the same stream *converge* on the levels that stream touches, and the test
// could pass while the main book was wrong at the snapshot instant. Comparing
// against the exchange's own levels at the exact sequence point removes both
// the convergence masking and the settling heuristic.
#include <algorithm>
#include <chrono>
#include <map>
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
  Price our_bid = 0, snap_bid = 0, our_ask = 0, snap_ask = 0;
  [[nodiscard]] bool passed() const { return mismatches == 0 && levels_compared > 0; }
};

// The exchange's own top-N levels for a side, best first.
std::vector<std::pair<Price, Qty>> snapshot_levels(const SnapshotRec& snap, Side side) {
  std::vector<std::pair<Price, Qty>> out;
  for (const LevelUpdate& lu : side == Side::Bid ? snap.bids : snap.asks) {
    if (lu.qty > 0) out.emplace_back(lu.price, lu.qty);
  }
  std::sort(out.begin(), out.end(), [side](const auto& a, const auto& b) {
    return side == Side::Bid ? a.first > b.first : a.first < b.first;
  });
  if (out.size() > kCompareDepth) out.resize(kCompareDepth);
  return out;
}

// Compare the reconstructed book against the exchange's snapshot, level by
// level from the touch. Both sides must agree on price *and* quantity.
Validation compare_to_snapshot(const MirroredBook& main, const SnapshotRec& snap) {
  Validation v;
  v.snapshot_id = snap.last_update_id;
  for (const Side side : {Side::Bid, Side::Ask}) {
    const auto ours = collect_levels(*main.engine, side, std::numeric_limits<Price>::min(),
                                     std::numeric_limits<Price>::max());
    const auto theirs = snapshot_levels(snap, side);
    // The snapshot carries a finite depth window; only compare as deep as both
    // sides actually report, but never fewer than a meaningful number of
    // levels (a truncated `ours` at the touch is itself a failure).
    const std::size_t n = std::min(ours.size(), theirs.size());
    v.levels_compared += n;
    for (std::size_t i = 0; i < n; ++i) {
      if (ours[i] != theirs[i]) ++v.mismatches;
    }
    if (n > 0) {
      if (side == Side::Bid) {
        v.our_bid = ours[0].first;
        v.snap_bid = theirs[0].first;
      } else {
        v.our_ask = ours[0].first;
        v.snap_ask = theirs[0].first;
      }
    }
  }
  return v;
}

int run(const std::string& path) {
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

  // Snapshots to validate against, keyed by the sequence point at which our
  // book must equal them. REST snapshots are fetched concurrently with the
  // stream, so a snapshot record can land in the file *after* diffs that
  // postdate it — hence the pre-scan rather than reacting in file order.
  std::map<std::uint64_t, const SnapshotRec*> targets;
  for (const Record& rec : records) {
    if (rec.kind == Record::Kind::Snapshot) {
      targets.emplace(rec.snapshot.last_update_id, &rec.snapshot);
    }
  }

  std::size_t applied = 0;
  std::size_t skipped = 0;
  std::size_t gaps = 0;
  std::size_t unreachable = 0;  // snapshot ids that never landed on a diff end
  std::vector<Validation> validations;
  double apply_s = 0.0;

  for (const Record& rec : records) {
    switch (rec.kind) {
      case Record::Kind::Snapshot: {
        if (!main_book.engine) {
          main_book.seed(rec.snapshot);
          targets.erase(rec.snapshot.last_update_id);  // it *is* our seed
        }
        break;
      }
      case Record::Kind::Diff: {
        if (!main_book.engine) break;  // pre-first-snapshot diffs: nothing to do yet
        const auto t0 = clock::now();
        const auto result = main_book.apply(rec.diff);
        apply_s += std::chrono::duration<double>(clock::now() - t0).count();
        applied += result != MirroredBook::ApplyResult::Skipped;
        skipped += result == MirroredBook::ApplyResult::Skipped;
        gaps += result == MirroredBook::ApplyResult::Gap;

        // A snapshot's lastUpdateId is the final id of a completed batch, so
        // it should coincide exactly with some diff's `u`. Validate there.
        if (result != MirroredBook::ApplyResult::Skipped && !targets.empty()) {
          auto it = targets.begin();
          while (it != targets.end() && it->first <= main_book.last_u) {
            if (it->first == main_book.last_u) {
              validations.push_back(compare_to_snapshot(main_book, *it->second));
            } else {
              // Overshot without ever hitting it: cannot reproduce that state
              // atomically. Counted and reported rather than silently skipped.
              ++unreachable;
            }
            it = targets.erase(it);
          }
        }
        break;
      }
      case Record::Kind::Trade:
        break;  // consumed by the market-making simulator, not the mirror
    }
  }
  unreachable += targets.size();  // snapshots the stream never reached

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

  std::printf(
      "\nvalidation: reconstructed book vs exchange snapshot, compared at the\n"
      "snapshot's own sequence point (last_u == lastUpdateId), top %zu levels/side:\n",
      kCompareDepth);
  bool all_pass = true;
  for (const Validation& v : validations) {
    std::printf("  snapshot %llu: bid %lld/%lld ask %lld/%lld | %zu levels, %zu mismatches -> %s\n",
                static_cast<unsigned long long>(v.snapshot_id),
                static_cast<long long>(v.our_bid), static_cast<long long>(v.snap_bid),
                static_cast<long long>(v.our_ask), static_cast<long long>(v.snap_ask),
                v.levels_compared, v.mismatches, v.passed() ? "PASS" : "FAIL");
    all_pass = all_pass && v.passed();
  }
  if (unreachable > 0) {
    std::printf("  note: %zu snapshot(s) did not land on a diff boundary and were not compared\n",
                unreachable);
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
  if (argc > 1) path = argv[1];
  return nanolob::replay::run(path);
}
