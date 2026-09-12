// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Global limits. Centralizes "what if it grows" decisions so lexer, parser,
// and IR code never re-derive them and never disagree.
#pragma once

#include <cstddef>
#include <cstdint>

namespace minc::support {

// Single source files larger than this are rejected with a diagnostic.
// 64 MiB is far above any realistic .mx file and keeps uint32 offsets safe.
inline constexpr std::size_t kMaxSourceBytes = std::size_t{64} * 1024u * 1024u;

// Human-readable rendering of kMaxSourceBytes for diagnostics. The assert
// below keeps this and the numeric limit from drifting apart.
inline constexpr const char* kMaxSourceBytesText = "64 MiB";

// Largest byte offset representable by Span/LineTable (they store uint32).
inline constexpr std::uint32_t kMaxOffset = 0xFFFFFFFFu;

// Guard for the uint32 offset arithmetic used throughout: a source at the
// size limit must still leave room for "end of file" and "offset + 1".
static_assert(kMaxSourceBytes < static_cast<std::size_t>(kMaxOffset),
              "source must stay strictly below the uint32 offset ceiling");

// Maximum files per compilation unit. Guards FileId exhaustion.
inline constexpr std::size_t kMaxSourceFiles = 1u << 20;

// Maximum interned symbols. SymId is uint32 and kInvalidSym is reserved, so
// the last usable id is 0xFFFFFFFE.
inline constexpr std::size_t kMaxSymbols = 0xFFFFFFFFu;

// Longest line rendered in diagnostics before truncation (display columns).
inline constexpr std::size_t kMaxRenderLineCols = 240;

// Maximum diagnostics kept per bag; prevents OOM on cascading errors.
inline constexpr std::size_t kMaxDiagnostics = 1024;

} // namespace minc::support
