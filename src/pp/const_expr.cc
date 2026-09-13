// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/const_expr.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "support/consteval/const_int.h"
#include "support/consteval/literal.h"

namespace minc::pp {

// The value and its arithmetic are shared with the type checker's constant
// folding; only the grammar over tokens lives here.
using ConstInt = support::ConstInt;

namespace {

class Parser {
public:
  Parser(std::span<const PPToken> tokens, const ConstExprOptions& options)
      : tokens_(tokens), options_(options) {}

  ConstExprResult run() {
    const ConstInt value = parseConditional();
    if (!failed_ && index_ < tokens_.size()) {
      failToken(peek(), "unexpected '" + std::string(spelling(peek())) + "' in #if expression");
    }
    result_.ok = !failed_;
    result_.value = failed_ ? ConstInt{} : value;
    return result_;
  }

private:
  // --- token access ---------------------------------------------------------

  [[nodiscard]] const PPToken& peek() const {
    static const PPToken end;
    return index_ < tokens_.size() ? tokens_[index_] : end;
  }
  [[nodiscard]] const PPToken& peekAt(std::size_t ahead) const {
    static const PPToken end;
    return index_ + ahead < tokens_.size() ? tokens_[index_ + ahead] : end;
  }
  const PPToken& take() {
    const PPToken& token = peek();
    if (index_ < tokens_.size()) {
      ++index_;
    }
    return token;
  }
  [[nodiscard]] bool atEnd() const {
    return index_ >= tokens_.size();
  }
  [[nodiscard]] std::string_view spelling(const PPToken& token) const {
    return options_.text == nullptr ? std::string_view{} : options_.text->spelling(token);
  }
  [[nodiscard]] support::Span spanOf(const PPToken& token) const {
    return token.loc.spelling.span();
  }

  // --- errors ---------------------------------------------------------------

  // No-op inside a short-circuited branch: the standard says the untaken branch
  // of `&&`/`||`/`?:` is not evaluated, so a division by zero there is not an
  // error. `depth_` is reset by the caller when it re-enters.
  void fail(support::Span span, std::string message) {
    if (suppressed_ > 0 || failed_) {
      return;
    }
    failed_ = true;
    result_.error = PPError{span, std::move(message), PPErrorCode::ExpressionSyntax};
  }
  void failToken(const PPToken& token, std::string message) {
    fail(spanOf(token), std::move(message));
  }

  // --- grammar --------------------------------------------------------------

  ConstInt parseConditional() {
    DepthGuard guard(*this);
    if (failed_) {
      return ConstInt{};
    }
    const ConstInt condition = parseLogicalOr();
    if (peek().is(lex::TokenKind::Question)) {
      take();
      // The taken branch is evaluated with errors live; the other one has its
      // errors suppressed, exactly as the standard requires.
      const bool taken = condition.truthy();
      const ConstInt left = suppressed(!taken, [this] { return parseConditional(); });
      if (!peek().is(lex::TokenKind::Colon)) {
        if (!failed_ && !atEnd()) {
          failToken(peek(), "expected ':' in #if conditional expression");
        }
        return ConstInt{};
      }
      take();
      const ConstInt right = suppressed(taken, [this] { return parseConditional(); });
      return taken ? left : right;
    }
    return condition;
  }

  // The right operand of `&&`/`||` is only *evaluated* if it can change the
  // answer; the other case is parsed with errors suppressed, which is what
  // makes `#if 0 && (1 / 0)` legal.
  template <typename Parse> ConstInt suppressed(bool suppress, Parse parse) {
    if (suppress) {
      ++suppressed_;
    }
    const ConstInt value = parse();
    if (suppress) {
      --suppressed_;
    }
    return value;
  }

  ConstInt parseLogicalOr() {
    ConstInt left = parseLogicalAnd();
    while (!failed_ && peek().is(lex::TokenKind::PipePipe)) {
      take();
      const ConstInt right = suppressed(left.truthy(), [this] { return parseLogicalAnd(); });
      // Short-circuit: a nonzero left operand makes the result 1 without
      // looking at the right one, and a zero one makes a live right operand the
      // only thing that can be true.
      left = ConstInt::fromSigned((left.truthy() || right.truthy()) ? 1 : 0);
    }
    return left;
  }

  ConstInt parseLogicalAnd() {
    ConstInt left = parseBitOr();
    while (!failed_ && peek().is(lex::TokenKind::AmpAmp)) {
      take();
      const ConstInt right = suppressed(!left.truthy(), [this] { return parseBitOr(); });
      left = ConstInt::fromSigned((left.truthy() && right.truthy()) ? 1 : 0);
    }
    return left;
  }

  ConstInt parseBitOr() {
    ConstInt left = parseBitXor();
    while (!failed_ && peek().is(lex::TokenKind::Pipe)) {
      take();
      const ConstInt right = parseBitXor();
      left = support::bitOr(left, right);
    }
    return left;
  }

  ConstInt parseBitXor() {
    ConstInt left = parseBitAnd();
    while (!failed_ && peek().is(lex::TokenKind::Caret)) {
      take();
      const ConstInt right = parseBitAnd();
      left = support::bitXor(left, right);
    }
    return left;
  }

  ConstInt parseBitAnd() {
    ConstInt left = parseEquality();
    while (!failed_ && peek().is(lex::TokenKind::Amp)) {
      take();
      const ConstInt right = parseEquality();
      left = support::bitAnd(left, right);
    }
    return left;
  }

  ConstInt parseEquality() {
    ConstInt left = parseRelational();
    while (!failed_) {
      const lex::TokenKind kind = peek().kind;
      if (kind != lex::TokenKind::EqualEqual && kind != lex::TokenKind::BangEqual) {
        break;
      }
      take();
      const ConstInt right = parseRelational();
      const bool equal = left.bits == right.bits;
      left = ConstInt::fromSigned((kind == lex::TokenKind::EqualEqual ? equal : !equal) ? 1 : 0);
    }
    return left;
  }

  ConstInt parseRelational() {
    ConstInt left = parseShift();
    while (!failed_) {
      const lex::TokenKind kind = peek().kind;
      if (kind != lex::TokenKind::Less && kind != lex::TokenKind::Greater &&
          kind != lex::TokenKind::LessEqual && kind != lex::TokenKind::GreaterEqual) {
        break;
      }
      take();
      const ConstInt right = parseShift();
      const int order = support::compare(left, right);
      const bool holds = kind == lex::TokenKind::Less        ? order < 0
                         : kind == lex::TokenKind::Greater   ? order > 0
                         : kind == lex::TokenKind::LessEqual ? order <= 0
                                                             : order >= 0;
      left = ConstInt::fromSigned(holds ? 1 : 0);
    }
    return left;
  }

  ConstInt parseShift() {
    ConstInt left = parseAdditive();
    while (!failed_) {
      const lex::TokenKind kind = peek().kind;
      if (kind != lex::TokenKind::LessLess && kind != lex::TokenKind::GreaterGreater) {
        break;
      }
      const PPToken op = take();
      const ConstInt right = parseAdditive();
      if (failed_) {
        return ConstInt{};
      }
      const std::optional<ConstInt> shifted = kind == lex::TokenKind::LessLess
                                                  ? support::shiftLeft(left, right)
                                                  : support::shiftRight(left, right);
      if (!shifted.has_value()) {
        fail(spanOf(op), "shift count " + std::to_string(right.bits) + " is out of range");
        return ConstInt{};
      }
      // The result of `<<`/`>>` has the promoted type of the left operand, which
      // the shared operation already carries.
      left = *shifted;
    }
    return left;
  }

  ConstInt parseAdditive() {
    ConstInt left = parseMultiplicative();
    while (!failed_) {
      const lex::TokenKind kind = peek().kind;
      if (kind != lex::TokenKind::Plus && kind != lex::TokenKind::Minus) {
        break;
      }
      take();
      const ConstInt right = parseMultiplicative();
      left = kind == lex::TokenKind::Plus ? support::add(left, right) : support::sub(left, right);
    }
    return left;
  }

  ConstInt parseMultiplicative() {
    ConstInt left = parseUnary();
    while (!failed_) {
      const lex::TokenKind kind = peek().kind;
      if (kind != lex::TokenKind::Star && kind != lex::TokenKind::Slash &&
          kind != lex::TokenKind::Percent) {
        break;
      }
      const PPToken op = take();
      const ConstInt right = parseUnary();
      if (failed_) {
        return ConstInt{};
      }
      switch (kind) {
      case lex::TokenKind::Star:
        left = support::mul(left, right);
        break;
      case lex::TokenKind::Slash: {
        const std::optional<ConstInt> quotient = support::divide(left, right);
        if (!quotient.has_value()) {
          fail(spanOf(op), "division by zero in #if expression");
          return ConstInt{};
        }
        left = *quotient;
        break;
      }
      default: {
        const std::optional<ConstInt> rest = support::remainder(left, right);
        if (!rest.has_value()) {
          fail(spanOf(op), "remainder by zero in #if expression");
          return ConstInt{};
        }
        left = *rest;
        break;
      }
      }
    }
    return left;
  }

  ConstInt parseUnary() {
    DepthGuard guard(*this);
    if (failed_) {
      return ConstInt{};
    }
    const lex::TokenKind kind = peek().kind;
    if (kind == lex::TokenKind::Minus || kind == lex::TokenKind::Plus ||
        kind == lex::TokenKind::Bang || kind == lex::TokenKind::Tilde) {
      take();
      const ConstInt operand = parseUnary();
      switch (kind) {
      case lex::TokenKind::Minus:
        return support::negate(operand);
      case lex::TokenKind::Plus:
        return operand;
      case lex::TokenKind::Bang:
        return support::logicalNot(operand);
      default:
        return support::bitNot(operand);
      }
    }
    return parsePrimary();
  }

  ConstInt parsePrimary() {
    if (failed_) {
      return ConstInt{};
    }
    const PPToken& token = peek();
    switch (token.kind) {
    case lex::TokenKind::IntegerLiteral:
      return parseIntegerLiteral(take());
    case lex::TokenKind::CharLiteral:
      return parseCharLiteral(take());
    case lex::TokenKind::LParen: {
      take();
      const ConstInt inner = parseConditional();
      if (!failed_ && !peek().is(lex::TokenKind::RParen)) {
        if (!atEnd()) {
          failToken(peek(), "expected ')' in #if expression");
        } else {
          fail(spanOf(token), "unterminated '(' in #if expression");
        }
        return ConstInt{};
      }
      if (!failed_) {
        take();
      }
      return inner;
    }
    case lex::TokenKind::Identifier: {
      // A name that is not a macro is 0 (the standard's rule). Recorded so the
      // caller can report it under -Wundef, which is the check that catches
      // typos in `#if FEATURE_X`.
      take();
      if (!suppressed_) {
        result_.undefinedNames.push_back(PPError{spanOf(token),
                                                 "undefined identifier '" +
                                                     std::string(spelling(token)) +
                                                     "' is replaced by 0 in this expression",
                                                 PPErrorCode::UndefinedIdentifier});
      }
      return ConstInt::fromSigned(0);
    }
    default:
      if (atEnd()) {
        fail(spanOf(token), "expected an expression after the last token");
      } else {
        failToken(token, "expected an integer, character or '(' in #if expression");
      }
      return ConstInt{};
    }
  }

  // --- helpers --------------------------------------------------------------

  ConstInt parseIntegerLiteral(const PPToken& token) {
    const std::string_view text = spelling(token);
    if (text.empty()) {
      failToken(token, "expected an integer literal in #if expression");
      return ConstInt{};
    }

    // `.mx` does not inherit C's implicit octal, but a `#if` reads C -- the
    // directive and the headers it came from -- so the one rule the two literal
    // readers disagree about is named at the call instead of assumed.
    const support::IntegerLiteral parsed =
        support::parseIntegerLiteral(text, support::IntegerBaseRule::ImplicitOctal);
    if (!parsed.ok) {
      failToken(token, parsed.message);
      return ConstInt{};
    }
    return parsed.value;
  }

  ConstInt parseCharLiteral(const PPToken& token) {
    const std::string_view text = spelling(token);
    if (text.size() < 2) {
      failToken(token, "malformed character literal in #if expression");
      return ConstInt{};
    }
    const support::IntegerLiteral parsed = support::parseCharLiteral(text);
    if (!parsed.ok) {
      failToken(token, parsed.message);
      return ConstInt{};
    }
    return parsed.value;
  }

  // --- state ----------------------------------------------------------------

  // Counts grammar recursion and fails instead of overflowing the stack. The
  // only recursive production here is the parenthesized/`?:` one, so this is
  // the whole guard.
  struct DepthGuard {
    explicit DepthGuard(Parser& parser) : self(&parser) {
      ++self->depth_;
      if (self->depth_ > self->options_.maxDepth) {
        self->fail({}, "conditional expression nests too deeply");
      }
    }
    ~DepthGuard() {
      --self->depth_;
    }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    // No trailing underscore: this member is public, and the suffix is the
    // project's convention for *private* state.
    Parser* self;
  };
  friend struct DepthGuard;

  std::span<const PPToken> tokens_;
  const ConstExprOptions& options_;
  std::size_t index_ = 0;
  std::size_t depth_ = 0;
  // Nonzero while evaluating a branch whose errors must be discarded.
  int suppressed_ = 0;
  bool failed_ = false;
  ConstExprResult result_;
};

} // namespace

ConstExprResult evaluateConstExpr(std::span<const PPToken> tokens,
                                  const ConstExprOptions& options) {
  Parser parser(tokens, options);
  return parser.run();
}

} // namespace minc::pp
