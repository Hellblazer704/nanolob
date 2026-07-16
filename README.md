# nanolob

[![CI](https://github.com/Hellblazer704/nanolob/actions/workflows/ci.yml/badge.svg)](https://github.com/Hellblazer704/nanolob/actions/workflows/ci.yml)

A low-latency limit order book and matching engine in C++20, with a lock-free
ingest pipeline, real market-data replay validated against exchange snapshots,
and an Avellaneda–Stoikov market-making simulator with queue-position fill
modeling on top.

**Headline number: 33 ns median add-order latency** (p99.9 = 356 ns), 34.8M
ops/s sustained on mixed order flow — measured on a stock laptop, with the full
optimization journey documented in [BENCHMARKS.md](BENCHMARKS.md).

```
Binance L2 feed ──> SPSC ring ──> matching engine ──> fills ──> A-S strategy
 (depth diffs,      (lock-free,    (price-time         │            │
  trades,            16M msgs/s    priority,           ▼            ▼
  snapshots)         end-to-end)   33ns adds)      PnL decomp   quotes back
                                                   + markouts   into the book
```

## Design

The engine core (`include/nanolob/`) is header-only with **zero dependencies
and zero heap allocations on the hot path**:

- **Orders** are intrusive nodes in per-price-level FIFO lists — no separate
  node allocations, O(1) cancel by pointer unlink. Each order and each price
  level is exactly one cache line.
- **Price levels** live in a best-to-worst intrusive list (matching and BBO are
  pointer walks) plus a **custom flat hash map** price→level for O(1) arrival
  at existing levels. Order-id lookup is the same open-addressing map: linear
  probing with backward-shift deletion, because order flow is cancel-heavy and
  tombstones would stretch probe chains all day.
- **Pool allocators** hand out orders and levels from pre-reserved slabs
  through an embedded free list.
- **Events are a template parameter** — handler dispatch inlines away; there
  are no virtual calls anywhere on the hot path.
- The **SPSC ring** (feed thread → engine thread) uses acquire/release
  publication, cache-line-separated counters, and cached counterpart indices
  so steady-state push/pop touches no shared line.

Semantics: limit GTC/IOC, market, cancel, modify (quantity reduction keeps
queue priority; reprice/increase is cancel-replace), and configurable
self-trade prevention (cancel-resting / cancel-incoming / off).

## Performance

Intel Core Ultra 5 225H (laptop, no tuning), GCC 16.1 `-O3`. Percentiles are
per-op rdtsc samples; throughput rows are uninstrumented. Full methodology,
workloads, and the step-by-step optimization journey: [BENCHMARKS.md](BENCHMARKS.md).

| | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| add order (no match) | 33 ns | 164 ns | 356 ns | — |
| cancel | 30 ns | 190 ns | 385 ns | — |
| mixed flow (adds/cancels/markets/modifies) | 26 ns | 161 ns | 437 ns | **34.8 M ops/s** |
| textbook baseline (std::map + unordered_map), mixed | 111 ns | 455 ns | 1.03 µs | 9.1 M/s |
| SPSC ring, raw | | | | 64.6 M items/s |
| ring → engine end-to-end | | | | 16.3 M msgs/s |

Every design point (baseline, +intrusive/pools, +flat hash map, ±cache-line
padding) stays alive in the benchmark suite, so each claimed delta is
reproducible — including the honest finding that node alignment is a
single-threaded wash and is kept purely for cross-thread reasons.

## Correctness

- **Randomized differential testing**: 20k-op random streams (all order types,
  modifies, STP) are replayed into both the engine and a deliberately naive
  `std::map` reference oracle; event sequences and book state must match
  *exactly*, across seeds and STP policies.
- 52 Catch2 test cases (~683k assertions) covering priority, partial fills,
  IOC/market remainders, modify semantics, STP, id reuse, hash-map churn
  against `std::unordered_map`, and cross-thread ring transfer.
- **Replay validation against the exchange**: the reconstructed BTCUSDT book is
  checked against later REST snapshots using a shadow-book technique (diff
  quantities are absolute, hence idempotent — see `replay/replay_binance.cpp`).
  Live captures: 0 sequence gaps, 0 phantom matches, 13/13 snapshot
  validations exact.
- CI: gcc + clang builds, full suite under ASan/UBSan, clang-tidy
  (`bugprone-*`/`clang-analyzer-*` as errors), and a ≥85% line-coverage gate
  on the engine headers (currently 100%).

## Market-making simulation

`nanolob_mmsim` replays a capture and quotes with either **Avellaneda–Stoikov**
(rolling horizon; reservation price skewed by inventory, spread from
γσ²τ + (1/γ)ln(1+γ/k), live EWMA variance) or a **naive fixed spread** with
identical size/caps/clamping, so only the policy differs.

Fills are simulated with **queue-position modeling**: quotes join the back of
the level FIFO; exchange trades at the quote price consume the estimated queue
ahead before filling; trades through the price and book crossings fill in
full; unexplained level shrinkage prorates as cancellations. PnL decomposes
exactly into spread capture vs inventory (adverse-selection) PnL.

The [analysis notebook](analysis/adverse_selection.ipynb) quantifies adverse
selection on a live capture: markout curves by horizon, and the
fill-conditional forward-move gap (conditional on being filled, the mid moves
systematically against the position — on the captured selloff session, by
hundreds of ticks over 5s). Spread capture is pennies; inventory is dollars.
The quoting policy's real job is inventory control.

## Build & run

Requires CMake ≥3.23, a C++20 compiler, and Ninja (tests fetch Catch2, benches
fetch Google Benchmark automatically).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# benchmarks (design points + ring)
./build/bench/nanolob_bench
./build/bench/nanolob_bench_unaligned --benchmark_filter="nanolob/"

# capture ~10 minutes of live BTCUSDT L2 + trades (needs: pip install websockets)
python scripts/download_binance.py --minutes 10 --out data/btcusdt.jsonl

# replay + validate the book against exchange snapshots
./build/replay/nanolob_replay data/btcusdt.jsonl

# market-making simulation, both strategies, then plots
./build/strategy/nanolob_mmsim --capture data/btcusdt.jsonl --strategy as    --out-prefix results/as
./build/strategy/nanolob_mmsim --capture data/btcusdt.jsonl --strategy naive --out-prefix results/naive
python scripts/plot_mm.py     # needs: pip install matplotlib pandas
```

## Limitations & next steps

Stated plainly, because they matter:

- Single-threaded matching core (by design — the ring is the concurrency
  boundary); no persistence or recovery.
- Fill simulation assumptions: prorated cancel positions, no latency between
  signal and quote, no fees/rebates (which dwarf 1–2 tick edges in practice),
  no self-impact on the book.
- A-S parameters (γ, k, τ) are set to sensible demo scales, not calibrated;
  k could be estimated from the captured trade stream.
- Results shown are from single sessions of one symbol. The framework makes
  multi-session, multi-regime sweeps cheap — that's the obvious next step,
  along with a fee model and a γ/τ frontier sweep.
