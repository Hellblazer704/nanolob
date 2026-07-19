#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "nanolob/flat_hash_map.hpp"

using nanolob::FlatHashMap;

TEST_CASE("flat hash map basic insert/find/erase") {
  FlatHashMap<std::uint64_t, int> map(16);

  CHECK(map.empty());
  CHECK(map.find(42) == nullptr);
  CHECK_FALSE(map.erase(42));

  CHECK(map.insert(42, 1));
  CHECK(map.insert(43, 2));
  CHECK_FALSE(map.insert(42, 99));  // duplicate keys are rejected, not overwritten

  REQUIRE(map.find(42) != nullptr);
  CHECK(*map.find(42) == 1);
  CHECK(map.size() == 2);

  CHECK(map.erase(42));
  CHECK(map.find(42) == nullptr);
  CHECK(map.find(43) != nullptr);
  CHECK(map.size() == 1);
}

TEST_CASE("flat hash map grows past its initial capacity") {
  FlatHashMap<std::uint64_t, std::uint64_t> map(16);
  constexpr std::uint64_t kN = 10000;
  for (std::uint64_t i = 0; i < kN; ++i) REQUIRE(map.insert(i, i * 7));
  CHECK(map.size() == kN);
  for (std::uint64_t i = 0; i < kN; ++i) {
    auto* v = map.find(i);
    REQUIRE(v != nullptr);
    CHECK(*v == i * 7);
  }
}

TEST_CASE("flat hash map for_each visits every live entry") {
  FlatHashMap<std::uint64_t, std::uint64_t> map(16);
  for (std::uint64_t i = 1; i <= 100; ++i) map.insert(i, i);
  for (std::uint64_t i = 1; i <= 100; i += 2) map.erase(i);
  std::uint64_t sum = 0;
  std::size_t count = 0;
  map.for_each([&](std::uint64_t k, std::uint64_t v) {
    CHECK(k == v);
    sum += v;
    ++count;
  });
  CHECK(count == 50);
  CHECK(sum == 2550);  // 2+4+...+100
}

// The interesting failure mode of linear probing + backward-shift deletion is
// probe-chain corruption: an erase that shifts the wrong element leaves some
// key unreachable. Hammer it with churn and cross-check std::unordered_map.
TEST_CASE("flat hash map randomized churn matches std::unordered_map") {
  FlatHashMap<std::uint64_t, std::uint64_t> map(16);
  std::unordered_map<std::uint64_t, std::uint64_t> oracle;
  std::mt19937_64 rng(0xC0FFEE);  // NOLINT(bugprone-random-generator-seed): deterministic test
  // Small key space forces heavy insert/erase collisions on the same slots.
  std::uniform_int_distribution<std::uint64_t> key_dist(0, 500);

  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t key = key_dist(rng);
    switch (rng() % 3) {
      case 0: {  // insert
        const bool inserted = map.insert(key, key * 3);
        CHECK(inserted == !oracle.contains(key));
        oracle.emplace(key, key * 3);
        break;
      }
      case 1: {  // erase
        const bool erased = map.erase(key);
        CHECK(erased == (oracle.erase(key) > 0));
        break;
      }
      default: {  // find
        auto* found = map.find(key);
        auto it = oracle.find(key);
        if (it == oracle.end()) {
          CHECK(found == nullptr);
        } else {
          REQUIRE(found != nullptr);
          CHECK(*found == it->second);
        }
      }
    }
    REQUIRE(map.size() == oracle.size());
  }
  // Final full sweep: every oracle key must still be reachable.
  for (const auto& [k, v] : oracle) {
    auto* found = map.find(k);
    REQUIRE(found != nullptr);
    CHECK(*found == v);
  }
}

TEST_CASE("flat hash map reserve avoids rehash during fill") {
  FlatHashMap<std::uint64_t, int> map(16);
  map.reserve(1000);
  const std::size_t cap = map.capacity();
  for (std::uint64_t i = 0; i < 1000; ++i) map.insert(i, 0);
  CHECK(map.capacity() == cap);
}

// Worst case for backward-shift deletion: a tiny key space forces long probe
// chains that constantly wrap the table boundary, so an off-by-one in the
// shift logic corrupts a chain and orphans a key. Cross-checked against
// std::unordered_map, with a full reachability sweep after each erase.
TEST_CASE("flat hash map maximal-collision churn keeps every key reachable") {
  FlatHashMap<std::uint64_t, std::uint64_t> map(16);
  std::unordered_map<std::uint64_t, std::uint64_t> oracle;
  std::mt19937_64 rng(0xD15EA5E);  // NOLINT(bugprone-random-generator-seed): deterministic test
  std::uniform_int_distribution<std::uint64_t> key_dist(0, 48);  // << table size

  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t key = key_dist(rng);
    if (rng() & 1) {
      CHECK(map.insert(key, key ^ 0xABCD) == !oracle.contains(key));
      oracle.emplace(key, key ^ 0xABCD);
    } else {
      CHECK(map.erase(key) == (oracle.erase(key) > 0));
    }
    REQUIRE(map.size() == oracle.size());
    // Every surviving key must remain reachable through its (now shifted)
    // probe chain — this is what a corrupted backward-shift would break.
    for (const auto& [k, v] : oracle) {
      auto* found = map.find(k);
      REQUIRE(found != nullptr);
      REQUIRE(*found == v);
    }
  }
}

// Fill to just under the load-factor bound, then drain in random order,
// verifying survivors stay reachable as the table empties.
TEST_CASE("flat hash map full random drain keeps survivors reachable") {
  FlatHashMap<std::uint64_t, std::uint64_t> map(16);
  std::vector<std::uint64_t> keys;
  for (std::uint64_t i = 0; i < 20000; ++i) {
    const std::uint64_t k = i * 2654435761ULL;  // Knuth multiplicative spread
    map.insert(k, i);
    keys.push_back(k);
  }
  std::mt19937_64 rng(4242);  // NOLINT(bugprone-random-generator-seed): deterministic test
  std::shuffle(keys.begin(), keys.end(), rng);

  for (std::size_t i = 0; i < keys.size(); ++i) {
    REQUIRE(map.erase(keys[i]));
    for (int s = 0; s < 16 && i + 1 < keys.size(); ++s) {
      const std::size_t j = i + 1 + rng() % (keys.size() - i - 1);
      REQUIRE(map.find(keys[j]) != nullptr);
    }
  }
  CHECK(map.empty());
}
