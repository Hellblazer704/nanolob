#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nanolob {

// Open-addressing, linear-probing hash map for trivially copyable key/value
// pairs (order-id -> Order*, price -> PriceLevel*).
//
// Design notes:
//  - Power-of-two capacity, mask instead of modulo.
//  - Linear probing: probe sequence is one contiguous cache stream, which
//    beats chained maps (std::unordered_map is a pointer chase per node).
//  - Backward-shift deletion instead of tombstones: an order book is
//    cancel-heavy, and tombstones would otherwise accumulate and stretch
//    probe distances until a rehash.
//  - Max load factor 0.5 keeps expected probe length ~1.5 slots.
//  - Rehash allocates; call reserve() up front to keep the hot path
//    allocation-free.
template <typename K, typename V>
class FlatHashMap {
  static_assert(sizeof(K) <= 8, "keys are hashed as 64-bit integers");

 public:
  explicit FlatHashMap(std::size_t initial_capacity = 1024) {
    std::size_t cap = 16;
    while (cap < initial_capacity) cap <<= 1;
    slots_.resize(cap);
    used_.assign(cap, 0);
  }

  void reserve(std::size_t n) {
    if (n * 2 > slots_.size()) rehash(next_pow2(n * 2));
  }

  // Returns false if the key already exists (no overwrite).
  bool insert(K key, V value) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    std::size_t idx = ideal(key);
    while (used_[idx]) {
      if (slots_[idx].key == key) return false;
      idx = (idx + 1) & mask();
    }
    slots_[idx] = Slot{key, value};
    used_[idx] = 1;
    ++size_;
    return true;
  }

  // Pointer to the mapped value, or nullptr if absent. Stable only until the
  // next insert/erase.
  [[nodiscard]] V* find(K key) noexcept {
    std::size_t idx = ideal(key);
    while (used_[idx]) {
      if (slots_[idx].key == key) return &slots_[idx].value;
      idx = (idx + 1) & mask();
    }
    return nullptr;
  }

  [[nodiscard]] const V* find(K key) const noexcept {
    return const_cast<FlatHashMap*>(this)->find(key);
  }

  bool erase(K key) noexcept {
    std::size_t idx = ideal(key);
    while (true) {
      if (!used_[idx]) return false;
      if (slots_[idx].key == key) break;
      idx = (idx + 1) & mask();
    }
    --size_;
    // Backward-shift: pull later probe-chain members into the hole so no
    // tombstone is needed.
    std::size_t hole = idx;
    std::size_t probe = idx;
    while (true) {
      probe = (probe + 1) & mask();
      if (!used_[probe]) break;
      const std::size_t home = ideal(slots_[probe].key);
      // Move `probe` into the hole unless its home lies strictly after the
      // hole in the (cyclic) probe order, in which case it is unreachable
      // from the hole and must stay.
      if (((probe - home) & mask()) >= ((probe - hole) & mask())) {
        slots_[hole] = slots_[probe];
        hole = probe;
      }
    }
    used_[hole] = 0;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (used_[i]) fn(slots_[i].key, slots_[i].value);
    }
  }

 private:
  struct Slot {
    K key;
    V value;
  };

  [[nodiscard]] std::size_t mask() const noexcept { return slots_.size() - 1; }

  // splitmix64 finalizer: cheap, and disperses the low bits that the mask
  // keeps (sequential order ids and clustered prices are the common case).
  [[nodiscard]] static std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }

  [[nodiscard]] std::size_t ideal(K key) const noexcept {
    return static_cast<std::size_t>(mix(static_cast<std::uint64_t>(key))) & mask();
  }

  [[nodiscard]] static std::size_t next_pow2(std::size_t n) noexcept {
    std::size_t cap = 16;
    while (cap < n) cap <<= 1;
    return cap;
  }

  void rehash(std::size_t new_cap) {
    std::vector<Slot> old_slots = std::move(slots_);
    std::vector<std::uint8_t> old_used = std::move(used_);
    slots_.assign(new_cap, Slot{});
    used_.assign(new_cap, 0);
    size_ = 0;
    for (std::size_t i = 0; i < old_slots.size(); ++i) {
      if (old_used[i]) insert(old_slots[i].key, old_slots[i].value);
    }
  }

  std::vector<Slot> slots_;
  std::vector<std::uint8_t> used_;
  std::size_t size_ = 0;
};

}  // namespace nanolob
