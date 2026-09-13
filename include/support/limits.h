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

// How many guarded parser entries may be live at once. This counts *frames*,
// not constructs: one nested parenthesis is four (expression, assignment,
// conditional, unary), so it is about 250 levels of nesting -- the same order
// as Clang's `-fbracket-depth` default of 256.
//
// It is a stack-safety limit, not a language limit, and it is sized for the
// worst case rather than the best: the deepest input must survive a sanitizer
// build on Windows' 1 MiB thread stack, not just a release build on Linux.
// Raising it trades a diagnostic for a crash, which is why it is not larger.
inline constexpr std::uint32_t kMaxNestingDepth = 1024;

// After this many syntax errors the parser stops descending and consumes the
// rest of the input into a single error node, so pathological input costs
// bounded work instead of a quadratic error cascade. Same role as
// `kMaxDiagnostics`, one stage earlier.
inline constexpr std::size_t kMaxParseErrors = 4096;

// Maximum diagnostics kept per bag; prevents OOM on cascading errors.
inline constexpr std::size_t kMaxDiagnostics = 1024;

// --- the preprocessor -------------------------------------------------------
//
// Everything below bounds a *hazard*, not a feature. The C standard's own
// minima (5.2.4.1) are floors a conforming implementation must exceed, and they
// are the reason these numbers are far larger than real code needs: 15 nested
// includes, 63 nested conditionals, 127 macro parameters and arguments.
//
// They can be raised, never disabled. A "no limits" switch would turn the
// compiler into a denial-of-service tool: macro expansion can be exponential
// under the standard's own rules (`#define A B B` / `#define B C C` doubles per
// level) and blue paint only blocks direct self-reference.

// Nested `#include` depth. Well past the standard's 15, and shallow enough that
// the include stack, the records, and the diagnostic chain stay readable.
inline constexpr std::size_t kMaxIncludeDepth = 200;

// Nested conditional-inclusion depth. The standard requires 63.
inline constexpr std::size_t kMaxConditionalNesting = 256;

// Macros live on the expansion stack at once (`#define X Y` / `#define Y X`).
inline constexpr std::size_t kMaxExpansionDepth = 256;

// Tokens a single translation unit may produce by expansion. The hard stop for
// an exponential bomb, checked as the token is appended so the array cannot
// grow past it.
inline constexpr std::size_t kMaxExpandedTokens = std::size_t{8} << 20;

// Bytes the preprocessed output may occupy. Same order as a source file, for
// the same reason: past this the parser is being handed something no human
// wrote, and the failure should be a diagnostic rather than an OOM kill.
inline constexpr std::size_t kMaxPreprocessedBytes = kMaxSourceBytes;

// Longest spelling a token may have after `#` or `##`. The standard's floor for
// a logical source line is 4095 characters; a paste that keeps doubling is
// quadratic work, so the result is capped where a line would be.
inline constexpr std::size_t kMaxTokenBytes = 4095;

// Parameters (including `...`) in one macro definition. Standard floor: 127.
inline constexpr std::size_t kMaxMacroParameters = 256;

// Files a single translation unit may include. Bounds the "include the same
// unguarded file a million times" case, which the guard optimization can only
// help with when the file has a guard.
inline constexpr std::size_t kMaxIncludesPerUnit = 65536;

// Macro expansion depth as seen by a *diagnostic*: how much of the "in
// expansion of macro 'X'" chain is rendered before it is elided. Separate from
// kMaxExpansionDepth because it is a presentation choice, not a safety one.
inline constexpr std::size_t kMaxMacroBacktrace = 8;

// --- lowering and name resolution -------------------------------------------
//
// Same rule as the preprocessor's bounds: every one of these bounds a *hazard*
// (untrusted input deciding how much memory a stage allocates), it is always on,
// and it is checked when the entry is created -- before the allocation -- so
// hitting it costs a diagnostic and not a kill.

// Nodes one translation unit may lower to. Above the largest realistic file by
// orders of magnitude: a 64 MiB source that is nothing but `a + b + c` lines
// stays under a million nodes.
inline constexpr std::size_t kMaxAstNodesPerUnit = std::size_t{8} << 20;

// Declarations (functions, parameters, variables, constants) one unit may have.
// Also the size of every scope table, which is why it is not larger.
inline constexpr std::size_t kMaxDefsPerUnit = std::size_t{1} << 20;

// Scopes one unit may open: the file, one per function, one per block.
inline constexpr std::size_t kMaxScopesPerUnit = std::size_t{1} << 20;

// How far the scope-chain walk will follow before answering NotFound. Bounds
// the *lookup* rather than the *tree*: the scope parents are a tree, so a long
// chain means deep nesting, and a lookup that walked forever would be a hang
// rather than a diagnostic. The parser already bounds the nesting this can
// mirror, so hitting it means something built the scope chain outside the
// parser.
inline constexpr std::size_t kMaxScopeDepth = kMaxNestingDepth + 16;

// Name uses one unit may contain. Four times the def bound because a use is
// cheaper than a declaration and an expression-heavy file has many.
inline constexpr std::size_t kMaxNameRefsPerUnit = std::size_t{4} << 20;

// Names the typo search will score for one failing lookup. This one is not
// about memory: it is the cost an editor pays on every keystroke, so it is small
// enough to be invisible and large enough to find the real candidate.
inline constexpr std::size_t kMaxSuggestionCandidates = 64;

// Longest edit distance that still counts as a suggestion. Two edits covers a
// transposition, a doubled letter and a missing letter; three would start
// suggesting names the reader did not mean.
inline constexpr std::uint32_t kMaxSuggestionDistance = 2;

// Largest single block the Arena will ask the allocator for. A runaway size --
// a SIZE_MAX from bad arithmetic, a corrupted length field -- must never reach
// operator new: what it does with an absurd request is implementation-defined
// (AddressSanitizer aborts rather than returning null), and Arena promises to
// report failure by returning nullptr on every platform. 2 GiB is far above any
// legitimate single AST/IR allocation while staying clear of that edge.
inline constexpr std::size_t kMaxArenaAllocation = std::size_t{1} << 31;

} // namespace minc::support
