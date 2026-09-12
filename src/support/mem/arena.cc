// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/mem/arena.h"

#include <cstdint>
#include <limits>

#include "support/limits.h"

namespace minc::support {

void Arena::BlockDeleter::operator()(char* memory) const noexcept {
  ::operator delete(memory, std::align_val_t(kMaxAlignment));
}

Arena::Arena(std::size_t blockSize)
    : blockSize_(blockSize < kMaxAlignment ? kMaxAlignment : blockSize) {}

char* Arena::newBlock(std::size_t bytes) noexcept {
  // A size the allocator cannot possibly satisfy is refused here instead of
  // being handed to operator new. What operator new does with an absurd request
  // is implementation-defined -- with AddressSanitizer's default settings it is
  // a hard abort, not a null return -- and the arena's contract is that it
  // always reports failure by returning nullptr. This is the only call site that
  // grows a block, so one check covers every path into the allocator.
  if (bytes > kMaxArenaAllocation) {
    return nullptr;
  }
  // Aligned, non-throwing: the arena reports failure by returning nullptr.
  return static_cast<char*>(::operator new(bytes, std::align_val_t(kMaxAlignment), std::nothrow));
}

std::size_t Arena::normalizeAlignment(std::size_t align) {
  // Round up to a power of two. Values beyond kMaxAlignment come back larger
  // than the limit so allocate() rejects them instead of under-aligning.
  std::size_t power = 1;
  while (power < align) {
    if (power > kMaxAlignment) {
      return power;
    }
    power <<= 1;
  }
  return power;
}

std::size_t Arena::alignUp(std::size_t value, std::size_t align) {
  const std::size_t mask = align - 1;
  if (value > std::numeric_limits<std::size_t>::max() - mask) {
    return std::numeric_limits<std::size_t>::max(); // saturate; caller sees no room
  }
  return (value + mask) & ~mask;
}

void* Arena::allocate(std::size_t size, std::size_t align) {
  if (size == 0) {
    size = 1;
  }
  align = normalizeAlignment(align);
  if (align > kMaxAlignment) {
    return nullptr;
  }

  // First fit at or after the active block. After rewind() the earlier blocks
  // have room again, so scanning forward reuses them instead of growing.
  for (std::size_t i = active_; i < blocks_.size(); ++i) {
    Block& block = blocks_[i];
    const std::size_t offset = alignUp(block.used, align);
    if (offset <= block.size && size <= block.size - offset) {
      char* pointer = block.data.get() + offset;
      block.used = offset + size;
      active_ = i;
      bytesUsed_ += size;
      return pointer;
    }
  }

  // No block fits: add one. A fresh block starts at offset 0, which is already
  // aligned to kMaxAlignment.
  const std::size_t want = blockSize_ > size ? blockSize_ : size;
  Block fresh;
  fresh.data.reset(newBlock(want));
  if (fresh.data == nullptr) {
    return nullptr;
  }
  fresh.size = want;
  fresh.used = size;
  blocks_.push_back(std::move(fresh));
  active_ = blocks_.size() - 1;
  bytesUsed_ += size;
  capacity_ += want;
  return blocks_.back().data.get();
}

void Arena::rewind() {
  for (Block& block : blocks_) {
    block.used = 0;
  }
  active_ = 0;
  bytesUsed_ = 0;
}

void Arena::release() {
  blocks_.clear();
  active_ = 0;
  bytesUsed_ = 0;
  capacity_ = 0;
}

} // namespace minc::support
