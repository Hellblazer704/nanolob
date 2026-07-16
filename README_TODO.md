# README outline (write this yourself)

Do not ship this file — replace it with `README.md` in your own words.
Suggested structure, roughly in the order a recruiter/interviewer will read:

## 1. One-paragraph pitch
- What nanolob is: a C++20 limit order book + matching engine with a market-data
  replay harness and a market-making simulation layer on top.
- The one number you want them to remember (e.g. p50 add-order latency from BENCHMARKS.md).

## 2. Architecture sketch
- Diagram or bullet flow: feed (Binance L2 diffs) -> SPSC ring -> matching engine
  -> fills -> strategy (Avellaneda-Stoikov) -> quotes back into the engine.
- Key data structures and *why*: intrusive FIFO lists per price level, flat
  hash maps for order-id and price-level lookup, pool allocators, no heap
  allocations on the hot path.

## 3. Performance
- Table of p50/p99/p99.9 per operation + throughput; link to BENCHMARKS.md for
  the full optimization journey. State the hardware and compiler.

## 4. Correctness story
- Unit tests (Catch2), randomized differential testing against a naive
  reference engine, ASan/UBSan in CI, coverage gate.
- Replay validation: reconstructed book cross-checked against exchange snapshots.

## 5. Market-making results
- One or two plots (PnL decomposition, inventory), one honest paragraph on what
  A-S does better than the naive fixed-spread baseline and where the simulation
  is still unrealistic (latency, queue model assumptions, fees).

## 6. Build & run
- Exact commands: configure/build/test, run benchmarks, download data, replay,
  run the simulation, regenerate plots.

## 7. Limitations / next steps
- Be candid: single-threaded matching, simplified STP semantics, fill model
  assumptions, no fees/rebates yet, etc.
