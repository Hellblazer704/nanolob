#pragma once

#include <benchmark/benchmark.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "nanolob/types.hpp"

namespace nanolob::bench {

// Per-op latencies are timed with rdtsc: on this codebase's target machines
// std::chrono::steady_clock is QueryPerformanceCounter with ~100ns
// granularity — useless for sub-microsecond ops. The TSC is invariant on
// anything from the last decade. Plain rdtsc (no serializing fence) is used,
// so each sample carries a few nanoseconds of pipeline fuzz plus ~6-10ns of
// measurement overhead; percentiles over millions of samples are unaffected
// in any way that matters at the reported precision.
inline double tsc_ghz() {
  static const double ghz = [] {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = __rdtsc();
    while (clock::now() - t0 < std::chrono::milliseconds(200)) {
    }
    const std::uint64_t c1 = __rdtsc();
    const auto t1 = clock::now();
    return static_cast<double>(c1 - c0) /
           static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }();
  return ghz;
}

inline double cycles_to_ns(double cycles) { return cycles / tsc_ghz(); }

// Destructively computes percentiles (nth_element reorders `samples`).
struct LatencyStats {
  double p50, p90, p99, p999, max;

  static LatencyStats from(std::vector<std::uint32_t>& samples) {
    auto pct = [&](double q) -> double {
      const auto k = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
      std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(k),
                       samples.end());
      return cycles_to_ns(static_cast<double>(samples[k]));
    };
    LatencyStats s{};
    s.p50 = pct(0.50);
    s.p90 = pct(0.90);
    s.p99 = pct(0.99);
    s.p999 = pct(0.999);
    s.max = cycles_to_ns(static_cast<double>(*std::max_element(samples.begin(), samples.end())));
    return s;
  }

  void publish(benchmark::State& state) const {
    state.counters["p50_ns"] = p50;
    state.counters["p90_ns"] = p90;
    state.counters["p99_ns"] = p99;
    state.counters["p99.9_ns"] = p999;
    state.counters["max_ns"] = max;
  }
};

// Minimal event sink: cheap enough not to distort measurements, stateful
// enough that the compiler cannot delete the matching work.
struct CountingHandler {
  std::uint64_t accepts = 0;
  std::uint64_t trades = 0;
  std::uint64_t traded_qty = 0;
  std::uint64_t cancels = 0;
  std::uint64_t rejects = 0;

  void on_accept(OrderId, Side, Price, Qty) { ++accepts; }
  void on_trade(const Trade& t) {
    ++trades;
    traded_qty += t.qty;
  }
  void on_cancel(OrderId, CancelReason) { ++cancels; }
  void on_reject(OrderId, RejectReason) { ++rejects; }
};

}  // namespace nanolob::bench
