// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// The expansion half of the preprocessor.
//
// Three rules are adopted verbatim from GCC's write-up because they are the
// subtle ones, and each is one line of code here as a result of the design:
//
//  1. A macro is disabled while *its own* replacement list is scanned, and
//     re-enabled when that context is popped. The disable is not a separate
//     stack: it is "is this macro the `macro` of a live context".
//  2. The "is a `(` next?" test happens while the macro is still enabled, and
//     only then is a context pushed -- so the parenthesis search never has to
//     undo a decision.
//  3. A name that was not replaced because its macro was disabled is marked
//     ineligible for further replacement. That mark is the only per-token
//     expansion state there is, and it is what makes `#define A B` + `#define B
//     A` terminate with `A` instead of expanding forever.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pp_internal.h"
#include "support/limits.h"

#include "pp/preprocessor.h"

namespace minc::pp {
namespace {

using detail::isSignificant;
using detail::skipTrivia;

// The parameter slot an argument index refers to. The variadic parameters share
// the slot after the named ones.
[[nodiscard]] bool usesRawArgument(const MacroInfo& macro, std::size_t slot) {
  if (slot < macro.params.size()) {
    return macro.params[slot].usedWithHash || macro.params[slot].usedWithPaste;
  }
  return macro.variadicUsedWithHash || macro.variadicUsedWithPaste;
}

} // namespace

bool Preprocessor::isIneligible(const PPToken& token) const {
  return token.has(PPTokenFlag::Ineligible);
}

bool Preprocessor::isMacroDisabled(support::SymId name) const {
  for (const Context& context : contexts_) {
    if (context.macro == name) {
      return true;
    }
  }
  return false;
}

void Preprocessor::popContext() {
  if (contexts_.empty()) {
    return;
  }
  const Context& context = contexts_.back();
  if (context.siteIndex != kNoSite) {
    record_.closeExpansion(context.siteIndex, static_cast<std::uint32_t>(output_.size()));
  }
  contexts_.pop_back();
}

std::optional<PPToken> Preprocessor::takeAbove(std::size_t base) {
  while (contexts_.size() > base) {
    Context& context = contexts_.back();
    if (context.index < context.tokens.size()) {
      const PPToken token = context.tokens[context.index++];
      if (isSignificant(token)) {
        return token;
      }
      continue;
    }
    popContext();
  }
  return std::nullopt;
}

// --- invocation -------------------------------------------------------------

bool Preprocessor::tryExpand(PPToken& token, bool emitPath) {
  const MacroInfo* macro = macros_.find(spelling(token));
  if (macro == nullptr) {
    return false;
  }
  // `__has_include` and `defined` are operators, not macros: they exist only in
  // an `#if` line and are handled while draining one. Reporting that here means
  // a stray use is diagnosed instead of silently pasted through.
  if (macro->builtin == BuiltinKind::HasInclude && !expressionMode_) {
    const std::string_view text = spelling(token);
    pushError(PPError{token.loc.spelling.span(),
                      "'" + std::string(text) + "' can only be used in '#if' and '#elif'",
                      PPErrorCode::ExpressionSyntax});
    return false;
  }
  if (isMacroDisabled(macro->name) || isIneligible(token)) {
    // Rule 3. The mark is kept on the token, which is the token that came out of
    // a replacement list or an argument, so re-enabling the macro later does not
    // resurrect it -- that is what "blue paint" means in one line.
    token.flags = static_cast<PPTokenFlags>(token.flags | flagOf(PPTokenFlag::Ineligible));
    return false;
  }
  if (macro->isFunctionLike() && macro->builtin != BuiltinKind::HasInclude) {
    // Rule 2: the next significant token decides, and only a `(` starts an
    // invocation. A function-like macro's name on its own is an ordinary
    // identifier.
    const PPToken next = peekSignificant();
    if (!next.is(lex::TokenKind::LParen)) {
      return false;
    }
  }
  expandMacro(*macro, token, emitPath);
  return true;
}

void Preprocessor::expandMacro(const MacroInfo& macro, const PPToken& nameToken, bool emitPath) {
  // The nesting cap. The contexts are a vector, so this is not a stack-safety
  // bound; it is the bound that keeps the provenance chain and the backtrace a
  // diagnostic can render from growing past what any human would read. Hitting
  // it abandons *this* expansion and keeps the token stream a valid prefix.
  if (contexts_.size() >= support::kMaxExpansionDepth) {
    pushError(
        PPError{nameToken.loc.spelling.span(),
                "macro expansion nests more than " + std::to_string(support::kMaxExpansionDepth) +
                    " levels deep; expansion of '" + std::string(spelling(nameToken)) + "' stopped",
                PPErrorCode::ExpansionDepthExceeded});
    return;
  }

  // Provenance first: the frame is what turns a diagnostic inside a macro into
  // "in expansion of macro 'X'". Hash-consed, so a macro invoked in a loop is
  // one frame shared by every expansion.
  ExpansionFrame frame;
  frame.macro = macro.name;
  frame.invocation = nameToken.loc.spelling.valid()
                         ? nameToken.loc.spelling
                         : (contexts_.empty() ? SourceLoc{} : contexts_.back().invocation);
  frame.parent = nameToken.loc.expansion;
  frame.define = macro.defineName;
  const ExpansionId expansion = expansions_.intern(frame);
  ++useCounts_[macro.name];

  const bool functionLike = macro.isFunctionLike() && macro.builtin != BuiltinKind::HasInclude;
  std::vector<std::vector<PPToken>> arguments;
  if (functionLike) {
    Arguments collected = collectArguments(macro, nameToken);
    if (!collected.ok) {
      pushError(collected.error);
      return;
    }
    arguments = std::move(collected.values);
  }

  std::vector<PPToken> replacement;
  if (macro.isBuiltin()) {
    std::optional<PPError> error;
    if (!expandBuiltin(macro, nameToken, replacement, error)) {
      if (error.has_value()) {
        pushError(std::move(*error));
      }
      return;
    }
  } else {
    substitute(macro, nameToken, expansion, arguments, replacement);
  }

  // Checked before the context is pushed, so the array cannot grow past the
  // budget: the bound is a bound, not a report after the fact.
  if (overExpansionBudget(expandedTokens_ + replacement.size())) {
    return;
  }
  expandedTokens_ += replacement.size();

  Context context;
  context.tokens = std::move(replacement);
  context.macro = macro.name;
  context.expansion = expansion;
  context.invocation = frame.invocation;
  if (emitPath && options_.recordExpansionMap && record_.recording()) {
    ExpansionSite site;
    site.invocation = frame.invocation;
    site.macro = macro.name;
    site.producedBegin = static_cast<std::uint32_t>(output_.size());
    site.frame = expansion;
    record_.addExpansion(site);
    context.siteIndex = static_cast<std::uint32_t>(record_.expansions().size() - 1);
  }
  contexts_.push_back(std::move(context));
}

Preprocessor::Arguments Preprocessor::collectArguments(const MacroInfo& macro,
                                                       const PPToken& nameToken) {
  Arguments result;
  const auto fail = [&](std::string message) {
    result.ok = false;
    result.error = PPError{nameToken.loc.spelling.span(), std::move(message),
                           PPErrorCode::MissingMacroArguments};
    return result;
  };

  // The `(` is known to be next (the caller peeked), but trivia has to be walked
  // to reach it, and consuming it here keeps the argument scanner uniform.
  PPToken open = take();
  while (isPPTrivia(open.kind)) {
    open = take();
  }
  if (!open.is(lex::TokenKind::LParen)) {
    result.ok = false;
    result.error = PPError{nameToken.loc.spelling.span(),
                           "internal error: expected '(' to start this macro's arguments",
                           PPErrorCode::UnterminatedMacroArguments};
    return result;
  }

  std::vector<std::vector<PPToken>> raw(1);
  bool sawSignificant = false;
  // A single depth counter over `(` `[` `{`: only a top-level comma separates
  // arguments, and a comma inside any bracket pair belongs to the argument.
  int depth = 1;
  bool done = false;
  while (!done) {
    PPToken token = take();
    if (token.isEndOfFile()) {
      result.ok = false;
      result.error =
          PPError{nameToken.loc.spelling.span(),
                  "unterminated argument list for macro '" + std::string(spelling(nameToken)) + "'",
                  PPErrorCode::UnterminatedMacroArguments};
      return result;
    }
    if (isPPTrivia(token.kind)) {
      raw.back().push_back(token);
      continue;
    }
    switch (token.kind) {
    case lex::TokenKind::LParen:
    case lex::TokenKind::LBracket:
    case lex::TokenKind::LBrace:
      ++depth;
      break;
    case lex::TokenKind::RParen:
      --depth;
      if (depth == 0) {
        done = true;
        continue;
      }
      break;
    case lex::TokenKind::RBracket:
    case lex::TokenKind::RBrace:
      if (depth > 1) {
        --depth;
      }
      break;
    case lex::TokenKind::Comma:
      if (depth == 1) {
        // The trivia before the comma belongs to neither argument.
        while (!raw.back().empty() && isPPTrivia(raw.back().back().kind)) {
          raw.back().pop_back();
        }
        raw.emplace_back();
        sawSignificant = true;
        continue;
      }
      break;
    default:
      break;
    }
    sawSignificant = sawSignificant || isSignificant(token);
    raw.back().push_back(token);
  }

  std::size_t given = raw.size();
  // `F()` for a macro with no parameters is *zero* arguments, not one empty
  // one; for a macro with parameters it is one empty argument, which is why this
  // depends on the declaration and not on the text.
  if (given == 1 && !sawSignificant && macro.params.empty()) {
    given = 0;
  }

  if (given < macro.params.size()) {
    return fail("macro '" + std::string(spelling(nameToken)) + "' requires at least " +
                std::to_string(macro.params.size()) + " argument(s), but " + std::to_string(given) +
                " were given");
  }

  // `...` collects *every* remaining argument, re-inserted with the commas that
  // separated them, because `__VA_ARGS__` is one token sequence and not a list.
  // This is also why the count check above only compares against the *named*
  // parameters: a variadic macro is never called with too many arguments.
  if (macro.variadic) {
    if (given == macro.params.size()) {
      // No argument for `...`, or `()` for a macro whose only parameter is `...`:
      // one empty argument, not zero. That distinction is the source of the
      // "is `__VA_ARGS__` empty?" folklore, and it is the standard's rule.
      raw.emplace_back();
    } else if (given > macro.params.size()) {
      std::vector<PPToken> variadic = std::move(raw[macro.params.size()]);
      for (std::size_t index = macro.params.size() + 1; index < given; ++index) {
        PPToken comma;
        comma.kind = lex::TokenKind::Comma;
        comma.length = 1;
        comma.loc.spelling = nameToken.loc.spelling;
        comma.scratch = addScratch(",");
        variadic.push_back(comma);
        variadic.insert(variadic.end(), raw[index].begin(), raw[index].end());
      }
      raw.resize(macro.params.size() + 1);
      raw[macro.params.size()] = std::move(variadic);
    }
  } else if (given > macro.params.size()) {
    result.ok = false;
    result.error = PPError{nameToken.loc.spelling.span(),
                           "macro '" + std::string(spelling(nameToken)) + "' takes at most " +
                               std::to_string(macro.params.size()) + " argument(s), but " +
                               std::to_string(given) + " were given",
                           PPErrorCode::TooManyMacroArguments};
    return result;
  }

  result.ok = true;
  result.values = std::move(raw);
  return result;
}

void Preprocessor::substitute(const MacroInfo& macro, const PPToken& nameToken,
                              ExpansionId expansion, std::vector<std::vector<PPToken>>& arguments,
                              std::vector<PPToken>& out) {
  const std::size_t variadicSlot = macro.params.size();
  // Pre-expansion happens once per argument even when the parameter appears
  // several times, which is both correct and the difference between linear and
  // quadratic work on a repeated parameter.
  std::vector<std::vector<PPToken>> expanded(arguments.size());
  for (std::size_t slot = 0; slot < arguments.size(); ++slot) {
    if (usesRawArgument(macro, slot)) {
      continue;
    }
    std::vector<PPToken> value = detail::withoutTrivia(arguments[slot]);
    if (value.empty()) {
      continue;
    }
    std::vector<PPToken> result;
    (void)preExpand(value, result);
    expanded[slot] = std::move(result);
  }

  const auto slotArgument = [&](std::uint8_t slot, bool raw) -> const std::vector<PPToken>& {
    static const std::vector<PPToken> empty;
    if (slot == kNotAParameter) {
      return empty;
    }
    const std::size_t index = slot;
    if (raw) {
      return index < arguments.size() ? arguments[index] : empty;
    }
    return index < expanded.size() ? expanded[index] : empty;
  };

  // Whether `__VA_OPT__`'s content is kept depends on the variadic argument
  // having any tokens at all -- the standard's rule, and the reason
  // `__VA_OPT__` is a better answer than `, ## __VA_ARGS__` for the same job.
  const bool variadicNonEmpty =
      variadicSlot < arguments.size() && !detail::withoutTrivia(arguments[variadicSlot]).empty();
  bool skipVariadicOpt = false;

  for (std::size_t i = 0; i < macro.body.size(); ++i) {
    const MacroBodyToken& body = macro.body[i];

    if (body.variadicOptOpen) {
      // The marker itself is never emitted; only its content is, and only when
      // the variadic part is non-empty.
      skipVariadicOpt = !variadicNonEmpty;
      continue;
    }
    if (body.variadicOptClose) {
      skipVariadicOpt = false;
      continue;
    }
    if (skipVariadicOpt) {
      continue;
    }

    if (detail::isHash(body.token) && i + 1 < macro.body.size()) {
      const std::uint8_t slot = macro.body[i + 1].param;
      const std::vector<PPToken>& argument = slotArgument(slot, /*raw=*/true);
      const std::string literal = stringifyTokens(*this, argument);
      PPToken token = makeScratchToken(lex::TokenKind::StringLiteral, literal,
                                       body.token.loc.spelling, PPTokenFlag::Stringified);
      token.loc.expansion = expansion;
      out.push_back(token);
      ++i; // the stringified parameter is consumed
      continue;
    }

    if (body.token.is(lex::TokenKind::HashHash)) {
      PPToken marker = body.token;
      marker.flags = flagOf(PPTokenFlag::PasteOperator);
      marker.loc.expansion = expansion;
      out.push_back(marker);
      continue;
    }

    if (body.param == kNotAParameter) {
      // A plain body token. Its spelling stays where it was written; only its
      // expansion chain changes, which is what "produced by this invocation"
      // means for a token the macro did not touch.
      PPToken token = body.token;
      token.loc.expansion = expansion;
      out.push_back(token);
      continue;
    }

    const bool raw = usesRawArgument(macro, body.param);
    const std::vector<PPToken>& argument = slotArgument(body.param, raw);
    // Whether the argument is *empty* is a question about its significant
    // tokens, not about its first token: `C(my, Name)` gives the second
    // argument a leading space, and treating that space as the argument would
    // make every argument written with a space after the comma look empty.
    if (detail::withoutTrivia(argument).empty()) {
      // An empty argument next to `##` becomes a placemarker so the operator
      // still has two operands; anywhere else it contributes nothing.
      if (raw) {
        PPToken placemarker;
        placemarker.kind = lex::TokenKind::EndOfFile;
        placemarker.flags = flagOf(PPTokenFlag::Placemarker);
        // Zero-width: a placemarker has no spelling, and `spelling` reads the
        // bytes the location names, so the length is what makes it empty.
        // Keeping the file and offset leaves a usable caret position.
        placemarker.loc.spelling =
            SourceLoc{body.token.loc.spelling.file, body.token.loc.spelling.offset, 0};
        placemarker.loc.expansion = expansion;
        out.push_back(placemarker);
      }
      continue;
    }
    for (const PPToken& token : argument) {
      if (!isSignificant(token)) {
        continue; // argument trivia is not part of the replacement
      }
      PPToken copy = token;
      copy.loc.expansion = expansion;
      copy.flags = static_cast<PPTokenFlags>(copy.flags & ~flagOf(PPTokenFlag::Param));
      out.push_back(copy);
    }
  }

  // The paste pass. It runs after substitution so that an operand which is
  // itself a `#` or `##` from an argument cannot be mistaken for the operator:
  // the operator is the token carrying `PasteOperator`.
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!out[i].has(PPTokenFlag::PasteOperator)) {
      continue;
    }
    if (i == 0 || i + 1 >= out.size()) {
      pushError(PPError{out[i].loc.spelling.span(),
                        "'##' has no operand on one side after substitution",
                        PPErrorCode::InvalidPaste});
      out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
      --i;
      continue;
    }
    const std::string left{spelling(out[i - 1])};
    const std::string right{spelling(out[i + 1])};
    if (left.size() + right.size() > support::kMaxTokenBytes) {
      std::string message = "pasting '";
      message += left;
      message += "' and '";
      message += right;
      message += "' produces more than ";
      message += std::to_string(support::kMaxTokenBytes);
      message += " bytes";
      pushError(PPError{out[i].loc.spelling.span(), std::move(message), PPErrorCode::TokenTooLong});
      out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
      --i;
      continue;
    }
    PPToken pasted;
    if (!pasteTokens(*this, out[i - 1], out[i + 1], pasted)) {
      std::string message = "pasting '";
      message += left;
      message += "' and '";
      message += right;
      message += "' does not produce a single token";
      pushError(PPError{out[i].loc.spelling.span(), std::move(message), PPErrorCode::InvalidPaste});
      // Both operands are kept and the operator is dropped, which is what GCC
      // does: the output stays a valid token stream and the user sees the two
      // pieces that did not fit together.
      out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
      --i;
      continue;
    }
    pasted.loc.expansion = expansion;
    out[i - 1] = pasted;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(i),
              out.begin() + static_cast<std::ptrdiff_t>(i) + 2);
    --i;
  }

  // Placemarkers that survived (one that was never an operand of a paste, which
  // cannot happen) would be zero-width nonsense in the output.
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const PPToken& token) { return token.isPlacemarker(); }),
            out.end());
  (void)nameToken;
}

// --- stand-alone expansion --------------------------------------------------

void Preprocessor::drainContexts(std::size_t base, std::vector<PPToken>& out, bool expression) {
  const std::size_t savedFloor = lookaheadFloor_;
  const bool savedExpression = expressionMode_;
  const bool savedFileLookahead = lookaheadFile_;
  lookaheadFloor_ = base;
  // An argument (and an `#if` line) is a closed world: its own tokens are all a
  // `(` test may see. A lookahead that reached the file would find the tokens
  // *after* the invocation and misread a plain identifier as a call.
  lookaheadFile_ = false;
  expressionMode_ = expression;

  while (contexts_.size() > base) {
    Context& context = contexts_.back();
    if (context.index >= context.tokens.size()) {
      popContext();
      continue;
    }
    PPToken token = context.tokens[context.index++];
    if (!isSignificant(token)) {
      continue;
    }
    if (expression && token.is(lex::TokenKind::Identifier)) {
      const std::string_view text = spelling(token);
      if (text == "defined") {
        handleDefined(out, base, token);
        continue;
      }
      if (text == "__has_include") {
        handleHasInclude(out, base, token);
        continue;
      }
    }
    if (token.is(lex::TokenKind::Identifier) && tryExpand(token, /*emitPath=*/false)) {
      continue;
    }
    out.push_back(token);
  }

  lookaheadFloor_ = savedFloor;
  lookaheadFile_ = savedFileLookahead;
  expressionMode_ = savedExpression;
}

bool Preprocessor::preExpand(const std::vector<PPToken>& raw, std::vector<PPToken>& out) {
  // Arguments and `#if` lines are expanded by the same loop the file is: push
  // the tokens as a context and drain it. `lookaheadFloor_` keeps the expansion
  // from seeing tokens outside the argument, which is the standard's rule and
  // the reason `f(g) (2)` comes out as written.
  const std::size_t base = contexts_.size();
  contexts_.push_back(Context{raw, 0, support::kInvalidSym, kNoExpansion, SourceLoc{}});
  drainContexts(base, out, /*expression=*/false);
  return true;
}

// --- operators --------------------------------------------------------------

void Preprocessor::handleDefined(std::vector<PPToken>& out, std::size_t base,
                                 const PPToken& operatorToken) {
  bool parenthesized = false;
  std::optional<PPToken> next = takeAbove(base);
  if (next.has_value() && next->is(lex::TokenKind::LParen)) {
    parenthesized = true;
    next = takeAbove(base);
  }

  bool defined = false;
  if (!next.has_value() || !next->is(lex::TokenKind::Identifier)) {
    pushError(PPError{operatorToken.loc.spelling.span(),
                      "'defined' expects an identifier, optionally in parentheses",
                      PPErrorCode::ExpressionSyntax});
  } else {
    defined = macros_.isDefined(session_->symbols().intern(spelling(*next)));
    if (parenthesized) {
      const std::optional<PPToken> close = takeAbove(base);
      if (!close.has_value() || !close->is(lex::TokenKind::RParen)) {
        pushError(PPError{operatorToken.loc.spelling.span(),
                          "expected ')' after the operand of 'defined'",
                          PPErrorCode::ExpressionSyntax});
      }
    }
  }

  // The result is an integer literal, so the evaluator never sees `defined`.
  out.push_back(makeScratchToken(lex::TokenKind::IntegerLiteral, defined ? "1" : "0",
                                 operatorToken.loc.spelling, PPTokenFlag::None));
}

void Preprocessor::handleHasInclude(std::vector<PPToken>& out, std::size_t base,
                                    const PPToken& operatorToken) {
  std::optional<PPToken> open = takeAbove(base);
  if (!open.has_value() || !open->is(lex::TokenKind::LParen)) {
    pushError(PPError{operatorToken.loc.spelling.span(),
                      "'__has_include' expects a parenthesized file name",
                      PPErrorCode::ExpressionSyntax});
    return;
  }

  // The operand is collected and then expanded, because the standard processes
  // it as `#include` does -- which is what makes `__has_include(HEADER)` work
  // when `HEADER` is a macro.
  std::vector<PPToken> argument;
  int depth = 1;
  while (true) {
    std::optional<PPToken> token = takeAbove(base);
    if (!token.has_value()) {
      pushError(PPError{operatorToken.loc.spelling.span(),
                        "unterminated '__has_include(' expression", PPErrorCode::ExpressionSyntax});
      return;
    }
    if (token->is(lex::TokenKind::LParen)) {
      ++depth;
    } else if (token->is(lex::TokenKind::RParen)) {
      --depth;
      if (depth == 0) {
        break;
      }
    }
    argument.push_back(*token);
  }

  std::vector<PPToken> expanded;
  (void)preExpand(detail::withoutTrivia(argument), expanded);
  if (expanded.empty()) {
    pushError(PPError{operatorToken.loc.spelling.span(), "'__has_include' expects a file name",
                      PPErrorCode::ExpressionSyntax});
    return;
  }

  std::string name;
  bool angle = false;
  if (expanded.front().is(lex::TokenKind::StringLiteral)) {
    const std::string_view literal = spelling(expanded.front());
    name = literal.size() >= 2 ? std::string(literal.substr(1, literal.size() - 2)) : std::string();
  } else if (expanded.front().is(lex::TokenKind::Less)) {
    angle = true;
    std::size_t close = expanded.size();
    for (std::size_t i = 1; i < expanded.size(); ++i) {
      if (expanded[i].is(lex::TokenKind::Greater)) {
        close = i;
        break;
      }
    }
    const SourceLoc& first = expanded.front().loc.spelling;
    const support::SourceFile* source = session_->sources().find(first.file);
    if (close >= expanded.size() || source == nullptr) {
      pushError(PPError{operatorToken.loc.spelling.span(), "malformed '<...>' in '__has_include'",
                        PPErrorCode::ExpressionSyntax});
      return;
    }
    name = std::string(source->slice(first.end(), expanded[close].loc.spelling.offset));
  } else {
    pushError(PPError{expanded.front().loc.spelling.span(),
                      "'__has_include' expects \"a file name\" or <a file name>",
                      PPErrorCode::ExpressionSyntax});
    return;
  }

  // Asking the resolver is the only honest answer: it is the same search order
  // and the same file identity the `#include` itself would use, so the two
  // cannot disagree.
  const std::string fromDir = files_.empty() ? std::string() : files_.back().dir;
  const bool exists = resolver_.open(name, angle, fromDir, /*includeNext=*/false).hasValue();
  out.push_back(makeScratchToken(lex::TokenKind::IntegerLiteral, exists ? "1" : "0",
                                 operatorToken.loc.spelling, PPTokenFlag::None));
}

} // namespace minc::pp
