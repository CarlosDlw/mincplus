// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// The directive half of the preprocessor: one function per directive, so a
// directive's rules can be read and tested without reading the other thirteen.
// Everything here consumes tokens from the *file* level -- `handleDirective`
// only runs when the expansion stack is empty -- which is why no function in
// this file has to ask whether a token came from a macro.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lex/token_stream.h"
#include "pp_internal.h"
#include "support/limits.h"
#include "support/source/source_file.h"

#include "pp/preprocessor.h"

namespace minc::pp {
namespace {

using detail::isHash;
using detail::isSignificant;
using detail::nextSignificant;
using detail::skipTrivia;

// True when `...` starts at `index`. The lexer produces three `.` tokens: the
// language has no variadic syntax yet, so a token kind for C's macro syntax
// would put a C concept in the language's lexer. Adjacency is checked through
// source offsets, so `. . .` is not an ellipsis.
[[nodiscard]] bool startsEllipsis(const std::vector<PPToken>& tokens, std::size_t index) {
  if (index + 2 >= tokens.size()) {
    return false;
  }
  for (std::size_t i = index; i < index + 3; ++i) {
    if (!tokens[i].is(lex::TokenKind::Dot)) {
      return false;
    }
    if (i > index && tokens[i].loc.spelling.offset != tokens[i - 1].loc.spelling.end()) {
      return false;
    }
  }
  return true;
}

// The index of the parameter a body token names, or `kNotAParameter`.
[[nodiscard]] std::uint8_t parameterIndex(const std::string_view text, const MacroInfo& macro,
                                          const support::Interner& symbols) {
  for (std::size_t i = 0; i < macro.params.size(); ++i) {
    if (symbols.lookup(macro.params[i].name) == text) {
      return static_cast<std::uint8_t>(i);
    }
  }
  if (macro.variadic && text == "__VA_ARGS__") {
    return static_cast<std::uint8_t>(macro.params.size());
  }
  return kNotAParameter;
}

} // namespace

std::string Preprocessor::rawTextOf(const std::vector<PPToken>& line, std::size_t from) const {
  // The message of `#error`/`#warning` is the text of the line as written, so
  // this reads the source instead of joining token spellings: the user's own
  // spacing is part of the message.
  std::size_t begin = from;
  skipTrivia(line, begin);
  std::size_t end = line.size();
  while (end > begin && isPPTrivia(line[end - 1].kind)) {
    --end;
  }
  if (begin >= end) {
    return {};
  }
  const SourceLoc& first = line[begin].loc.spelling;
  const SourceLoc& last = line[end - 1].loc.spelling;
  if (first.file != last.file || last.end() < first.offset) {
    return {};
  }
  const support::SourceFile* source = session_->sources().find(first.file);
  if (source == nullptr) {
    return {};
  }
  return std::string(source->slice(first.offset, last.end()));
}

bool Preprocessor::buildMacroInfo(const std::vector<PPToken>& definition, const SourceLoc& define,
                                  const SourceLoc& defineName, MacroInfo& out) {
  std::size_t index = 0;
  skipTrivia(definition, index);
  if (index >= definition.size() || !definition[index].is(lex::TokenKind::Identifier)) {
    pushError(PPError{defineName.valid() ? defineName.span() : define.span(),
                      "expected a macro name", PPErrorCode::MissingMacroName});
    return false;
  }

  out.name = session_->symbols().intern(spelling(definition[index]));
  out.define = define;
  out.defineName = definition[index].loc.spelling.valid() ? definition[index].loc.spelling : define;
  const std::size_t nameIndex = index;
  ++index;

  // A function-like definition has `(` *immediately* after the name: whitespace
  // in between makes it object-like with a parenthesized body. That is the
  // single most surprising rule in the directive, and the reason trivia is
  // consulted here at all.
  //
  // Adjacency is "same origin and touching", not "both spans valid". A `-D`
  // definition is synthesized and has no file to point at, but its offsets are
  // still into the text it was built from -- requiring a real file here is what
  // silently turned `-D 'F(x)=body'` into an object-like macro.
  const bool functionLike =
      index < definition.size() && definition[index].is(lex::TokenKind::LParen) &&
      definition[index].loc.spelling.file == definition[nameIndex].loc.spelling.file &&
      definition[index].loc.spelling.offset == definition[nameIndex].loc.spelling.end();
  if (functionLike) {
    out.kind = MacroKind::FunctionLike;
    ++index; // consume '('
    if (!buildParameterList(definition, index, out)) {
      return false;
    }
  } else {
    out.kind = MacroKind::ObjectLike;
  }
  return buildReplacementList(definition, index, out);
}

bool Preprocessor::buildParameterList(const std::vector<PPToken>& definition, std::size_t& index,
                                      MacroInfo& out) {
  const SourceLoc location = out.defineName.valid() ? out.defineName : out.define;
  const auto fail = [&](std::string message) {
    pushError(PPError{location.span(), std::move(message), PPErrorCode::InvalidDirective});
    return false;
  };

  // `()` is the empty list and `(void)` is one parameter named `void`, exactly
  // as in C: there is no special case for "no parameters".
  skipTrivia(definition, index);
  if (index < definition.size() && definition[index].is(lex::TokenKind::RParen)) {
    ++index;
    return true;
  }

  while (true) {
    skipTrivia(definition, index);
    if (startsEllipsis(definition, index)) {
      out.variadic = true;
      index += 3;
      break;
    }
    if (index >= definition.size() || !definition[index].is(lex::TokenKind::Identifier)) {
      return fail("expected a parameter name in this macro's parameter list");
    }
    MacroParam param;
    param.name = session_->symbols().intern(spelling(definition[index]));
    for (const MacroParam& existing : out.params) {
      if (existing.name == param.name) {
        return fail("duplicate parameter '" + std::string(spelling(definition[index])) +
                    "' in this macro definition");
      }
    }
    out.params.push_back(param);
    ++index;
    // The parameter cap is checked here, where the count first exists, so no
    // part of the pipeline can see a definition the expander would refuse.
    if (out.params.size() + (out.variadic ? 1U : 0U) > support::kMaxMacroParameters) {
      pushError(PPError{location.span(),
                        "this macro has more than " + std::to_string(support::kMaxMacroParameters) +
                            " parameters",
                        PPErrorCode::MacroParameterLimit});
      return false;
    }
    skipTrivia(definition, index);
    if (index < definition.size() && definition[index].is(lex::TokenKind::Comma)) {
      ++index;
      continue;
    }
    break;
  }

  skipTrivia(definition, index);
  if (index >= definition.size() || !definition[index].is(lex::TokenKind::RParen)) {
    return fail("expected ')' in this macro's parameter list");
  }
  ++index;
  return true;
}

bool Preprocessor::buildReplacementList(const std::vector<PPToken>& definition, std::size_t index,
                                        MacroInfo& out) {
  const SourceLoc location = out.defineName.valid() ? out.defineName : out.define;
  const auto fail = [&](const PPToken& token, std::string message, PPErrorCode code) {
    pushError(PPError{token.loc.spelling.valid() ? token.loc.spelling.span() : location.span(),
                      std::move(message), code});
    return false;
  };

  // The body, trivia removed. `##` needs no reassembly: the lexer's longest
  // match already made it one token, and `# #` two, which is precisely the
  // distinction the paste operator depends on.
  std::vector<PPToken> tokens;
  for (; index < definition.size(); ++index) {
    const PPToken& token = definition[index];
    if (isSignificant(token)) {
      tokens.push_back(token);
    }
  }

  out.spellings.clear();
  out.body.clear();
  out.body.reserve(tokens.size());
  for (const PPToken& token : tokens) {
    const std::string_view text = spelling(token);
    MacroBodyToken entry;
    entry.token = token;
    // A token that names a parameter is replaced at the invocation, so its
    // spelling in the body is the parameter's name and the replacement carries
    // its own spelling. Matching is by text because the same name can be spelled
    // in a synthetic definition (a `-D`) with no source span to intern from.
    entry.param = token.is(lex::TokenKind::Identifier)
                      ? parameterIndex(text, out, session_->symbols())
                      : kNotAParameter;
    entry.spellingOffset = static_cast<std::uint32_t>(out.spellings.size());
    entry.spellingLength = static_cast<std::uint32_t>(text.size());
    out.spellings.append(text);
    out.body.push_back(entry);
  }

  if (out.body.empty()) {
    return true;
  }

  // `#` must be followed by a parameter and `##` must have an operand on both
  // sides; both are constraints the standard states, so both are diagnostics
  // rather than undefined behaviour. The `#`/`##` operand flags are decided
  // once, here, because they are static properties of the definition.
  const auto markRaw = [&](std::uint8_t slot, bool withHash) {
    if (slot == kNotAParameter) {
      return;
    }
    if (slot < out.params.size()) {
      if (withHash) {
        out.params[slot].usedWithHash = true;
      } else {
        out.params[slot].usedWithPaste = true;
      }
    } else if (withHash) {
      out.variadicUsedWithHash = true;
    } else {
      out.variadicUsedWithPaste = true;
    }
  };

  // `__VA_OPT__(content)` is recognized here, as a pair of markers *around* the
  // content -- the marker is the operator and its opening parenthesis, the
  // close is the matching `)` -- so the expander never has to re-derive which
  // parentheses belong to the operator, and neither `__VA_OPT__` nor its `(` is
  // ever emitted. It is C23, ubiquitous in headers that also build with older
  // compilers, and eleven lines of work once the pair is marked.
  for (std::size_t i = 0; i < out.body.size(); ++i) {
    if (!out.body[i].token.is(lex::TokenKind::Identifier) || out.spellingOf(i) != "__VA_OPT__") {
      continue;
    }
    if (!out.variadic) {
      return fail(out.body[i].token, "'__VA_OPT__' is only valid in the body of a variadic macro",
                  PPErrorCode::InvalidDirective);
    }
    if (i + 1 >= out.body.size() || !out.body[i + 1].token.is(lex::TokenKind::LParen)) {
      return fail(out.body[i].token, "'__VA_OPT__' must be followed by '(' in a macro body",
                  PPErrorCode::InvalidDirective);
    }
    std::size_t depth = 0;
    std::size_t close = i + 1;
    for (; close < out.body.size(); ++close) {
      if (out.body[close].token.is(lex::TokenKind::LParen)) {
        ++depth;
      } else if (out.body[close].token.is(lex::TokenKind::RParen)) {
        if (--depth == 0) {
          break;
        }
      } else if (out.spellingOf(close) == "__VA_OPT__") {
        return fail(out.body[close].token, "'__VA_OPT__' cannot nest",
                    PPErrorCode::InvalidDirective);
      }
    }
    if (close >= out.body.size()) {
      return fail(out.body[i].token, "unterminated '__VA_OPT__(' in a macro body",
                  PPErrorCode::InvalidDirective);
    }
    // Both tokens of the opening marker -- `__VA_OPT__` and its `(` -- carry
    // `variadicOptOpen`. Marking only the name would leave the `(` in the
    // output, which is how `(0 __VA_OPT__(+ x))` becomes `(0(+x)`.
    out.body[i].variadicOptOpen = true;
    out.body[i + 1].variadicOptOpen = true;
    out.body[close].variadicOptClose = true;
  }

  for (std::size_t i = 0; i < out.body.size(); ++i) {
    const lex::TokenKind kind = out.body[i].token.kind;
    if (isHash(out.body[i].token)) {
      if (i + 1 >= out.body.size() || out.body[i + 1].param == kNotAParameter) {
        return fail(out.body[i].token, "'#' must be followed by a parameter of this macro",
                    PPErrorCode::InvalidHashOperand);
      }
      markRaw(out.body[i + 1].param, /*withHash=*/true);
    } else if (kind == lex::TokenKind::HashHash) {
      if (i == 0 || i + 1 >= out.body.size()) {
        return fail(out.body[i].token,
                    "'##' cannot appear at the beginning or end of a replacement list",
                    PPErrorCode::InvalidPaste);
      }
      markRaw(out.body[i - 1].param, /*withHash=*/false);
      markRaw(out.body[i + 1].param, /*withHash=*/false);
    }
  }
  return true;
}

void Preprocessor::handleDefine(const std::vector<PPToken>& line, const SourceLoc& directive) {
  std::size_t index = detail::afterDirectiveName(line);
  const PPToken* nameToken = nextSignificant(line, index);
  if (nameToken == nullptr || !nameToken->is(lex::TokenKind::Identifier)) {
    pushError(
        PPError{directive.span(), "#define requires a macro name", PPErrorCode::MissingMacroName});
    return;
  }

  const std::vector<PPToken> definition(line.begin() + static_cast<std::ptrdiff_t>(index),
                                        line.end());
  MacroInfo info;
  if (!buildMacroInfo(definition, directive, nameToken->loc.spelling, info)) {
    return;
  }
  MacroTable::Change change = macros_.define(std::move(info));
  if (change.error.has_value()) {
    pushError(*change.error);
    return;
  }
  if (change.changed) {
    ++record_.stats().macrosDefined;
  }
}

void Preprocessor::handleUndef(const std::vector<PPToken>& line, const SourceLoc& directive) {
  std::size_t index = detail::afterDirectiveName(line);
  const PPToken* nameToken = nextSignificant(line, index);
  if (nameToken == nullptr || !nameToken->is(lex::TokenKind::Identifier)) {
    pushError(
        PPError{directive.span(), "#undef requires a macro name", PPErrorCode::MissingMacroName});
    return;
  }
  // A macro name in `#undef` is not expanded: the directive names the macro.
  (void)macros_.undef(session_->symbols().intern(spelling(*nameToken)), nameToken->loc.spelling);
}

void Preprocessor::defineFromText(std::string_view text) {
  // `-D NAME`, `-D NAME=body`, `-D NAME(params)=body`. The `=` is split at the
  // *text* level and at paren depth zero, because it separates the name from the
  // body: lexing `NAME=body` as a definition would put the `=` in the body.
  std::size_t equals = std::string_view::npos;
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')') {
      --depth;
    } else if (text[i] == '=' && depth == 0) {
      equals = i;
      break;
    }
  }
  std::string combined(text.substr(0, equals));
  if (equals != std::string_view::npos) {
    const std::string_view body = text.substr(equals + 1);
    if (!body.empty()) {
      // A separating space, so a function-like definition's `)` cannot glue
      // itself to the first body token.
      combined += ' ';
      combined += body;
    }
  }

  // `-D` is `#define` over text, so it goes through the same builder: the two
  // cannot drift, and a command-line macro gets the same parameter analysis as
  // one written in a header.
  //
  // The text is lexed as a *real* source file, not as a buffer with no identity:
  // the tokens it produces become part of the translation unit, so a caret on
  // one of them has to point at something a reader can find. `<command line>` is
  // the attribution other compilers use for exactly this declaration.
  const support::Fallible<support::FileId> unit = session_->addFile("<command line>", combined);
  if (!unit.hasValue()) {
    return;
  }
  const support::SourceFile* source = session_->sources().find(unit.value());
  if (source == nullptr) {
    return;
  }
  const lex::TokenStream stream = lex::TokenStream::lex(unit.value(), source->text);
  std::vector<PPToken> definition;
  for (const std::uint32_t tokenIndex : stream.significantIndices()) {
    definition.push_back(toPP(stream.tokens()[tokenIndex], unit.value()));
  }
  if (definition.empty()) {
    return;
  }

  MacroInfo info;
  if (!buildMacroInfo(definition, SourceLoc{}, definition.front().loc.spelling, info)) {
    return;
  }
  macros_.install(std::move(info));
}

void Preprocessor::handleInclude(const std::vector<PPToken>& line, const SourceLoc& directive,
                                 bool next) {
  const std::string directiveName = next ? "#include_next" : "#include";
  std::size_t index = detail::afterDirectiveName(line);
  const PPToken* nameToken = nextSignificant(line, index);
  if (nameToken == nullptr) {
    pushError(PPError{directive.span(), directiveName + " expects a file name",
                      PPErrorCode::InvalidDirective});
    return;
  }

  // A written name arrived as one `HeaderName` token, spliced from the raw bytes
  // of the line before this handler ran: the delimiters are part of its spelling
  // and the name is what is between them. Anything else is the computed form,
  // which is macro expanded first.
  std::string name;
  bool angle = false;
  if (nameToken->is(lex::TokenKind::HeaderName)) {
    const std::string_view written = spelling(*nameToken);
    angle = !written.empty() && written.front() == '<';
    name = std::string(written.size() >= 2 ? written.substr(1, written.size() - 2) : written);
  } else if (nameToken->is(lex::TokenKind::Less)) {
    // A `<` here means the header-name scan found no closing `>`, which is the
    // one thing it cannot report itself.
    pushError(PPError{directive.span(), "unterminated '<...>' in " + directiveName,
                      PPErrorCode::InvalidDirective});
    return;
  } else {
    std::vector<PPToken> expanded;
    const std::size_t base = contexts_.size();
    contexts_.push_back(Context{detail::withoutTrivia(std::span(line).subspan(index)), 0,
                                support::kInvalidSym, kNoExpansion, SourceLoc{}});
    drainContexts(base, expanded, /*expression=*/false);
    if (expanded.empty()) {
      pushError(PPError{directive.span(), "empty file name in " + directiveName,
                        PPErrorCode::InvalidDirective});
      return;
    }
    if (expanded.front().is(lex::TokenKind::StringLiteral)) {
      const std::string_view literal = spelling(expanded.front());
      name =
          literal.size() >= 2 ? std::string(literal.substr(1, literal.size() - 2)) : std::string();
    } else if (expanded.front().is(lex::TokenKind::Less)) {
      // A macro that expands to `<...>`: not a header-name by the standard's
      // rules -- a name cannot be produced by expansion -- but accepted, as GCC
      // accepts it. The bytes are re-read from where the `<` was written with
      // the same scanner the written form uses, so the two cannot disagree about
      // what a name is.
      const support::SourceFile* source =
          session_->sources().find(expanded.front().loc.spelling.file);
      const std::optional<lex::HeaderName> header =
          source == nullptr
              ? std::nullopt
              : lex::scanHeaderName(source->text, expanded.front().loc.spelling.offset);
      if (!header.has_value()) {
        pushError(PPError{directive.span(), "unterminated '<...>' in " + directiveName,
                          PPErrorCode::InvalidDirective});
        return;
      }
      angle = true;
      name = std::string(header->text);
    } else {
      pushError(PPError{expanded.front().loc.spelling.span(),
                        directiveName + " expects \"a file name\" or <a file name>",
                        PPErrorCode::InvalidDirective});
      return;
    }
  }

  if (name.empty()) {
    pushError(PPError{directive.span(), "empty file name in " + directiveName,
                      PPErrorCode::InvalidDirective});
    return;
  }

  const std::size_t includeDepth =
      std::min(options_.budgets.includeDepth, support::kMaxIncludeDepth);
  if (files_.size() >= includeDepth) {
    pushError(
        PPError{directive.span(),
                "include depth exceeded after " + std::to_string(files_.size()) + " nested files",
                PPErrorCode::IncludeDepthExceeded});
    return;
  }
  const std::size_t includesPerUnit =
      std::min(options_.budgets.includesPerUnit, support::kMaxIncludesPerUnit);
  if (record_.stats().includeDirectives >= includesPerUnit) {
    pushError(PPError{directive.span(),
                      "more than " + std::to_string(includesPerUnit) +
                          " includes in one translation unit",
                      PPErrorCode::IncludeBudgetExceeded});
    return;
  }

  const std::string fromDir = files_.empty() ? std::string() : files_.back().dir;
  support::Fallible<IncludeOpen> opened = resolver_.open(name, angle, fromDir, next);
  if (!opened) {
    pushError(PPError{directive.span(), opened.error(), PPErrorCode::IncludeNotFound});
    return;
  }

  InclusionRecord inclusion;
  inclusion.directive = directive;
  inclusion.included = opened.value().file;
  inclusion.path = opened.value().path;
  inclusion.depth = static_cast<std::uint32_t>(files_.size());
  inclusion.isSystem = opened.value().isSystem;

  // A cycle in the include chain is reported by name, which is what makes the
  // error actionable: "a.h includes itself through b.h" rather than "too deep".
  for (const FileFrame& frame : files_) {
    if (frame.identity == opened.value().identity) {
      if (!selfReferenceReported_) {
        selfReferenceReported_ = true;
        std::string chain;
        for (const FileFrame& entry : files_) {
          chain += entry.path + " -> ";
        }
        chain += opened.value().path;
        pushError(PPError{directive.span(),
                          "file '" + opened.value().path + "' includes itself: " + chain,
                          PPErrorCode::IncludeSelfReference});
      }
      if (record_.recording()) {
        record_.addInclude(std::move(inclusion));
      }
      return;
    }
  }

  // The optimization: a guarded file whose guard is already defined is elided
  // without being opened again. The guard is the *current* definition, which is
  // what makes `#undef GUARD` correctly re-include the file.
  bool skip = options_.optimizeIncludes && resolver_.isOnce(opened.value().identity);
  if (!skip && options_.optimizeIncludes) {
    if (const std::optional<support::SymId> guard = resolver_.guardOf(opened.value().identity)) {
      skip = macros_.isDefined(*guard);
    }
  }
  inclusion.guardSkipped = skip;
  inclusion.read = !skip && opened.value().read;
  if (record_.recording()) {
    record_.addInclude(std::move(inclusion));
  }
  if (skip) {
    return;
  }

  pushFile(opened.value().file, opened.value().path, opened.value().dir, opened.value().isSystem,
           opened.value().identity);
}

void Preprocessor::handleConditional(const std::vector<PPToken>& line, const SourceLoc& directive,
                                     std::string_view name) {
  std::size_t index = detail::afterDirectiveName(line);
  const PPToken* argument = nextSignificant(line, index);

  const bool emitting = conditionals_.emitting();
  const auto report = [&](PPErrorCode code, std::string message) {
    pushError(PPError{directive.span(), std::move(message), code});
  };

  if (name == "endif") {
    if (const std::optional<PPErrorCode> error = conditionals_.close()) {
      report(*error, "'#endif' without a matching '#if'");
    }
    return;
  }
  if (name == "else") {
    if (const std::optional<PPErrorCode> error = conditionals_.takeElse()) {
      report(*error, *error == PPErrorCode::ElseAfterElse
                         ? "'#else' after '#else' in the same conditional"
                         : "'#else' without a matching '#if'");
    }
    return;
  }

  const bool isIf = name == "if" || name == "ifdef" || name == "ifndef";
  bool condition = false;

  if (name == "if" || name == "elif") {
    // The condition of a skipped group is not evaluated at all: a `#if` whose
    // text would not compile is legal inside a group that is not taken, and
    // evaluating it would report errors for code the user excluded.
    const bool wanted = isIf ? emitting : conditionals_.shouldEvaluateElif();
    if (wanted) {
      bool ok = true;
      condition = evaluateCondition(line, index, ok);
      if (!ok) {
        condition = false;
      }
    }
  } else {
    const bool wanted = isIf ? emitting : conditionals_.shouldEvaluateElif();
    if (wanted) {
      if (argument == nullptr || !argument->is(lex::TokenKind::Identifier)) {
        report(PPErrorCode::InvalidDirective, "'#" + std::string(name) + "' expects a macro name");
      } else {
        const bool defined = macros_.isDefined(session_->symbols().intern(spelling(*argument)));
        condition = (name == "ifdef" || name == "elifdef") ? defined : !defined;
      }
    }
  }

  if (isIf) {
    if (const std::optional<PPErrorCode> error =
            conditionals_.open(condition, support::kMaxConditionalNesting)) {
      report(*error, "conditional directives nest more than " +
                         std::to_string(support::kMaxConditionalNesting) + " levels deep");
    }
    return;
  }

  if (const std::optional<PPErrorCode> error = conditionals_.takeElif(condition)) {
    report(*error, *error == PPErrorCode::ElseAfterElse
                       ? "'#elif' after '#else' in the same conditional"
                       : "'#elif' without a matching '#if'");
  }
}

bool Preprocessor::evaluateCondition(const std::vector<PPToken>& line, std::size_t from, bool& ok) {
  ok = false;
  std::vector<PPToken> expression = detail::withoutTrivia(std::span(line).subspan(from));
  if (expression.empty()) {
    pushError(PPError{detail::cover(line).span(), "expected an expression after '#if'",
                      PPErrorCode::ExpressionSyntax});
    return false;
  }

  const std::size_t base = contexts_.size();
  contexts_.push_back(
      Context{std::move(expression), 0, support::kInvalidSym, kNoExpansion, SourceLoc{}});
  std::vector<PPToken> expanded;
  drainContexts(base, expanded, /*expression=*/true);
  // `drainContexts` restores the floor itself; nothing else can have changed it.

  ConstExprOptions options;
  options.text = this;
  const ConstExprResult result = evaluateConstExpr(expanded, options);
  if (options_.warnUndef) {
    for (const PPError& undefined : result.undefinedNames) {
      pushWarning(undefined);
    }
  }
  if (!result.ok) {
    pushError(result.error);
    return false;
  }
  ok = true;
  return result.value.truthy();
}

void Preprocessor::handleLine(const std::vector<PPToken>& line, const SourceLoc& directive) {
  std::size_t index = detail::afterDirectiveName(line);
  skipTrivia(line, index);
  const PPToken* number = index < line.size() ? &line[index] : nullptr;
  if (number == nullptr || !number->is(lex::TokenKind::IntegerLiteral)) {
    pushError(PPError{directive.span(), "'#line' expects a line number",
                      PPErrorCode::InvalidLineDirective});
    return;
  }
  std::int64_t value = 0;
  for (const char c : spelling(*number)) {
    if (c < '0' || c > '9') {
      pushError(PPError{number->loc.spelling.span(), "'#line' expects a decimal line number",
                        PPErrorCode::InvalidLineDirective});
      return;
    }
    value = value * 10 + (c - '0');
    if (value > static_cast<std::int64_t>(support::kMaxOffset)) {
      pushError(PPError{number->loc.spelling.span(), "line number is too large",
                        PPErrorCode::InvalidLineDirective});
      return;
    }
  }
  ++index;
  skipTrivia(line, index);
  if (index < line.size() && line[index].is(lex::TokenKind::StringLiteral)) {
    const std::string_view literal = spelling(line[index]);
    fileNameOverride_ =
        literal.size() >= 2 ? std::string(literal.substr(1, literal.size() - 2)) : std::string();
  }
  // `#line N` says the *next* line is N, so the offset is relative to the line
  // after the directive -- and the physical line, not the already-adjusted one.
  const std::int64_t physical = currentLine(directive) - lineOffset_;
  lineOffset_ = value - (physical + 1);
}

void Preprocessor::handleErrorOrWarning(const std::vector<PPToken>& line,
                                        const SourceLoc& directive, bool warning) {
  std::string message = rawTextOf(line, detail::afterDirectiveName(line));
  if (message.empty()) {
    message = warning ? "#warning" : "#error";
  }
  PPError error{directive.span(), std::move(message),
                warning ? PPErrorCode::WarningDirective : PPErrorCode::ErrorDirective};
  if (warning) {
    pushWarning(std::move(error));
  } else {
    pushError(std::move(error));
  }
}

void Preprocessor::handlePragma(const std::vector<PPToken>& line, const SourceLoc& directive) {
  std::size_t index = detail::afterDirectiveName(line);
  const PPToken* name = nextSignificant(line, index);
  if (name != nullptr && name->is(lex::TokenKind::Identifier) && spelling(*name) == "once") {
    if (!files_.empty()) {
      resolver_.markOnce(files_.back().identity);
    }
    return;
  }
  // Every other pragma is preserved in the record and not interpreted: a pragma
  // is each target's business, and a compiler that pretends otherwise grows
  // piles of vendor-specific state. `-Wunknown-pragmas` is the opt-in that turns
  // "we ignored it" into a diagnostic, and it is off by default for the same
  // reason.
  if (options_.warnUnknownPragma) {
    pushWarning(PPError{directive.span(), "unknown pragma is ignored", PPErrorCode::UnknownPragma});
  }
}

} // namespace minc::pp
