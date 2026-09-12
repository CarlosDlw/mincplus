// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "pp/const_expr.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace minc::pp {
namespace {

constexpr std::uint64_t kUint64Max = ~std::uint64_t{0};

// Value of one hex digit, or -1.
[[nodiscard]] int hexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

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
      left = fold(left, right, left.bits | right.bits);
    }
    return left;
  }

  ConstInt parseBitXor() {
    ConstInt left = parseBitAnd();
    while (!failed_ && peek().is(lex::TokenKind::Caret)) {
      take();
      const ConstInt right = parseBitAnd();
      left = fold(left, right, left.bits ^ right.bits);
    }
    return left;
  }

  ConstInt parseBitAnd() {
    ConstInt left = parseEquality();
    while (!failed_ && peek().is(lex::TokenKind::Amp)) {
      take();
      const ConstInt right = parseEquality();
      left = fold(left, right, left.bits & right.bits);
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
      left = ConstInt::fromSigned(compare(kind, left, right) ? 1 : 0);
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
      if (right.bits >= 64) {
        fail(spanOf(op), "shift count " + std::to_string(right.bits) + " is out of range");
        return ConstInt{};
      }
      const std::uint64_t count = right.bits;
      const std::uint64_t bits =
          kind == lex::TokenKind::LessLess ? (left.bits << count) : (left.bits >> count);
      // The result of `<<`/`>>` has the promoted type of the left operand.
      left = ConstInt{bits, left.isUnsigned};
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
      const std::uint64_t bits =
          kind == lex::TokenKind::Plus ? left.bits + right.bits : left.bits - right.bits;
      left = fold(left, right, bits);
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
        left = fold(left, right, left.bits * right.bits);
        break;
      case lex::TokenKind::Slash:
        if (right.bits == 0) {
          fail(spanOf(op), "division by zero in #if expression");
          return ConstInt{};
        }
        left = fold(left, right, left.bits / right.bits);
        break;
      default:
        if (right.bits == 0) {
          fail(spanOf(op), "remainder by zero in #if expression");
          return ConstInt{};
        }
        left = fold(left, right, left.bits % right.bits);
        break;
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
        return ConstInt{0U - operand.bits, operand.isUnsigned};
      case lex::TokenKind::Plus:
        return operand;
      case lex::TokenKind::Bang:
        return ConstInt::fromSigned(operand.truthy() ? 0 : 1);
      default:
        return ConstInt{~operand.bits, operand.isUnsigned};
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

  // The operand types of a binary operator: unsigned when either side is.
  [[nodiscard]] static ConstInt fold(const ConstInt& left, const ConstInt& right,
                                     std::uint64_t bits) {
    return ConstInt{bits, left.isUnsigned || right.isUnsigned};
  }

  [[nodiscard]] static bool compare(lex::TokenKind kind, const ConstInt& left,
                                    const ConstInt& right) {
    if (left.isUnsigned || right.isUnsigned) {
      switch (kind) {
      case lex::TokenKind::Less:
        return left.bits < right.bits;
      case lex::TokenKind::Greater:
        return left.bits > right.bits;
      case lex::TokenKind::LessEqual:
        return left.bits <= right.bits;
      default:
        return left.bits >= right.bits;
      }
    }
    const std::int64_t lhs = left.signedValue();
    const std::int64_t rhs = right.signedValue();
    switch (kind) {
    case lex::TokenKind::Less:
      return lhs < rhs;
    case lex::TokenKind::Greater:
      return lhs > rhs;
    case lex::TokenKind::LessEqual:
      return lhs <= rhs;
    default:
      return lhs >= rhs;
    }
  }

  ConstInt parseIntegerLiteral(const PPToken& token) {
    const std::string_view text = spelling(token);
    if (text.empty()) {
      failToken(token, "expected an integer literal in #if expression");
      return ConstInt{};
    }

    std::size_t index = 0;
    unsigned base = 10;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      base = 16;
      index = 2;
    } else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
      base = 2;
      index = 2;
    } else if (text.size() >= 2 && text[0] == '0') {
      base = 8;
      index = 1;
    }

    std::uint64_t value = 0;
    bool overflowed = false;
    bool anyDigit = false;
    for (; index < text.size(); ++index) {
      const int digit = hexValue(text[index]);
      if (digit < 0 || static_cast<unsigned>(digit) >= base) {
        break;
      }
      anyDigit = true;
      const std::uint64_t digitValue = static_cast<std::uint64_t>(digit);
      // Overflow is decided *before* multiplying, against the exact room left.
      // A "did the value shrink" or `value > max / base` test rejects
      // `0xFFFFFFFFFFFFFFFF`, which fits in 64 bits and is the very literal a
      // `#if` is most likely to compare against.
      if (value > (kUint64Max - digitValue) / base) {
        overflowed = true;
      }
      // Keep consuming digits after the overflow: the suffix scan starts where
      // the digits end, and stopping early would leave `...0bignumULL` half
      // lexed.
      value = value * base + digitValue;
    }
    if (!anyDigit) {
      failToken(token, "malformed integer literal '" + std::string(text) + "' in #if expression");
      return ConstInt{};
    }

    bool isUnsigned = false;
    for (; index < text.size(); ++index) {
      const char suffix = text[index];
      if (suffix == 'u' || suffix == 'U') {
        isUnsigned = true;
      } else if (suffix != 'l' && suffix != 'L') {
        failToken(token, "unknown suffix in integer literal '" + std::string(text) + "'");
        return ConstInt{};
      }
    }
    if (overflowed) {
      failToken(token, "integer literal '" + std::string(text) + "' does not fit in 64 bits");
      return ConstInt{};
    }
    // A decimal literal too large for intmax_t is evaluated as unsigned, which
    // is the standard's behaviour rather than a diagnostic.
    if (!isUnsigned && value > static_cast<std::uint64_t>(INT64_MAX)) {
      isUnsigned = true;
    }
    return ConstInt{value, isUnsigned};
  }

  ConstInt parseCharLiteral(const PPToken& token) {
    const std::string_view text = spelling(token);
    // Strip the quotes the lexer guarantees; the body may be empty or escaped,
    // which the lexer has already flagged as a lexical problem.
    if (text.size() < 2) {
      failToken(token, "malformed character literal in #if expression");
      return ConstInt{};
    }
    std::string_view body = text.substr(1, text.size() - 2);
    std::uint64_t value = 0;
    std::size_t index = 0;
    while (index < body.size()) {
      const auto decoded = decodeChar(body, index);
      if (!decoded.has_value()) {
        failToken(token, "unknown escape in character literal '" + std::string(text) + "'");
        return ConstInt{};
      }
      index = decoded->second;
      // Multi-character literals are implementation-defined; this is the
      // packed value GCC produces, which is the least surprising choice.
      value = (value << 8U) | decoded->first;
    }
    return ConstInt::fromSigned(static_cast<std::int64_t>(value));
  }

  // Decodes the character or escape at `index`, returning (value, next index).
  [[nodiscard]] static std::optional<std::pair<std::uint64_t, std::size_t>>
  decodeChar(std::string_view body, std::size_t index) {
    if (body[index] != '\\') {
      return std::make_pair(static_cast<std::uint64_t>(static_cast<unsigned char>(body[index])),
                            index + 1);
    }
    if (index + 1 >= body.size()) {
      return std::nullopt;
    }
    const char escape = body[index + 1];
    switch (escape) {
    case 'n':
      return std::make_pair(10U, index + 2);
    case 't':
      return std::make_pair(9U, index + 2);
    case 'r':
      return std::make_pair(13U, index + 2);
    case 'a':
      return std::make_pair(7U, index + 2);
    case 'b':
      return std::make_pair(8U, index + 2);
    case 'f':
      return std::make_pair(12U, index + 2);
    case 'v':
      return std::make_pair(11U, index + 2);
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': {
      std::uint64_t value = 0;
      std::size_t next = index + 1;
      std::size_t digits = 0;
      while (next < body.size() && digits < 3 && body[next] >= '0' && body[next] <= '7') {
        value = value * 8U + static_cast<std::uint64_t>(body[next] - '0');
        ++next;
        ++digits;
      }
      return std::make_pair(value, next);
    }
    case 'x':
    case 'X': {
      std::uint64_t value = 0;
      std::size_t next = index + 2;
      std::size_t digits = 0;
      while (next < body.size()) {
        const int digit = hexValue(body[next]);
        if (digit < 0) {
          break;
        }
        value = value * 16U + static_cast<std::uint64_t>(digit);
        ++next;
        ++digits;
      }
      if (digits == 0) {
        return std::nullopt;
      }
      return std::make_pair(value, next);
    }
    case '\\':
    case '\'':
    case '"':
      return std::make_pair(static_cast<std::uint64_t>(static_cast<unsigned char>(escape)),
                            index + 2);
    default:
      return std::nullopt;
    }
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
