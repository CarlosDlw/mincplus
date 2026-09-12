// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "syntax/builder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "parse/parser.h"
#include "parse/token_source.h"

namespace minc::syntax {
namespace {

// Consumes the event stream and produces the green tree.
//
// Two rules carry the whole design:
//
//   * The parser is trivia-blind. When it says "token", the builder flushes the
//     trivia that precedes the next significant token into the node currently
//     open, then emits it. So the grammar has no whitespace rules and the tree
//     still holds every byte.
//   * A node may be adopted by a later node. `forwardParent` says where that
//     parent is, and the builder enters the parent nodes *before* the node when
//     it reaches it, so a left-associative chain nests correctly without moving
//     anything.
class TreeBuilder {
public:
  TreeBuilder(support::Arena& arena, const lex::TokenStream& stream)
      : stream_(&stream), cache_(arena) {}

  [[nodiscard]] std::optional<BuildResult> build(std::vector<parse::Event>& events) {
    std::vector<parse::SyntaxKind> parents;
    for (std::size_t i = 0; i < events.size(); ++i) {
      parse::Event& event = events[i];
      switch (event.tag) {
      case parse::EventTag::Tombstone:
        break;
      case parse::EventTag::Start:
        handleStart(events, i, event, parents);
        break;
      case parse::EventTag::Finish:
        if (!closeNode()) {
          return std::nullopt;
        }
        break;
      case parse::EventTag::Token:
        if (!emitToken(event.kind, event.missing)) {
          return std::nullopt;
        }
        break;
      }
    }

    if (!stack_.empty() || root_ == nullptr) {
      // A malformed event stream. Refuse rather than hand back a partial tree.
      return std::nullopt;
    }

    BuildResult result;
    result.root = root_;
    result.stats.nodeCount = nodeCount_;
    result.stats.tokenCount = tokenCount_;
    result.stats.maxDepth = maxDepth_;
    result.stats.lossless =
        lossless_ && static_cast<std::size_t>(covered_) == stream_->text().size();
    return result;
  }

private:
  struct Frame {
    parse::SyntaxKind kind;
    std::vector<GreenChild> children;
  };

  void handleStart(std::vector<parse::Event>& events, std::size_t index, parse::Event& event,
                   std::vector<parse::SyntaxKind>& parents) {
    if (event.forwardParent == 0) {
      openNode(event.kind);
      return;
    }

    // Walk the forward-parent chain and open the nodes from the outside in, so
    // the node that was finished first becomes the innermost.
    parents.clear();
    parents.push_back(event.kind);
    std::size_t at = index;
    std::uint32_t forward = event.forwardParent;
    while (forward != 0) {
      at += forward;
      parse::Event& target = events[at];
      parents.push_back(target.kind);
      forward = target.forwardParent;
      // Consumed as a parent: when the loop reaches this index it is a no-op,
      // and the node's own Finish still closes it.
      target = parse::Event{};
    }
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
      openNode(*it);
    }
  }

  void openNode(parse::SyntaxKind kind) {
    stack_.push_back(Frame{kind, {}});
    maxDepth_ = std::max(maxDepth_, static_cast<std::uint32_t>(stack_.size()));
  }

  [[nodiscard]] bool closeNode() {
    if (stack_.empty()) {
      return false;
    }
    Frame frame = std::move(stack_.back());
    stack_.pop_back();

    if (stack_.empty()) {
      // This is the file node. Anything the events never asked for is still
      // source and must be preserved, so it is flushed before the node is
      // finalized. Normally the parser has already emitted every token,
      // including the end-of-file leaf, and this loop does nothing.
      while (raw_ < stream_->size()) {
        if (!appendRawToken(frame.children)) {
          return false;
        }
      }
    }

    const GreenNode* node = cache_.node(frame.kind, frame.children);
    if (node == nullptr) {
      return false;
    }
    ++nodeCount_;
    if (stack_.empty()) {
      root_ = node;
    } else {
      stack_.back().children.push_back(GreenChild::ofNode(node));
    }
    return true;
  }

  [[nodiscard]] bool emitToken(parse::SyntaxKind kind, bool missing) {
    if (stack_.empty()) {
      return false;
    }
    std::vector<GreenChild>& out = stack_.back().children;
    if (!flushTrivia(out)) {
      return false;
    }
    if (!missing) {
      return appendRawToken(out);
    }
    // A token the parser inserted: zero width, and it consumes nothing, so the
    // shape of a node does not depend on what the user has typed yet.
    const GreenToken* inserted = cache_.token(kind, std::string_view{});
    if (inserted == nullptr) {
      return false;
    }
    out.push_back(GreenChild::ofToken(inserted));
    ++tokenCount_;
    return true;
  }

  [[nodiscard]] bool flushTrivia(std::vector<GreenChild>& out) {
    while (raw_ < stream_->size() && stream_->tokens()[raw_].isTrivia()) {
      if (!appendRawToken(out)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool appendRawToken(std::vector<GreenChild>& out) {
    if (raw_ >= stream_->size()) {
      return false;
    }
    const lex::Token& raw = stream_->tokens()[raw_];
    if (raw.offset != covered_) {
      // A gap would mean the tree no longer reproduces the source.
      lossless_ = false;
    }
    covered_ = raw.end();
    const GreenToken* token =
        cache_.token(parse::toSyntaxKind(raw.kind), stream_->text().substr(raw.offset, raw.length));
    if (token == nullptr) {
      return false;
    }
    out.push_back(GreenChild::ofToken(token));
    ++tokenCount_;
    ++raw_;
    return true;
  }

  const lex::TokenStream* stream_;
  GreenCache cache_;
  std::vector<Frame> stack_;
  std::size_t raw_ = 0;
  std::uint32_t covered_ = 0;
  bool lossless_ = true;
  const GreenNode* root_ = nullptr;
  std::uint32_t nodeCount_ = 0;
  std::uint32_t tokenCount_ = 0;
  std::uint32_t maxDepth_ = 0;
};

} // namespace

std::optional<BuildResult> buildGreenTree(support::Arena& arena, std::vector<parse::Event>& events,
                                          const lex::TokenStream& stream) {
  TreeBuilder builder(arena, stream);
  return builder.build(events);
}

std::optional<SyntaxTree> buildSyntaxTree(support::Arena& arena, const lex::TokenStream& stream,
                                          std::uint32_t revision) {
  auto source = parse::makeTokenStreamSource(stream);
  parse::Parser parser(*source);
  parse::ParseOutput output = parser.run();

  std::optional<BuildResult> built = buildGreenTree(arena, output.events, stream);
  if (!built.has_value()) {
    return std::nullopt;
  }

  TreeStats stats = built->stats;
  stats.bailedOut = output.bailedOut;
  return SyntaxTree(built->root, stream.text(), stream.file(), revision, std::move(output.errors),
                    stats);
}

} // namespace minc::syntax
