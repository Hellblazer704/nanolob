// Engine benchmarks across the design points documented in BENCHMARKS.md:
//   baseline:      std::map levels + std::deque FIFOs + std::unordered_map ids
//                  (the tests' reference oracle — a competent "textbook" LOB)
//   nanolob_stdmap: final book structures, but std::unordered_map id lookup
//   nanolob:       shipped configuration (flat hash map, pools, intrusive lists)
// A separate binary compiled with NANOLOB_NO_CACHE_ALIGN registers the same
// benchmarks under an "unaligned:" prefix to price the cache-line padding.
#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bench_common.hpp"
#include "nanolob/engine.hpp"
#include "nanolob/testing/reference_engine.hpp"
#include "workload.hpp"

#ifndef NANOLOB_BENCH_TAG
#define NANOLOB_BENCH_TAG ""
#endif

namespace nanolob::bench {
namespace {

// std::unordered_map behind the engine's IdMap interface, to isolate the
// contribution of the custom flat hash map.
class StdIdMap {
 public:
  explicit StdIdMap(std::size_t expected) { map_.reserve(expected); }
  bool insert(OrderId k, Order* v) { return map_.emplace(k, v).second; }
  Order** find(OrderId k) {
    auto it = map_.find(k);
    return it == map_.end() ? nullptr : &it->second;
  }
  bool erase(OrderId k) { return map_.erase(k) > 0; }
  [[nodiscard]] std::size_t size() const { return map_.size(); }

 private:
  std::unordered_map<OrderId, Order*> map_;
};

struct NanolobFactory {
  using Engine = MatchingEngine<CountingHandler>;
  static std::unique_ptr<Engine> make(CountingHandler& h) {
    return std::make_unique<Engine>(h, StpPolicy::None, std::size_t{1} << 17);
  }
};

struct NanolobStdMapFactory {
  using Engine = MatchingEngine<CountingHandler, StdIdMap>;
  static std::unique_ptr<Engine> make(CountingHandler& h) {
    return std::make_unique<Engine>(h, StpPolicy::None, std::size_t{1} << 17);
  }
};

struct BaselineFactory {
  using Engine = test::ReferenceEngine<CountingHandler>;
  static std::unique_ptr<Engine> make(CountingHandler& h) {
    return std::make_unique<Engine>(h, StpPolicy::None);
  }
};

template <typename Engine>
void replay(Engine& engine, const std::vector<FeedMsg>& msgs, OrderId id_offset) {
  for (const FeedMsg& msg : msgs) {
    FeedMsg m = msg;
    m.id += id_offset;
    apply(engine, m);
  }
}

template <typename Engine>
void replay_sampled(Engine& engine, const Workload& w, OrderId id_offset,
                    std::vector<std::uint32_t>& samples) {
  for (std::size_t i = 0; i < w.ops.size(); ++i) {
    FeedMsg m = w.ops[i];
    m.id += id_offset;
    if (w.measure[i]) {
      const std::uint64_t t0 = __rdtsc();
      apply(engine, m);
      const std::uint64_t dt = __rdtsc() - t0;
      samples.push_back(static_cast<std::uint32_t>(
          dt > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : dt));
    } else {
      apply(engine, m);
    }
  }
}

// Latency benchmark: replays the workload a fixed number of times, timing
// each measured op with rdtsc, and reports percentile counters.
template <typename Factory>
void bm_latency(benchmark::State& state, const Workload* w) {
  CountingHandler handler;
  auto engine = Factory::make(handler);
  replay(*engine, w->prefill, 0);

  std::vector<std::uint32_t> samples;
  samples.reserve(w->ops.size() * 4);
  OrderId id_offset = 0;
  for (auto _ : state) {
    replay_sampled(*engine, *w, id_offset, samples);
    id_offset += w->ops.size() + (OrderId{1} << 21);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(w->ops.size()));
  benchmark::DoNotOptimize(handler.trades + handler.cancels + handler.accepts);
  LatencyStats::from(samples).publish(state);
}

// Throughput benchmark: same replay, no per-op instrumentation at all.
template <typename Factory>
void bm_throughput(benchmark::State& state, const Workload* w) {
  CountingHandler handler;
  auto engine = Factory::make(handler);
  replay(*engine, w->prefill, 0);

  OrderId id_offset = 0;
  for (auto _ : state) {
    replay(*engine, w->ops, id_offset);
    id_offset += w->ops.size() + (OrderId{1} << 21);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(w->ops.size()));
  benchmark::DoNotOptimize(handler.trades + handler.cancels + handler.accepts);
}

template <typename Factory>
void register_variant(const std::string& variant) {
  static const Workload add_rest = make_rest_heavy(500'000, MeasureKind::Adds);
  static const Workload cancels = make_rest_heavy(500'000, MeasureKind::Cancels);
  static const Workload mixed = make_mixed(500'000);

  const std::string tag = std::string(NANOLOB_BENCH_TAG) + variant;
  benchmark::RegisterBenchmark((tag + "/add_order/latency").c_str(), bm_latency<Factory>,
                               &add_rest)
      ->Iterations(4)
      ->Unit(benchmark::kMillisecond);
  benchmark::RegisterBenchmark((tag + "/cancel/latency").c_str(), bm_latency<Factory>, &cancels)
      ->Iterations(4)
      ->Unit(benchmark::kMillisecond);
  benchmark::RegisterBenchmark((tag + "/mixed/latency").c_str(), bm_latency<Factory>, &mixed)
      ->Iterations(4)
      ->Unit(benchmark::kMillisecond);
  benchmark::RegisterBenchmark((tag + "/mixed/throughput").c_str(), bm_throughput<Factory>,
                               &mixed)
      ->Unit(benchmark::kMillisecond);
}

const int registered = [] {
  register_variant<BaselineFactory>("baseline");
  register_variant<NanolobStdMapFactory>("nanolob_stdmap");
  register_variant<NanolobFactory>("nanolob");
  return 0;
}();

}  // namespace
}  // namespace nanolob::bench
