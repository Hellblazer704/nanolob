#include <catch2/catch_test_macros.hpp>
#include <set>
#include <vector>

#include "nanolob/pool.hpp"

namespace {

struct Tracked {
  static inline int live_count = 0;
  int value = 0;
  Tracked() { ++live_count; }
  explicit Tracked(int v) : value(v) { ++live_count; }
  ~Tracked() { --live_count; }
};

}  // namespace

TEST_CASE("pool allocates distinct objects and recycles freed storage") {
  nanolob::Pool<Tracked> pool(/*slab_objects=*/8);

  std::vector<Tracked*> objs;
  std::set<Tracked*> addresses;
  for (int i = 0; i < 20; ++i) {  // spans multiple slabs
    Tracked* t = pool.alloc(i);
    REQUIRE(t->value == i);
    objs.push_back(t);
    addresses.insert(t);
  }
  CHECK(addresses.size() == 20);
  CHECK(pool.live() == 20);
  CHECK(pool.capacity() >= 20);
  CHECK(Tracked::live_count == 20);

  Tracked* recycled_addr = objs.back();
  pool.dealloc(objs.back());
  objs.pop_back();
  CHECK(pool.live() == 19);
  CHECK(Tracked::live_count == 19);

  // LIFO free list: the most recently freed slot is handed out next.
  Tracked* again = pool.alloc(99);
  CHECK(again == recycled_addr);
  CHECK(again->value == 99);

  pool.dealloc(again);
  for (Tracked* t : objs) pool.dealloc(t);
  CHECK(pool.live() == 0);
  CHECK(Tracked::live_count == 0);
}

TEST_CASE("pool reserve pre-grows capacity") {
  nanolob::Pool<Tracked> pool(/*slab_objects=*/16);
  pool.reserve(100);
  CHECK(pool.capacity() >= 100);
  const std::size_t cap = pool.capacity();

  std::vector<Tracked*> objs;
  objs.reserve(100);
  for (int i = 0; i < 100; ++i) objs.push_back(pool.alloc());
  CHECK(pool.capacity() == cap);  // no growth needed
  for (Tracked* t : objs) pool.dealloc(t);
}

TEST_CASE("pool hands out storage aligned for over-aligned types") {
  struct alignas(64) Wide {
    char data[64];
  };
  nanolob::Pool<Wide> pool(4);
  std::vector<Wide*> objs;
  for (int i = 0; i < 10; ++i) {
    Wide* w = pool.alloc();
    CHECK(reinterpret_cast<std::uintptr_t>(w) % 64 == 0);
    objs.push_back(w);
  }
  for (Wide* w : objs) pool.dealloc(w);
}
