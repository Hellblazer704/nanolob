#pragma once

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace nanolob {

// Fixed-stride object pool. Storage is grabbed from the system allocator in
// large slabs; freed objects are recycled through an embedded free list built
// inside the dead objects' own storage. Memory is only returned to the OS on
// destruction.
//
// Hot-path property: once reserve() has been called with the expected peak
// live-object count, alloc()/dealloc() are O(1) pointer swaps and never touch
// the system allocator.
template <typename T>
class Pool {
 public:
  explicit Pool(std::size_t slab_objects = 4096) : slab_objects_(slab_objects) {}

  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(Pool&&) = delete;

  ~Pool() {
    for (std::byte* slab : slabs_) {
      ::operator delete[](slab, std::align_val_t{alignof(T)});
    }
  }

  // Pre-grow so the first `n` live objects never trigger a slab allocation.
  void reserve(std::size_t n) {
    while (capacity_ < n) add_slab();
  }

  template <typename... Args>
  [[nodiscard]] T* alloc(Args&&... args) {
    if (free_head_ == nullptr) add_slab();
    FreeNode* node = free_head_;
    free_head_ = node->next;
    ++live_;
    return ::new (static_cast<void*>(node)) T(std::forward<Args>(args)...);
  }

  void dealloc(T* obj) {
    obj->~T();
    auto* node = reinterpret_cast<FreeNode*>(obj);
    node->next = free_head_;
    free_head_ = node;
    --live_;
  }

  [[nodiscard]] std::size_t live() const noexcept { return live_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  struct FreeNode {
    FreeNode* next;
  };

  static constexpr std::size_t stride() noexcept {
    return sizeof(T) < sizeof(FreeNode) ? sizeof(FreeNode) : sizeof(T);
  }

  void add_slab() {
    auto* raw = static_cast<std::byte*>(
        ::operator new[](stride() * slab_objects_, std::align_val_t{alignof(T)}));
    slabs_.push_back(raw);
    // Thread the slab onto the free list back-to-front so objects are handed
    // out in ascending address order (sequential prefetcher-friendly).
    for (std::size_t i = slab_objects_; i-- > 0;) {
      auto* node = reinterpret_cast<FreeNode*>(raw + i * stride());
      node->next = free_head_;
      free_head_ = node;
    }
    capacity_ += slab_objects_;
  }

  std::size_t slab_objects_;
  std::size_t capacity_ = 0;
  std::size_t live_ = 0;
  FreeNode* free_head_ = nullptr;
  std::vector<std::byte*> slabs_;
};

}  // namespace nanolob
