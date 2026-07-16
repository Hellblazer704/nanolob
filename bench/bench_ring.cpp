// SPSC ring benchmarks: raw transfer rate, and the end-to-end configuration
// the ring exists for — a market-data thread feeding the matching engine.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "nanolob/engine.hpp"
#include "nanolob/spsc_ring.hpp"
#include "workload.hpp"

namespace nanolob::bench {
namespace {

constexpr std::size_t kRingSize = 4096;

// Raw ring bandwidth: one producer spinning tokens at one consumer.
void bm_ring_raw(benchmark::State& state) {
  constexpr std::uint64_t kItems = 20'000'000;
  for (auto _ : state) {
    auto ring = std::make_unique<SpscRing<std::uint64_t, kRingSize>>();
    std::thread producer([&] {
      for (std::uint64_t i = 0; i < kItems; ++i) {
        while (!ring->try_push(i)) {
        }
      }
    });
    std::uint64_t sum = 0;
    std::uint64_t popped = 0;
    while (popped < kItems) {
      std::uint64_t v = 0;
      if (ring->try_pop(v)) {
        sum += v;
        ++popped;
      }
    }
    producer.join();
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(kItems));
}
BENCHMARK(bm_ring_raw)->Iterations(3)->Unit(benchmark::kMillisecond)->UseRealTime();

// End-to-end: feed thread pushes FeedMsgs through the ring, engine thread
// (the benchmark thread) pops and matches. This is the deployment shape:
// the engine core never parses, never allocates, never blocks.
void bm_ring_to_engine(benchmark::State& state) {
  static const Workload mixed = make_mixed(2'000'000);

  for (auto _ : state) {
    CountingHandler handler;
    MatchingEngine<CountingHandler> engine(handler, StpPolicy::None, std::size_t{1} << 17);
    for (const FeedMsg& m : mixed.prefill) apply(engine, m);

    auto ring = std::make_unique<SpscRing<FeedMsg, kRingSize>>();
    std::thread feed([&] {
      for (const FeedMsg& m : mixed.ops) {
        while (!ring->try_push(m)) {
        }
      }
    });
    std::size_t consumed = 0;
    FeedMsg m{};
    while (consumed < mixed.ops.size()) {
      if (ring->try_pop(m)) {
        apply(engine, m);
        ++consumed;
      }
    }
    feed.join();
    benchmark::DoNotOptimize(handler.trades + handler.cancels);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(mixed.ops.size()));
}
BENCHMARK(bm_ring_to_engine)->Iterations(3)->Unit(benchmark::kMillisecond)->UseRealTime();

}  // namespace
}  // namespace nanolob::bench
