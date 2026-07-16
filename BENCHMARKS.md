# Benchmarks

The point of this file is the **optimization journey** — each design point is
kept alive in the benchmark suite so every number below can be regenerated,
not just the final one.

## Environment

| | |
|---|---|
| CPU | Intel Core Ultra 5 225H (14C/14T, up to ~4.9 GHz boost) |
| RAM | 16 GiB |
| OS | Windows 11 Home |
| Compiler | GCC 16.1 (MinGW-w64 UCRT), `-O3 -DNDEBUG`, C++20 |
| Harness | Google Benchmark v1.9.1 |

Caveats, stated up front: this is a laptop — no core pinning, no isolcpus, no
fixed frequency, stock power profile. Numbers move a few percent run to run;
the *ratios* between design points are stable. Latencies are per-op `rdtsc`
deltas (TSC calibrated against `steady_clock`; `std::chrono` on Windows has
~100 ns granularity, useless at this scale). Each sample carries ~6–10 ns of
measurement overhead which is **not** subtracted; treat small absolute
numbers as slightly pessimistic. `items_per_second` on `latency` rows
includes that instrumentation; the `throughput` rows are uninstrumented and
are the honest throughput numbers.

Workloads (see `bench/workload.hpp`, fully deterministic):

- **add_order / cancel**: book prefilled with 50k orders across 200 price
  levels per side; the stream adds one non-crossing order and cancels the
  oldest, holding depth steady. Isolates the pure hot path — no matching.
- **mixed**: 40% passive add, 5% aggressive IOC, 5% market, 40% cancel,
  10% modify against the same prefilled book. Matching, level creation and
  teardown, and priority-keeping modifies all exercised.

Reproduce with:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bench/nanolob_bench
./build/bench/nanolob_bench_unaligned --benchmark_filter="nanolob/"
```

## The journey

### Design point 0 — `baseline`: the textbook book

`std::map<Price, std::deque<Order>>` per side, `std::unordered_map` for
order-id lookup, node allocations everywhere. This is a *competent* naive
implementation (it is also the differential-test oracle — it must be
correct), not a strawman.

| op | p50 | p90 | p99 | p99.9 | throughput |
|---|---|---|---|---|---|
| add_order | 75 ns | 109 ns | 233 ns | 564 ns | 6.3 M/s |
| cancel | 201 ns | 314 ns | 599 ns | 1.49 µs | 6.2 M/s |
| mixed | 111 ns | 227 ns | 455 ns | 1.03 µs | 9.1 M/s |

Where the time goes: every add is a red-black-tree walk plus at least one
heap allocation; every cancel is a tree walk plus a **linear scan of the
price level** to find the order. Allocation and pointer-chasing dominate;
the tail is allocator noise.

### Design point 1 — `nanolob_stdmap`: intrusive lists + pools + level list

Replace the tree and the deques with the final book structure: orders are
intrusive nodes in per-level FIFO lists, levels are pool-allocated nodes in a
best-to-worst linked list plus a price→level flat hash map, and both orders
and levels come from slab pools (zero heap traffic after warmup). Order-id
lookup stays `std::unordered_map` so its cost is isolated from the next step.

| op | p50 | p90 | p99 | p99.9 | throughput | vs baseline |
|---|---|---|---|---|---|---|
| add_order | 44 ns | 53 ns | 157 ns | 308 ns | — | p50 −41% |
| cancel | 41 ns | 48 ns | 85 ns | 263 ns | — | p50 −80% |
| mixed | 48 ns | 74 ns | 168 ns | 601 ns | 25.3 M/s | 2.8× |

Cancel improves 5× at p50: it is now hash-lookup → unlink → pool-free, with
no scan and no tree. The p99.9 collapse (1.49 µs → 263 ns) is the pools —
the system allocator no longer sits on the hot path taking locks and
occasionally going long.

### Design point 2 — `nanolob`: custom flat hash map for id lookup

Swap `std::unordered_map` for an open-addressing, linear-probing map with
backward-shift deletion (`include/nanolob/flat_hash_map.hpp`), sized to load
factor ≤ 0.5.

| op | p50 | p90 | p99 | p99.9 | throughput | vs point 1 |
|---|---|---|---|---|---|---|
| add_order | 33 ns | 56 ns | 164 ns | 356 ns | — | p50 −26% |
| cancel | 30 ns | 61 ns | 190 ns | 385 ns | — | p50 −27% |
| mixed | 26 ns | 54 ns | 161 ns | 437 ns | **34.8 M/s** | +37% |

`std::unordered_map` is a chained table: every lookup is bucket → node
pointer chase (a dependent cache miss), and every insert/erase allocates or
frees a node. The flat map probes a contiguous array (the next slot is
usually on the same cache line) and never allocates after `reserve()`.
Backward-shift deletion matters specifically because order flow is
cancel-heavy: tombstones would stretch probe chains all day.

**Cumulative: p50 add-order 75 → 33 ns, mixed throughput 9.1 → 34.8 M ops/s
(3.8×), p99.9 down 2–5× across the board. The <1 µs p50 target is met with
~30× margin — p99.9 is under 0.5 µs.**

### Design point 3 — cache-line alignment of book nodes: measured, kept for a different reason

`Order` and `PriceLevel` are padded to exactly 64 bytes (`alignas(64)`).
`nanolob_bench_unaligned` compiles the identical engine with the padding
stripped (56-byte nodes):

| op | aligned p50 / p99.9 | unaligned p50 / p99.9 |
|---|---|---|
| add_order | 33 ns / 356 ns | 32 ns / 279 ns |
| mixed throughput | 34.8 M/s | 33.3 M/s |

**Honest result: a wash.** Single-threaded, the padding buys nothing
measurable — denser packing even wins back some cache footprint. The
alignment is kept because (a) it guarantees no node ever straddles two lines,
so a fill touches exactly one line per order, and (b) pool-neighbouring nodes
can never false-share once book state is read from another thread (strategy
thread in phase 4). That is a *concurrency* insurance policy, not a
single-thread speedup, and the benchmark exists so nobody has to take the
folklore on faith.

### Branch prediction notes

No `[[likely]]` sprinkling — the structure itself keeps branches predictable:

- The passive-add fast path hits `crosses() == false` on its first check;
  in rest-heavy flow that branch is taken essentially never, so it predicts
  essentially always. Aggressive orders pay the matching loop, which is real
  work, not misprediction.
- The bid/ask direction branch inside `BookSide::better()` is data-dependent
  and ~50/50 across a stream — but each *call site* resolves against the same
  side object repeatedly during a walk, and the walk itself is short (new
  levels are born near the touch; the insert loop from best runs ~1–3 hops).
- Hash probing at load factor 0.5 means the "slot occupied, keep probing"
  branch is not-taken ~85% of the time (expected probe length ≈ 1.5); the
  probe loop body has no unpredictable data-dependent branches at all.
- The engine is templated on the event handler, so `if (has_listener)`-style
  runtime branches don't exist — dispatch is inlined and dead events cost
  nothing.

## SPSC ring (feed thread → engine thread)

`include/nanolob/spsc_ring.hpp`: power-of-two capacity, acquire/release
publication, head and tail on separate cache lines, and each side keeps a
cached copy of the other side's index so steady-state push/pop touches no
shared cache line until the cached view goes stale (Rigtorp-style).

| benchmark | rate |
|---|---|
| raw ring, `uint64_t` tokens, 2 threads | 64.6 M items/s |
| ring + full engine consuming mixed flow | 16.3 M msgs/s |

End-to-end, the pipeline sustains ~16 M messages/s — the engine, not the
ring, is the bottleneck (as it should be: the ring's job is to never be the
story). At Binance-replay rates (thousands of msgs/s, phase 3) the ingest
path is ~4 orders of magnitude of headroom.
