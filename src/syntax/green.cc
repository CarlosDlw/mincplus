// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/green.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace minc::syntax {

parse::SyntaxKind GreenChild::kind() const {
  return isNode ? node->kind : token->kind;
}

std::uint32_t GreenChild::width() const {
  return isNode ? node->width : token->width();
}

std::size_t GreenCache::TokenKeyHash::operator()(const TokenKey& key) const {
  return std::hash<std::string_view>{}(key.text) ^
         (static_cast<std::size_t>(key.kind) * 0x9E3779B9U);
}

std::size_t GreenCache::NodeKeyHash::operator()(const NodeKey& key) const {
  std::size_t hash = static_cast<std::size_t>(key.kind) * 0x9E3779B9U;
  for (const void* child : key.children) {
    hash ^= std::hash<const void*>{}(child) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
  }
  return hash;
}

const GreenToken* GreenCache::token(parse::SyntaxKind kind, std::string_view text) {
  const TokenKey key{static_cast<std::uint16_t>(kind), text};
  const auto found = tokens_.find(key);
  if (found != tokens_.end()) {
    return found->second;
  }

  void* storage = arena_->allocate(sizeof(GreenToken), alignof(GreenToken));
  if (storage == nullptr) {
    return nullptr;
  }
  GreenToken* created = ::new (storage) GreenToken{kind, text};
  tokens_.emplace(key, created);
  return created;
}

const GreenNode* GreenCache::node(parse::SyntaxKind kind, std::span<const GreenChild> children) {
  // Key on the child *pointers*: the cache hands out one canonical pointer per
  // distinct subtree, so pointer equality is structural equality, and a shared
  // subtree is recognised without comparing it.
  NodeKey key;
  key.kind = static_cast<std::uint16_t>(kind);
  key.children.reserve(children.size());
  std::uint32_t width = 0;
  for (const GreenChild& child : children) {
    key.children.push_back(child.isNode ? static_cast<const void*>(child.node)
                                        : static_cast<const void*>(child.token));
    width += child.width();
  }

  const auto found = nodes_.find(key);
  if (found != nodes_.end()) {
    return found->second;
  }

  std::span<const GreenChild> stored;
  if (!children.empty()) {
    void* array = arena_->allocate(sizeof(GreenChild) * children.size(), alignof(GreenChild));
    if (array == nullptr) {
      return nullptr;
    }
    auto* items = static_cast<GreenChild*>(array);
    for (std::size_t i = 0; i < children.size(); ++i) {
      items[i] = children[i];
    }
    stored = std::span<const GreenChild>(items, children.size());
  }

  void* storage = arena_->allocate(sizeof(GreenNode), alignof(GreenNode));
  if (storage == nullptr) {
    return nullptr;
  }
  GreenNode* created = ::new (storage) GreenNode{kind, width, stored};
  nodes_.emplace(std::move(key), created);
  return created;
}

void GreenCache::clear() {
  tokens_.clear();
  nodes_.clear();
}

} // namespace minc::syntax
