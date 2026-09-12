// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/preprocessor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lex/lexer.h"
#include "pp_internal.h"
#include "support/limits.h"
#include "support/source/source_file.h"

namespace minc::pp {
namespace {

using detail::isHash;
using detail::isSignificant;
using detail::skipTrivia;

// The directive names this stage knows by name. Anything else is either a
// reserved-but-unimplemented directive (named, so the message is useful) or a
// typo. Kept in one place so the skip logic and the dispatch cannot disagree
// about which names are conditionals.
enum class DirectiveId : std::uint8_t {
  Define,
  Undef,
  Include,
  IncludeNext,
  If,
  Ifdef,
  Ifndef,
  Elif,
  Elifdef,
  Elifndef,
  Else,
  Endif,
  Line,
  Error,
  Warning,
  Pragma,
  Null,
  Reserved, // `#embed`, `#assert`, ... : legal C this stage does not accept yet
  Unknown,
};

struct DirectiveName {
  std::string_view name;
  DirectiveId id;
};

constexpr DirectiveId kNoDirective = DirectiveId::Unknown;

// The table. Ordering does not matter; linear search over seventeen entries on
// a directive line is not a cost worth a hash map.
constexpr DirectiveName kDirectiveNames[] = {
    {"define", DirectiveId::Define},
    {"undef", DirectiveId::Undef},
    {"include", DirectiveId::Include},
    {"include_next", DirectiveId::IncludeNext},
    {"if", DirectiveId::If},
    {"ifdef", DirectiveId::Ifdef},
    {"ifndef", DirectiveId::Ifndef},
    {"elif", DirectiveId::Elif},
    {"elifdef", DirectiveId::Elifdef},
    {"elifndef", DirectiveId::Elifndef},
    {"else", DirectiveId::Else},
    {"endif", DirectiveId::Endif},
    {"line", DirectiveId::Line},
    {"error", DirectiveId::Error},
    {"warning", DirectiveId::Warning},
    {"pragma", DirectiveId::Pragma},
    {"embed", DirectiveId::Reserved},
    {"assert", DirectiveId::Reserved},
    {"unassert", DirectiveId::Reserved},
    {"sccs", DirectiveId::Reserved},
    {"ident", DirectiveId::Reserved},
};

[[nodiscard]] bool isConditional(DirectiveId id) {
  switch (id) {
  case DirectiveId::If:
  case DirectiveId::Ifdef:
  case DirectiveId::Ifndef:
  case DirectiveId::Elif:
  case DirectiveId::Elifdef:
  case DirectiveId::Elifndef:
  case DirectiveId::Else:
  case DirectiveId::Endif:
    return true;
  default:
    return false;
  }
}

// The span of a whole directive: `#` through the last token before the newline.
[[nodiscard]] SourceLoc directiveSpan(const std::vector<PPToken>& line) {
  std::size_t last = line.size();
  while (last > 0 && isPPTrivia(line[last - 1].kind)) {
    --last;
  }
  if (last == 0) {
    return line.empty() ? SourceLoc{} : line.front().loc.spelling;
  }
  return detail::cover(std::span<const PPToken>(line.data(), last));
}

} // namespace

Preprocessor::Preprocessor(support::Session& session, PPOptions options)
    : session_(&session), options_(std::move(options)), macros_(session.symbols()),
      resolver_(session, options_.includes) {
  scratch_.emplace_back(); // index 0: the empty spelling, so 0 is never dangling
}

Preprocessor::~Preprocessor() = default;

void Preprocessor::reset() {
  macros_.clear();
  expansions_.clear();
  resolver_.clear();
  conditionals_.clear();
  record_.clear();
  files_.clear();
  contexts_.clear();
  output_.clear();
  outputText_.clear();
  outputOffset_.clear();
  outputLength_.clear();
  outputOrigin_.clear();
  lexed_.clear();
  headerNames_.clear();
  lastOrigin_ = support::Span{};
  wroteSeparator_ = false;
  scratch_.clear();
  scratch_.emplace_back();
  errors_.clear();
  warnings_.clear();
  useCounts_.clear();
  counter_ = 0;
  expandedTokens_ = 0;
  emittedBytes_ = 0;
  lineOffset_ = 0;
  fileNameOverride_.reset();
  budgetReported_ = false;
  selfReferenceReported_ = false;
  aborted_ = false;
  atLineStart_ = true;
  lookaheadFloor_ = 0;
  lookaheadFile_ = true;
  lastEof_ = SourceLoc{};
}

// --- TokenText --------------------------------------------------------------

std::string_view Preprocessor::spelling(const PPToken& token) const {
  if (token.scratch != kNoScratch && token.scratch < scratch_.size()) {
    return scratch_[token.scratch];
  }
  if (!token.loc.spelling.valid()) {
    return {};
  }
  const support::SourceFile* file = session_->sources().find(token.loc.spelling.file);
  if (file == nullptr) {
    return {};
  }
  return file->slice(token.loc.spelling.offset, token.loc.spelling.end());
}

std::uint32_t Preprocessor::addScratch(std::string text) {
  const auto index = static_cast<std::uint32_t>(scratch_.size());
  scratch_.push_back(std::move(text));
  return index;
}

bool Preprocessor::relex(std::string_view text, PPToken& out) {
  if (text.empty() || text.size() > support::kMaxTokenBytes) {
    return false;
  }
  const lex::Token first = lex::lexOne(text, 0);
  // Exactly one token, or the paste is not one preprocessing token. A second
  // token would leave `length != text.size()`, and `#` glued to something is an
  // Invalid one-byte token, so both cases fall out of this test.
  if (first.is(lex::TokenKind::EndOfFile) || first.length != text.size()) {
    return false;
  }
  out = PPToken{};
  out.kind = first.kind;
  out.length = static_cast<std::uint32_t>(text.size());
  out.scratch = addScratch(std::string(text));
  return true;
}

// --- inspection -------------------------------------------------------------

std::uint32_t Preprocessor::useCount(support::SymId name) const {
  const auto it = useCounts_.find(name);
  return it == useCounts_.end() ? 0U : it->second;
}

std::string_view Preprocessor::pathOf(support::FileId file) const {
  const support::SourceFile* source = session_->sources().find(file);
  return source == nullptr ? std::string_view{} : std::string_view(source->path);
}

std::int64_t Preprocessor::currentLine(const SourceLoc& where) const {
  const support::SourceFile* source = session_->sources().find(where.file);
  if (source == nullptr) {
    return 1;
  }
  return static_cast<std::int64_t>(source->lookup(where.offset).line) + lineOffset_;
}

// --- cursor -----------------------------------------------------------------

PPToken Preprocessor::eofToken() const {
  PPToken token;
  token.kind = lex::TokenKind::EndOfFile;
  token.loc.spelling = lastEof_;
  return token;
}

PPToken Preprocessor::toPP(const lex::Token& token, support::FileId file) const {
  PPToken out;
  out.kind = token.kind;
  out.length = token.length;
  out.loc.spelling = SourceLoc{file, token.offset, token.length};
  return out;
}

void Preprocessor::updateAtLineStart(const lex::Token& token) {
  // "At the start of a line" is the state a directive can only begin in. A
  // directive may be indented, so whitespace keeps the flag; a newline sets it;
  // anything else clears it.
  //
  if (token.is(lex::TokenKind::Newline)) {
    atLineStart_ = true;
    return;
  }
  if (token.is(lex::TokenKind::Whitespace) || token.is(lex::TokenKind::LineComment)) {
    return;
  }
  if (token.is(lex::TokenKind::BlockComment)) {
    // A block comment ending on a later line ends that line: the token after it
    // is at the start of a line even though no newline token appeared.
    if (!files_.empty() && files_.back().stream != nullptr) {
      const std::string_view spelling =
          files_.back().stream->text().substr(token.offset, token.length);
      if (spelling.find('\n') != std::string_view::npos) {
        atLineStart_ = true;
      }
    }
    return;
  }
  atLineStart_ = false;
}

PPToken Preprocessor::take() {
  while (true) {
    if (!contexts_.empty()) {
      Context& context = contexts_.back();
      if (context.index < context.tokens.size()) {
        return context.tokens[context.index++];
      }
      // The replacement list is exhausted: pop it. Popping is what re-enables
      // the macro and what closes the recorded output range, which is why this
      // goes through `popContext` rather than touching the vector.
      popContext();
      continue;
    }
    if (files_.empty()) {
      return eofToken();
    }
    FileFrame& frame = files_.back();
    if (frame.index < frame.stream->size()) {
      const lex::Token& raw = (*frame.stream)[frame.index];
      ++frame.index;
      if (raw.is(lex::TokenKind::EndOfFile)) {
        // The lexer tiles a file with an explicit end-of-file token; it means
        // "this file is done", not "the translation unit is done". Popping the
        // frame here is what makes a `#include` resume its includer, so the
        // two must not be confused.
        closeFile();
        continue;
      }
      updateAtLineStart(raw);
      return toPP(raw, frame.id);
    }
    closeFile();
  }
}

void Preprocessor::settle() {
  // A live expansion is not a file boundary: whatever it has left to produce
  // comes first, and the file it came from must stay on the stack until it is
  // drained.
  if (!contexts_.empty()) {
    return;
  }
  while (!files_.empty()) {
    const FileFrame& frame = files_.back();
    const bool exhausted = frame.index >= frame.stream->size() ||
                           (*frame.stream)[frame.index].is(lex::TokenKind::EndOfFile);
    if (!exhausted) {
      return;
    }
    closeFile();
  }
}

PPToken Preprocessor::peekSignificant() const {
  // Walk the top-down over the expansion stack, then the current file. Trivia is
  // skipped because "is the next token a `(`?" is a question about preprocessing
  // tokens. The walk stops at `lookaheadFloor_` so that pre-expanding an
  // argument cannot see tokens outside that argument, which is what the standard
  // requires and what makes `f(g) (2)` come out right.
  for (std::size_t i = contexts_.size(); i-- > lookaheadFloor_;) {
    const Context& context = contexts_[i];
    for (std::size_t j = context.index; j < context.tokens.size(); ++j) {
      if (isSignificant(context.tokens[j])) {
        return context.tokens[j];
      }
    }
  }
  if (!lookaheadFile_ || files_.empty()) {
    return eofToken();
  }
  const FileFrame& frame = files_.back();
  for (std::size_t j = frame.index; j < frame.stream->size(); ++j) {
    const lex::Token& raw = (*frame.stream)[j];
    if (isPPTrivia(raw.kind) || raw.is(lex::TokenKind::EndOfFile)) {
      continue;
    }
    return toPP(raw, frame.id);
  }
  return eofToken();
}

void Preprocessor::pushFile(support::FileId file, std::string path, std::string dir, bool isSystem,
                            const support::FileIdentity& identity) {
  const support::SourceFile* source = session_->sources().find(file);
  if (source == nullptr) {
    return;
  }
  FileFrame frame;
  frame.id = file;
  frame.path = std::move(path);
  frame.dir = std::move(dir);
  // Copied, not moved: the resolver below is asked the same identity questions
  // before the frame is ever consulted, and a moved-from identity would answer
  // them about nothing.
  frame.identity = identity;
  frame.isSystem = isSystem;
  frame.stream =
      std::make_shared<const lex::TokenStream>(lex::TokenStream::lex(file, source->text));
  lexed_.push_back(frame.stream);
  frame.index = 0;
  frame.conditionalDepth = conditionals_.size();
  frame.savedAtLineStart = atLineStart_;
  atLineStart_ = true;
  lastEof_ = SourceLoc{file, source->size(), 0};
  files_.push_back(std::move(frame));

  // A file that opens twice without a guard is diagnosed: the information is
  // already in hand, and it is one of the most common real bugs in C code. Not
  // an error, because a deliberate second inclusion is legal.
  // `openCount` is read before this inclusion is counted, so "seen before" is
  // `>= 1`: the warning belongs on the *second* read, which is the first one that
  // could have been elided.
  if (options_.warnMissingGuard && resolver_.openCount(identity) >= 1 &&
      !resolver_.isOnce(identity) && !resolver_.guardOf(identity).has_value()) {
    pushWarning(PPError{SourceLoc{file, 0, 0}.span(),
                        "file '" + files_.back().path +
                            "' is included more than once and has no include guard or "
                            "#pragma once",
                        PPErrorCode::MissingIncludeGuard});
  }
  resolver_.noteOpened(identity);

  // The multiple-include optimization: a canonical guard is remembered so the
  // *next* inclusion of this file can be elided without opening it.
  if (std::optional<support::SymId> guard =
          sniffIncludeGuard(*files_.back().stream, session_->symbols())) {
    resolver_.noteGuard(identity, *guard);
  }
}

void Preprocessor::closeFile() {
  if (!files_.empty()) {
    const FileFrame& frame = files_.back();
    // The includer resumes where its own line-start state was; the included
    // file's last token has no say in it.
    atLineStart_ = frame.savedAtLineStart;
    lastEof_ = SourceLoc{frame.id, static_cast<std::uint32_t>(frame.stream->size()), 0};
    // An `#endif` cannot close a conditional opened in another file; if the file
    // ends with conditionals still open, that is where they are reported.
    if (conditionals_.size() > frame.conditionalDepth) {
      conditionals_.clear();
      pushError(PPError{lastEof_.span(),
                        "unterminated conditional directive; expected '#endif' before the end of "
                        "the file",
                        PPErrorCode::UnterminatedConditional});
    }
    files_.pop_back();
  }
}

// --- emit -------------------------------------------------------------------

std::size_t Preprocessor::expandedTokenBudget() const {
  return std::min(options_.budgets.expandedTokens, support::kMaxExpandedTokens);
}

std::size_t Preprocessor::preprocessedByteBudget() const {
  return std::min(options_.budgets.preprocessedBytes, support::kMaxPreprocessedBytes);
}

bool Preprocessor::overExpansionBudget(std::size_t produced) {
  if (aborted_) {
    return true;
  }
  if (produced <= expandedTokenBudget()) {
    return false;
  }
  if (!budgetReported_) {
    budgetReported_ = true;
    pushError(PPError{lastEof_.span(),
                      "macro expansion produced more than " +
                          std::to_string(expandedTokenBudget()) +
                          " tokens for this translation unit; expansion stopped",
                      PPErrorCode::ExpansionBudgetExceeded});
  }
  aborted_ = true;
  return true;
}

void Preprocessor::emit(const PPToken& token) {
  if (token.isEndOfFile()) {
    return;
  }
  const std::string_view text = spelling(token);
  const bool trivia = isPPTrivia(token.kind);
  const support::Span origin = token.loc.spelling.span();

  // Checked *before* the buffer grows, so the bound is a bound rather than a
  // report after the fact.
  if (emittedBytes_ + text.size() + 1U > preprocessedByteBudget()) {
    if (!budgetReported_) {
      budgetReported_ = true;
      pushError(PPError{lastEof_.span(),
                        "preprocessed output is larger than " +
                            std::to_string(preprocessedByteBudget()) +
                            " bytes for this translation unit; output stopped",
                        PPErrorCode::PreprocessedBytesExceeded});
    }
    aborted_ = true;
    return;
  }

  // The separator rule, and the reason the output is *text* rather than a token
  // list: two tokens are separated by a space only when the second does not
  // continue the first byte for byte. Trivia written in a file is copied
  // verbatim, so a file with no directives comes out identical to its input and
  // the tree over it is the tree of the file; a token produced by a macro, the
  // first token of an included file, and anything after a dropped directive line
  // get one space each, because nothing was written there to copy.
  if (!trivia && !output_.empty() && !wroteSeparator_ && !continuesOutput(origin)) {
    appendSeparator();
  }

  const std::uint32_t offset = static_cast<std::uint32_t>(outputText_.size());
  outputText_.append(text);
  outputOffset_.push_back(offset);
  outputLength_.push_back(static_cast<std::uint32_t>(text.size()));
  outputOrigin_.push_back(origin);
  output_.push_back(token);
  lastOrigin_ = origin;
  wroteSeparator_ = false;
  emittedBytes_ += text.size() + 1U;
  if (!trivia) {
    ++record_.stats().tokensEmitted;
  }
}

bool Preprocessor::continuesOutput(const support::Span& origin) const {
  if (!lastOrigin_.valid() || !origin.valid()) {
    return false;
  }
  return lastOrigin_.file == origin.file && lastOrigin_.end == origin.begin;
}

void Preprocessor::appendSeparator() {
  // A real token, one byte wide, so the stream tiles the text like any lexed
  // one: a gap would be a token the tree could not hold, and `lossless()` would
  // be false for a reason nothing could fix.
  PPToken space;
  space.kind = lex::TokenKind::Whitespace;
  space.length = 1;
  space.loc.spelling = SourceLoc{};
  const std::uint32_t offset = static_cast<std::uint32_t>(outputText_.size());
  outputText_.push_back(' ');
  outputOffset_.push_back(offset);
  outputLength_.push_back(1);
  outputOrigin_.push_back(support::Span{});
  output_.push_back(space);
  wroteSeparator_ = true;
  emittedBytes_ += 1U;
}

void Preprocessor::pushError(PPError error) {
  // An error raised while a replacement list is being scanned belongs to that
  // expansion, and saying so is the difference between "unknown identifier TYPO"
  // and "unknown identifier TYPO, in expansion of macro 'DECLARE'". The frame is
  // stamped here rather than at every call site so none can forget it.
  if (error.expansion == kNoExpansion && !contexts_.empty()) {
    error.expansion = contexts_.back().expansion;
  }
  errors_.push_back(std::move(error));
}

void Preprocessor::pushWarning(PPError error) {
  if (error.expansion == kNoExpansion && !contexts_.empty()) {
    error.expansion = contexts_.back().expansion;
  }
  warnings_.push_back(std::move(error));
}

PPToken Preprocessor::makeScratchToken(lex::TokenKind kind, std::string text,
                                       const SourceLoc& where, PPTokenFlag flag) {
  PPToken token;
  token.kind = kind;
  token.flags = flagOf(flag);
  token.length = static_cast<std::uint32_t>(text.size());
  token.loc.spelling = where;
  token.scratch = addScratch(std::move(text));
  return token;
}

// --- the run ----------------------------------------------------------------

bool Preprocessor::startsDirective() const {
  if (files_.empty() || !atLineStart_) {
    return false;
  }
  const FileFrame& frame = files_.back();
  if (frame.index >= frame.stream->size()) {
    return false;
  }
  return isHash((*frame.stream)[frame.index]);
}

void Preprocessor::readDirectiveLine(std::vector<PPToken>& line) {
  FileFrame& frame = files_.back();
  const std::string_view text = frame.stream->text();
  while (frame.index < frame.stream->size()) {
    const lex::Token& raw = (*frame.stream)[frame.index];
    if (raw.is(lex::TokenKind::EndOfFile)) {
      // A file that does not end in a newline ends its last directive here.
      break;
    }
    if (raw.is(lex::TokenKind::Newline)) {
      // The newline ends the directive and is not part of it; the file's own
      // stream keeps it, so nothing is lost.
      ++frame.index;
      atLineStart_ = true;
      break;
    }
    ++frame.index;
    updateAtLineStart(raw);
    line.push_back(toPP(raw, frame.id));
  }
  // After the tokens rather than while reading them: a header-name is recognized
  // from the raw bytes, and its span can cover tokens the plain lexer produced
  // under rules that do not apply to it. See `spliceHeaderNames`.
  spliceHeaderNames(line, text);
}

void Preprocessor::spliceHeaderNames(std::vector<PPToken>& line, std::string_view text) {
  if (line.size() < 2 || !isHash(line[0])) {
    return;
  }

  // Where a header-name can begin, and nowhere else: C defines it as the operand
  // of `#include`/`#include_next`, and C23 as an operand of `__has_include`.
  std::vector<std::size_t> candidates;
  std::size_t index = 1;
  const PPToken* name = detail::nextSignificant(line, index);
  if (name == nullptr || !name->is(lex::TokenKind::Identifier)) {
    return;
  }
  const std::string_view directive = spelling(*name);

  if (directive == "include" || directive == "include_next") {
    ++index;
    // `nextSignificant` leaves `index` on the token it returned, so that is the
    // operand's index once it comes back non-null.
    if (detail::nextSignificant(line, index) != nullptr) {
      candidates.push_back(index);
    }
  } else if (directive == "if" || directive == "elif") {
    // `__has_include(<name>)`: the name is one operand of an expression, so it
    // can sit anywhere in the line, not only first.
    for (std::size_t i = 1; i < line.size(); ++i) {
      if (!line[i].is(lex::TokenKind::Identifier) || spelling(line[i]) != "__has_include") {
        continue;
      }
      std::size_t cursor = i + 1;
      const PPToken* open = detail::nextSignificant(line, cursor);
      if (open == nullptr || !open->is(lex::TokenKind::LParen)) {
        continue;
      }
      ++cursor;
      if (detail::nextSignificant(line, cursor) != nullptr) {
        candidates.push_back(cursor);
      }
    }
  } else {
    return;
  }

  // Back to front, so each splice leaves the indices of the ones before it
  // untouched.
  const support::FileId file = line.front().loc.spelling.file;
  for (std::size_t i = candidates.size(); i > 0; --i) {
    const std::size_t at = candidates[i - 1];
    const std::optional<lex::HeaderName> header =
        lex::scanHeaderName(text, line[at].loc.spelling.offset);
    if (header.has_value()) {
      spliceHeaderName(line, at, *header, file);
    }
  }
}

void Preprocessor::spliceHeaderName(std::vector<PPToken>& line, std::size_t index,
                                    const lex::HeaderName& name, support::FileId file) {
  // Every token whose bytes lie inside the name is covered: the opening
  // delimiter, the pieces the plain lexer made of the path, the trivia between
  // them (a space is an h-char, so it belongs to the name), and -- for
  // `#include <a//b.h>` -- the line comment that used to swallow the `>`.
  const std::uint32_t nameEnd = name.offset + name.length;
  std::size_t end = index;
  while (end < line.size() && line[end].loc.spelling.offset < nameEnd) {
    ++end;
  }

  // The last covered token can stick out past the name. To the plain lexer,
  // `__has_include(<a//b.h>)` is one line comment whose opening bytes are the
  // name and whose closing byte is the `)` that ends the operand, and `#include
  // <a/*b.h>` has the same shape. Those trailing bytes are the directive's own
  // syntax, so they are re-lexed rather than disappearing with the comment: the
  // comment's kind is not the reading that applies to any of them.
  std::vector<PPToken> tail;
  if (end > index) {
    const SourceLoc& covered = line[end - 1].loc.spelling;
    const support::SourceFile* source =
        covered.end() > nameEnd ? session_->sources().find(covered.file) : nullptr;
    for (std::uint32_t at = nameEnd; source != nullptr && at < covered.end();) {
      const lex::Token raw = lex::lexOne(source->text, at);
      // Bytes that are not a whole token under these rules are not delivered as
      // a mangled one; nothing else can follow them inside the covered token.
      if (raw.length == 0 || at + raw.length > covered.end()) {
        break;
      }
      tail.push_back(toPP(raw, covered.file));
      at += raw.length;
    }
  }

  PPToken token;
  token.kind = lex::TokenKind::HeaderName;
  token.length = name.length;
  // The spelling is the source bytes, delimiters included: the token is the
  // whole `<...>` or `"..."`, so a caret underlines what the reader sees and
  // the name is read back out of it where it is wanted.
  token.loc.spelling = SourceLoc{file, name.offset, name.length};

  headerNames_.push_back(token.loc.spelling.span());
  line.erase(line.begin() + static_cast<std::ptrdiff_t>(index + 1),
             line.begin() + static_cast<std::ptrdiff_t>(end));
  line[index] = token;
  line.insert(line.begin() + static_cast<std::ptrdiff_t>(index + 1), tail.begin(), tail.end());
}

void Preprocessor::handleDirective() {
  if (files_.empty()) {
    return;
  }
  std::vector<PPToken> line;
  readDirectiveLine(line);
  if (line.empty()) {
    return;
  }

  // `line[0]` is the `#`. Everything else is the directive.
  const SourceLoc directive = directiveSpan(line);
  std::size_t index = 1;
  const PPToken* nameToken = detail::nextSignificant(line, index);
  if (nameToken == nullptr) {
    // `#` alone on a line: the null directive, which the standard defines and
    // which is what a generated file leaves behind.
    if (record_.recording()) {
      record_.addDirective(DirectiveRecord{directive, DirectiveKind::Null, support::kInvalidSym,
                                           conditionals_.emitting(), false});
    }
    return;
  }
  if (!nameToken->is(lex::TokenKind::Identifier)) {
    if (conditionals_.emitting()) {
      pushError(PPError{nameToken->loc.spelling.span(), "expected a directive name after '#'",
                        PPErrorCode::InvalidDirective});
    }
    return;
  }

  const std::string_view name = spelling(*nameToken);
  DirectiveId id = kNoDirective;
  for (const DirectiveName& entry : kDirectiveNames) {
    if (entry.name == name) {
      id = entry.id;
      break;
    }
  }

  const bool emitting = conditionals_.emitting();
  const bool conditional = isConditional(id);
  if (!emitting && !conditional) {
    // Inside a skipped group, directives are processed only as far as tracking
    // nesting requires. Reporting them would produce errors for code the user
    // deliberately excluded.
    if (record_.recording()) {
      record_.addDirective(
          DirectiveRecord{directive, DirectiveKind::Null, support::kInvalidSym, false, false});
    }
    return;
  }

  if (record_.recording()) {
    DirectiveRecord entry;
    entry.span = directive;
    entry.taken = emitting;
    switch (id) {
    case DirectiveId::Define:
      entry.kind = DirectiveKind::Define;
      break;
    case DirectiveId::Undef:
      entry.kind = DirectiveKind::Undef;
      break;
    case DirectiveId::Include:
      entry.kind = DirectiveKind::Include;
      break;
    case DirectiveId::IncludeNext:
      entry.kind = DirectiveKind::IncludeNext;
      break;
    case DirectiveId::Pragma:
      entry.kind = DirectiveKind::Pragma;
      break;
    case DirectiveId::Line:
      entry.kind = DirectiveKind::Line;
      break;
    case DirectiveId::Error:
      entry.kind = DirectiveKind::Error;
      break;
    case DirectiveId::Warning:
      entry.kind = DirectiveKind::Warning;
      break;
    case DirectiveId::Null:
      entry.kind = DirectiveKind::Null;
      break;
    default:
      entry.kind = conditional ? DirectiveKind::Conditional : DirectiveKind::Null;
      break;
    }
    record_.addDirective(entry);
  }

  switch (id) {
  case DirectiveId::Define:
    handleDefine(line, directive);
    break;
  case DirectiveId::Undef:
    handleUndef(line, directive);
    break;
  case DirectiveId::Include:
    handleInclude(line, directive, /*next=*/false);
    break;
  case DirectiveId::IncludeNext:
    handleInclude(line, directive, /*next=*/true);
    break;
  case DirectiveId::If:
  case DirectiveId::Ifdef:
  case DirectiveId::Ifndef:
  case DirectiveId::Elif:
  case DirectiveId::Elifdef:
  case DirectiveId::Elifndef:
  case DirectiveId::Else:
  case DirectiveId::Endif:
    handleConditional(line, directive, name);
    break;
  case DirectiveId::Line:
    handleLine(line, directive);
    break;
  case DirectiveId::Error:
    handleErrorOrWarning(line, directive, /*warning=*/false);
    break;
  case DirectiveId::Warning:
    handleErrorOrWarning(line, directive, /*warning=*/true);
    break;
  case DirectiveId::Pragma:
    handlePragma(line, directive);
    break;
  case DirectiveId::Reserved:
    pushError(PPError{directive.span(),
                      "'#" + std::string(name) + "' is not supported by this compiler",
                      PPErrorCode::NotSupported});
    break;
  default:
    pushError(PPError{directive.span(), "'#" + std::string(name) + "' is not a directive",
                      PPErrorCode::InvalidDirective});
    break;
  }
}

PPResult Preprocessor::run(support::FileId mainFile) {
  reset();

  const support::SourceFile* source = session_->sources().find(mainFile);
  if (source == nullptr) {
    pushError(
        PPError{{}, "internal error: the input file is not loaded", PPErrorCode::InvalidDirective});
    PPResult result;
    result.mainFile = mainFile;
    result.errors = errors_;
    return result;
  }

  installPredefined();
  for (const std::pair<std::string, std::string>& define : options_.defines) {
    defineFromText(define.second.empty() ? define.first : define.first + "=" + define.second);
  }
  for (const std::string& name : options_.undefines) {
    (void)macros_.undef(session_->symbols().intern(name), SourceLoc{});
  }

  pushFile(mainFile, source->path, support::directoryOf(source->path), /*isSystem=*/false,
           support::identifyFile(source->path));

  while (!aborted_ && !atEndOfInput()) {
    settle();
    if (contexts_.empty() && startsDirective()) {
      handleDirective();
      continue;
    }
    if (!conditionals_.emitting()) {
      // Inside a group that is not taken, ordinary tokens are consumed and
      // dropped: not expanded, not emitted, not diagnosed. Only conditionals
      // are still processed, and `handleDirective` above already does that.
      (void)take();
      continue;
    }
    PPToken token = take();
    if (token.isEndOfFile()) {
      break;
    }
    if (!isSignificant(token)) {
      // Trivia is emitted only when it came from a file. Trivia written inside a
      // macro's replacement list or an argument is not part of the preprocessed
      // text: the space between two expanded tokens is the one the emitter
      // inserts, and copying a macro's internal layout would make the output
      // depend on how the macro happened to be formatted.
      if (contexts_.empty()) {
        emit(token);
      }
      continue;
    }
    if (token.is(lex::TokenKind::Identifier) && tryExpand(token, /*emitPath=*/true)) {
      continue;
    }
    if (lex::isPreprocessorOp(token.kind)) {
      // Reaching here means the `#` did not start a directive (something other
      // than whitespace preceded it on its line) or the `##` was not inside a
      // macro's replacement list. The preprocessor is the only stage that can
      // attach a meaning to either, and it has none to attach, so it says so.
      //
      // The token is still emitted: dropping it would break the lossless
      // invariant, and the parser's "unexpected token" on top of this is the
      // honest follow-on, not a duplicate.
      pushError(PPError{token.loc.spelling.span(),
                        token.is(lex::TokenKind::Hash)
                            ? "stray '#' in program; a directive starts with '#' at the "
                              "beginning of a line"
                            : "stray '##' in program; it is only valid inside a macro's "
                              "replacement list",
                        PPErrorCode::StrayHashOperator, token.loc.expansion});
    }
    emit(token);
  }

  PPResult result;
  result.mainFile = mainFile;
  result.tokens = std::move(output_);
  result.text = std::move(outputText_);
  result.origins = std::move(outputOrigin_);
  result.lexed = std::move(lexed_);
  result.headerNames = std::move(headerNames_);
  // The lexer view of the same output. Flags are dropped: they describe the
  // bytes as they were lexed, and these tokens were not lexed here.
  result.stream.reserve(result.tokens.size() + 1U);
  for (std::size_t i = 0; i < result.tokens.size(); ++i) {
    result.stream.push_back(lex::Token{outputOffset_[i], outputLength_[i], result.tokens[i].kind,
                                       /*flags=*/0});
  }
  // The stream ends with an `EndOfFile` token at the end of the text, exactly as
  // a lexed one does. It is load-bearing: the parser emits the end-of-file leaf
  // and the builder writes it from this stream, so a stream without one would
  // make the last leaf unwritable and fail the build of an otherwise valid tree.
  const auto end = static_cast<std::uint32_t>(result.text.size());
  result.stream.push_back(lex::Token{end, 0, lex::TokenKind::EndOfFile, /*flags=*/0});
  result.origins.push_back(support::Span{});
  outputOffset_.clear();
  outputLength_.clear();
  result.errors = errors_;
  result.warnings = warnings_;
  return result;
}

lex::TokenStream preprocessedStream(const PPResult& result) {
  return lex::TokenStream::fromPreprocessed(result.mainFile, result.text, result.stream,
                                            result.origins);
}

} // namespace minc::pp
