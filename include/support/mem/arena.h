// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Bump arena for AST/IR nodes.
//
// Contract:
//   * allocate() and create()/createArray() return nullptr instead of
//     throwing, so the arena never introduces exceptions (see README).
//   * Destructors do not run. Only place trivially-destructible values here,
//     or manage the destructor yourself before calling rewind()/release().
//   * Blocks are aligned to kMaxAlignment, so any request whose alignment is
//     a power of two up to that value is honoured exactly. Larger alignments
//     are refused (nullptr) rather than silently mis-aligned.
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace minc::support {

class Arena {
public:
  // Alignment of every block, and the largest alignment a request may ask for.
  static constexpr std::size_t kMaxAlignment = 64;

  explicit Arena(std::size_t blockSize = 4096);

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns nullptr when out of memory, when `align` exceeds kMaxAlignment,
  // or when `size` cannot be satisfied.
  [[nodiscard]] void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t));

  template <typename T, typename... Args> [[nodiscard]] T* create(Args&&... args) {
    void* memory = allocate(sizeof(T), alignof(T));
    if (memory == nullptr) {
      return nullptr;
    }
    return ::new (memory) T(std::forward<Args>(args)...);
  }

  // Value-initialized array of `count` elements, or nullptr on failure.
  template <typename T> [[nodiscard]] T* createArray(std::size_t count) {
    if (count == 0 || count > static_cast<std::size_t>(-1) / sizeof(T)) {
      return nullptr;
    }
    void* memory = allocate(sizeof(T) * count, alignof(T));
    if (memory == nullptr) {
      return nullptr;
    }
    T* items = static_cast<T*>(memory);
    for (std::size_t i = 0; i < count; ++i) {
      ::new (static_cast<void*>(items + i)) T();
    }
    return items;
  }

  // Keep the blocks and reuse them for the next compilation unit. Much
  // cheaper than re-acquiring them, at the cost of holding the peak memory.
  void rewind();

  // Free every block.
  void release();

  [[nodiscard]] std::size_t bytesUsed() const {
    return bytesUsed_;
  }
  [[nodiscard]] std::size_t blockCount() const {
    return blocks_.size();
  }
  [[nodiscard]] std::size_t capacityBytes() const {
    return capacity_;
  }

private:
  // Frees a block through the aligned form of operator delete. Using a
  // unique_ptr means Block stays movable without a hand-written rule of five.
  struct BlockDeleter {
    void operator()(char* memory) const noexcept;
  };
  struct Block {
    std::unique_ptr<char[], BlockDeleter> data;
    std::size_t size = 0;
    std::size_t used = 0;
  };

  [[nodiscard]] static char* newBlock(std::size_t bytes) noexcept;
  [[nodiscard]] static std::size_t alignUp(std::size_t value, std::size_t align);
  [[nodiscard]] static std::size_t normalizeAlignment(std::size_t align);

  std::vector<Block> blocks_;
  std::size_t active_ = 0; // index of the block allocations start from
  std::size_t blockSize_;
  std::size_t bytesUsed_ = 0;
  std::size_t capacity_ = 0;
};

static_assert(Arena::kMaxAlignment >= alignof(std::max_align_t),
              "arena blocks must cover the natural maximum alignment");
static_assert((Arena::kMaxAlignment & (Arena::kMaxAlignment - 1)) == 0,
              "kMaxAlignment must be a power of two");

} // namespace minc::support
