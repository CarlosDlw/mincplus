// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "lex/lexer.h"
#include "lex/token_kind.h"
#include "lex/token_stream.h"

namespace minc::lex {
namespace {

// The lexer is a pure function of (text, offset), so a test needs no context at
// all: no session, no diagnostics, no files.
Token lexFirst(std::string_view text) {
  return lexOne(text, 0);
}

std::string_view spellingOf(std::string_view text, Token token) {
  return text.substr(token.offset, token.length);
}

// Non-trivia kinds in source order. This is what the parser will see.
std::vector<TokenKind> significantKinds(std::string_view text) {
  const TokenStream stream = TokenStream::lex(0, text);
  std::vector<TokenKind> kinds;
  for (const std::uint32_t index : stream.significantIndices()) {
    kinds.push_back(stream.tokens()[index].kind);
  }
  return kinds;
}

TEST(LexerTest, EmptyInputIsASingleEndOfFile) {
  const Token token = lexFirst("");
  EXPECT_EQ(token.kind, TokenKind::EndOfFile);
  EXPECT_EQ(token.offset, 0u);
  EXPECT_EQ(token.length, 0u);
  EXPECT_FALSE(token.hasAnyFlag());
}

TEST(LexerTest, PastTheEndIsStillEndOfFile) {
  const Token token = lexOne("abc", 99);
  EXPECT_EQ(token.kind, TokenKind::EndOfFile);
  EXPECT_EQ(token.length, 0u);
}

TEST(LexerTest, IdentifiersAndKeywords) {
  EXPECT_EQ(lexFirst("fn").kind, TokenKind::KwFn);
  EXPECT_EQ(lexFirst("let").kind, TokenKind::KwLet);
  EXPECT_EQ(lexFirst("const").kind, TokenKind::KwConst);
  EXPECT_EQ(lexFirst("return").kind, TokenKind::KwReturn);

  // Near misses must stay identifiers: the keyword table is an exact match.
  EXPECT_EQ(lexFirst("fnord").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("lets").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("Returns").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("_let").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("i32").kind, TokenKind::Identifier); // type names are not keywords yet
}

TEST(LexerTest, ControlFlowKeywordsAreClassifiedOnce) {
  // The five that head a control-flow statement, and the two facts the parser
  // depends on: each lexes to its own kind, and the classification is a list the
  // lexer owns rather than a list the parser repeats.
  const struct {
    const char* text;
    TokenKind kind;
  } cases[] = {
      {"if", TokenKind::KwIf},       {"while", TokenKind::KwWhile},       {"for", TokenKind::KwFor},
      {"break", TokenKind::KwBreak}, {"continue", TokenKind::KwContinue},
  };
  for (const auto& testCase : cases) {
    const Token token = lexFirst(testCase.text);
    EXPECT_EQ(token.kind, testCase.kind) << testCase.text;
    EXPECT_TRUE(isKeyword(token.kind)) << testCase.text;
    EXPECT_TRUE(isControlFlowKeyword(token.kind)) << testCase.text;
  }

  // `else` is a keyword but heads nothing: it continues an `if`, so the parser
  // must not treat it as the start of a statement.
  EXPECT_EQ(lexFirst("else").kind, TokenKind::KwElse);
  EXPECT_TRUE(isKeyword(TokenKind::KwElse));
  EXPECT_FALSE(isControlFlowKeyword(TokenKind::KwElse));
  // ... and the same for the declaration words, which the statement dispatch
  // handles itself. If these ever came back true, `isStatementStart` would be
  // answering a different question than the parser asks it.
  EXPECT_FALSE(isControlFlowKeyword(TokenKind::KwFn));
  EXPECT_FALSE(isControlFlowKeyword(TokenKind::KwLet));
  EXPECT_FALSE(isControlFlowKeyword(TokenKind::KwConst));
  EXPECT_FALSE(isControlFlowKeyword(TokenKind::KwReturn));
  EXPECT_FALSE(isControlFlowKeyword(TokenKind::Identifier));
}

TEST(LexerTest, KeywordsNearMissesOfTheNewOnesStayIdentifiers) {
  EXPECT_EQ(lexFirst("iff").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("If").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("For").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("breaks").kind, TokenKind::Identifier);
  EXPECT_EQ(lexFirst("continu").kind, TokenKind::Identifier);
}

TEST(LexerTest, IdentifierConsumesTrailingDigitsAndUnderscores) {
  const std::string_view text = "a_1_b";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::Identifier);
  EXPECT_EQ(spellingOf(text, token), text);
}

TEST(LexerTest, EveryKeywordInTheTableLexesToItsKind) {
  for (const Keyword& keyword : keywords()) {
    const Token token = lexFirst(keyword.text);
    EXPECT_EQ(token.kind, keyword.kind) << keyword.text;
    EXPECT_EQ(token.length, keyword.text.size()) << keyword.text;
    EXPECT_EQ(keywordFromText(keyword.text), keyword.kind) << keyword.text;
  }
}

TEST(LexerTest, KeywordLookupIsCaseSensitive) {
  EXPECT_FALSE(keywordFromText("Fn").has_value());
  EXPECT_FALSE(keywordFromText("").has_value());
}

// Every punctuator spelling, one row each. Longest match is what makes the
// multi-character rows work, and this table is the spec for the dispatch.
TEST(LexerTest, EveryPunctuatorSpelling) {
  const struct {
    std::string_view spelling;
    TokenKind kind;
  } kCases[] = {
      {"<<=", TokenKind::LessLessEqual},
      {">>=", TokenKind::GreaterGreaterEqual},
      {"##", TokenKind::HashHash},
      {"#", TokenKind::Hash},
      {"->", TokenKind::Arrow},
      {"++", TokenKind::PlusPlus},
      {"--", TokenKind::MinusMinus},
      {"<<", TokenKind::LessLess},
      {">>", TokenKind::GreaterGreater},
      {"<=", TokenKind::LessEqual},
      {">=", TokenKind::GreaterEqual},
      {"==", TokenKind::EqualEqual},
      {"!=", TokenKind::BangEqual},
      {"&&", TokenKind::AmpAmp},
      {"||", TokenKind::PipePipe},
      {"+=", TokenKind::PlusEqual},
      {"-=", TokenKind::MinusEqual},
      {"*=", TokenKind::StarEqual},
      {"/=", TokenKind::SlashEqual},
      {"%=", TokenKind::PercentEqual},
      {"&=", TokenKind::AmpEqual},
      {"|=", TokenKind::PipeEqual},
      {"^=", TokenKind::CaretEqual},
      {"(", TokenKind::LParen},
      {")", TokenKind::RParen},
      {"{", TokenKind::LBrace},
      {"}", TokenKind::RBrace},
      {"[", TokenKind::LBracket},
      {"]", TokenKind::RBracket},
      {";", TokenKind::Semicolon},
      {",", TokenKind::Comma},
      {":", TokenKind::Colon},
      {"?", TokenKind::Question},
      {".", TokenKind::Dot},
      {"+", TokenKind::Plus},
      {"-", TokenKind::Minus},
      {"*", TokenKind::Star},
      {"/", TokenKind::Slash},
      {"%", TokenKind::Percent},
      {"&", TokenKind::Amp},
      {"|", TokenKind::Pipe},
      {"^", TokenKind::Caret},
      {"~", TokenKind::Tilde},
      {"!", TokenKind::Bang},
      {"<", TokenKind::Less},
      {">", TokenKind::Greater},
      {"=", TokenKind::Equal},
  };

  for (const auto& testCase : kCases) {
    const Token token = lexFirst(testCase.spelling);
    EXPECT_EQ(token.kind, testCase.kind) << testCase.spelling;
    EXPECT_EQ(token.length, testCase.spelling.size()) << testCase.spelling;
  }
}

TEST(LexerTest, LongestMatchDoesNotOverreach) {
  // `<==` is `<=` then `=`, never `<<` or `<=` plus something longer.
  const std::string_view text = "<==";
  const Token first = lexOne(text, 0);
  EXPECT_EQ(first.kind, TokenKind::LessEqual);
  EXPECT_EQ(first.length, 2u);
  EXPECT_EQ(lexOne(text, first.end()).kind, TokenKind::Equal);

  // A lone `>` at the very end of the buffer must not read past it.
  EXPECT_EQ(lexOne(">", 0).kind, TokenKind::Greater);
  EXPECT_EQ(lexOne(">=", 0).kind, TokenKind::GreaterEqual);
}

TEST(LexerTest, PunctuatorAtEndOfBufferIsNotTruncated) {
  EXPECT_EQ(lexOne("-", 0).kind, TokenKind::Minus);
  EXPECT_EQ(lexOne("<<", 0).kind, TokenKind::LessLess);
  EXPECT_EQ(lexOne("<<=", 0).kind, TokenKind::LessLessEqual);
}

TEST(LexerTest, DecimalIntegers) {
  const struct {
    std::string_view text;
    TokenKind kind;
  } kCases[] = {
      {"0", TokenKind::IntegerLiteral},
      {"42", TokenKind::IntegerLiteral},
      {"1234567890", TokenKind::IntegerLiteral},
      // A leading zero is *not* octal: `.mx` spells octal `0o`.
      {"010", TokenKind::IntegerLiteral},
      {"0o17", TokenKind::IntegerLiteral},
      {"0b1010", TokenKind::IntegerLiteral},
      {"0xFF", TokenKind::IntegerLiteral},
      {"0Xab", TokenKind::IntegerLiteral},
      {"1.5", TokenKind::FloatLiteral},
      {".5", TokenKind::FloatLiteral},
      {"1e5", TokenKind::FloatLiteral},
      {"1e+5", TokenKind::FloatLiteral},
      {"1.5e-3", TokenKind::FloatLiteral},
      {"0x1.8p3", TokenKind::FloatLiteral},
      {"0x1p-2", TokenKind::FloatLiteral},
  };

  for (const auto& testCase : kCases) {
    const Token token = lexFirst(testCase.text);
    EXPECT_EQ(token.kind, testCase.kind) << testCase.text;
    EXPECT_EQ(spellingOf(testCase.text, token), testCase.text) << testCase.text;
    EXPECT_FALSE(token.hasAnyFlag()) << testCase.text;
  }
}

TEST(LexerTest, TheSecondDotOfARangeIsNotAFraction) {
  // `.5` is a number, and a `.` followed by a digit is a fraction -- except when
  // the character *before* the dot is another dot, because then the reader wrote
  // the range operator the language reserves for slices. One character of
  // lookbehind, and it is what makes `a[1..2]` reach the parser as `1`, `.`, `.`,
  // `2` instead of `1`, `.`, `.2` -- which is the difference between the parser
  // being able to say "`..` is reserved" and it saying "expected `]`".
  EXPECT_EQ(
      significantKinds("a[1..2]"),
      (std::vector<TokenKind>{TokenKind::Identifier, TokenKind::LBracket, TokenKind::IntegerLiteral,
                              TokenKind::Dot, TokenKind::Dot, TokenKind::IntegerLiteral,
                              TokenKind::RBracket, TokenKind::EndOfFile}));
  // And the two forms that *are* numbers, unchanged by the lookbehind.
  const std::string_view text = ".5 1.5";
  EXPECT_EQ(significantKinds(text),
            (std::vector<TokenKind>{TokenKind::FloatLiteral, TokenKind::FloatLiteral,
                                    TokenKind::EndOfFile}));
  EXPECT_EQ(spellingOf(text, lexFirst(".5")), ".5");
}

TEST(LexerTest, BasePrefixWithoutDigitsIsFlaggedNotSwallowed) {
  const std::string_view text = "0x";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(token.length, 2u);
  EXPECT_TRUE(token.has(TokenFlag::MissingDigits));

  const std::string_view binary = "0b";
  EXPECT_TRUE(lexFirst(binary).has(TokenFlag::MissingDigits));
}

TEST(LexerTest, NumberStopsBeforeAnIdentifier) {
  // The prefix is consumed but the following identifier is a separate token, so
  // recovery reads better than swallowing the whole run.
  const std::string_view hex = "0xG";
  const Token number = lexFirst(hex);
  EXPECT_EQ(spellingOf(hex, number), "0x");
  EXPECT_TRUE(number.has(TokenFlag::MissingDigits));
  EXPECT_EQ(lexOne(hex, number.end()).kind, TokenKind::Identifier);

  // `1else` is `1` then `else`: an exponent needs digits. The suffix is a
  // keyword rather than an identifier, which is the point -- the number ends at
  // the first byte that cannot continue it, whatever that byte begins.
  const std::string_view word = "1else";
  const Token one = lexFirst(word);
  EXPECT_EQ(one.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(one.length, 1u);
  EXPECT_EQ(lexOne(word, one.end()).kind, TokenKind::KwElse);
}

TEST(LexerTest, ExponentNeedsDigits) {
  const std::string_view text = "1e";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(token.length, 1u);
  EXPECT_EQ(lexOne(text, token.end()).kind, TokenKind::Identifier);
}

TEST(LexerTest, DotWithoutADigitIsPunctuation) {
  // `1.` and `obj.field` stay separate tokens; a float needs a digit after the
  // dot, which is what keeps member access unambiguous.
  const std::string_view text = "1.";
  const Token one = lexFirst(text);
  EXPECT_EQ(one.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(one.length, 1u);
  EXPECT_EQ(lexOne(text, one.end()).kind, TokenKind::Dot);
}

TEST(LexerTest, StringLiteral) {
  const std::string_view text = "\"hi\"";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::StringLiteral);
  EXPECT_EQ(token.length, 4u);
  EXPECT_FALSE(token.hasAnyFlag());

  EXPECT_FALSE(lexFirst("\"\"").hasAnyFlag());
}

TEST(LexerTest, StringLiteralDoesNotCrossALine) {
  for (const std::string_view text :
       {std::string_view("\"abc\n"), std::string_view("\"abc\r\n"), std::string_view("\"abc")}) {
    const Token token = lexFirst(text);
    EXPECT_EQ(token.kind, TokenKind::StringLiteral);
    EXPECT_TRUE(token.has(TokenFlag::UnterminatedString)) << text.size();
    EXPECT_EQ(token.length, 4u);
  }
}

TEST(LexerTest, TrailingBackslashLeavesTheLiteralUnterminated) {
  const std::string_view text = "\"a\\";
  const Token token = lexFirst(text);
  EXPECT_TRUE(token.has(TokenFlag::UnterminatedString));
  EXPECT_EQ(token.length, text.size());
}

TEST(LexerTest, EscapedQuoteDoesNotEndTheLiteral) {
  const std::string_view text = "\"a\\\"b\"";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::StringLiteral);
  EXPECT_EQ(token.length, text.size());
  EXPECT_FALSE(token.hasAnyFlag());
}

TEST(LexerTest, CharacterLiteral) {
  EXPECT_EQ(lexFirst("'a'").kind, TokenKind::CharLiteral);
  EXPECT_FALSE(lexFirst("'a'").hasAnyFlag());
  EXPECT_FALSE(lexFirst("'\\n'").hasAnyFlag());
  EXPECT_FALSE(lexFirst("'\\x41'").hasAnyFlag());
  EXPECT_FALSE(lexFirst("'\\101'").hasAnyFlag());
  // Multi-character literals are a type question, not a lexical one.
  EXPECT_FALSE(lexFirst("'ab'").hasAnyFlag());
}

TEST(LexerTest, EmptyCharacterLiteralIsFlagged) {
  const Token token = lexFirst("''");
  EXPECT_EQ(token.kind, TokenKind::CharLiteral);
  EXPECT_EQ(token.length, 2u);
  EXPECT_TRUE(token.has(TokenFlag::EmptyCharLiteral));
}

TEST(LexerTest, UnknownAndShortEscapes) {
  EXPECT_TRUE(lexFirst("\"\\q\"").has(TokenFlag::UnknownEscape));
  // An escape that wants digits and has none is its own flag, because the sentence
  // is about the escape and not about a numeric prefix that is missing:
  // `lex-escape-digits` names both the braced form and the exact widths.
  EXPECT_TRUE(lexFirst("\"\\x\"").has(TokenFlag::EscapeDigits));
  EXPECT_TRUE(lexFirst("\"\\o\"").has(TokenFlag::EscapeDigits));
  EXPECT_TRUE(lexFirst("\"\\u12\"").has(TokenFlag::EscapeDigits));
  EXPECT_TRUE(lexFirst("\"\\U1\"").has(TokenFlag::EscapeDigits));
  // A braced escape needs digits between its braces, and both braces.
  EXPECT_TRUE(lexFirst("\"\\x{}\"").has(TokenFlag::EscapeDigits));
  EXPECT_TRUE(lexFirst("\"\\x{41\"").has(TokenFlag::EscapeDigits));
  EXPECT_TRUE(lexFirst("\"\\u{}\"").has(TokenFlag::EscapeDigits));
  EXPECT_FALSE(lexFirst("\"\\u0041\"").hasAnyFlag());
  EXPECT_FALSE(lexFirst("\"\\U0001F600\"").hasAnyFlag());
}

// The whole alphabet of `literals.md`, as tokens: every row the language has is
// one escape and one token, and the delimited forms put the digits' end where the
// braces are.
TEST(LexerTest, TheWholeEscapeAlphabet) {
  for (const std::string_view spelling :
       {"\"\\a\"",       "\"\\b\"",      "\"\\e\"",     "\"\\f\"",     "\"\\n\"",
        "\"\\r\"",       "\"\\t\"",      "\"\\v\"",     "\"\\?\"",     "\"\\'\"",
        "\"\\\\\"",      "\"\\101\"",    "\"\\377\"",   "\"\\xFF\"",   "\"\\x{41}\"",
        "\"\\o{101}\"",  "\"\\o{377}\"", "\"\\u0041\"", "\"\\u{e9}\"", "\"\\U0001F600\"",
        "\"\\U{1F600}\""}) {
    const Token token = lexFirst(spelling);
    EXPECT_EQ(token.kind, TokenKind::StringLiteral) << spelling;
    EXPECT_FALSE(token.hasAnyFlag()) << spelling;
    EXPECT_EQ(token.length, spelling.size()) << spelling;
  }
}

// An escape above a byte has two different owners, and they are not the same
// stage: a `str` has no byte for it, so the scanner refuses it (nothing later
// reads a string's escapes); a `char` is a *type* that cannot hold it, so the
// checker states that rule, and flagging it here as well would put two sentences
// on one mistake and give the wrong advice (`literals.md`, decision 23).
TEST(LexerTest, AnEscapeWiderThanAByteIsAStringsRefusal) {
  EXPECT_TRUE(lexFirst("\"\\x1FF\"").has(TokenFlag::EscapeTooWide));
  EXPECT_TRUE(lexFirst("\"\\400\"").has(TokenFlag::EscapeTooWide));
  EXPECT_TRUE(lexFirst("\"\\x{1F600}\"").has(TokenFlag::EscapeTooWide));
  EXPECT_FALSE(lexFirst("\"\\x{FF}\"").has(TokenFlag::EscapeTooWide));
  // A code point in a string is a *character*, and its bytes are its UTF-8
  // encoding: nothing is above a byte there.
  EXPECT_FALSE(lexFirst("\"\\u{1F600}\"").has(TokenFlag::EscapeTooWide));
  // And a character literal's width is not a lexical question at all.
  EXPECT_FALSE(lexFirst("'\\u{1F600}'").hasAnyFlag());
  EXPECT_FALSE(lexFirst("'\\x{1F600}'").hasAnyFlag());
  EXPECT_FALSE(lexFirst("'\\400'").hasAnyFlag());
  EXPECT_FALSE(lexFirst("'\\xFF'").hasAnyFlag());
}

// C++23's named escape is *known* and refused by name, so the sentence can say
// what to write instead of calling it an unknown escape.
TEST(LexerTest, TheNamedEscapeIsRefusedByItsOwnName) {
  const std::string_view text = "\"\\N{GREEK SMALL LETTER ALPHA}\"";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::StringLiteral);
  EXPECT_TRUE(token.has(TokenFlag::NamedEscape));
  // The whole name is consumed: the literal still ends where it should.
  EXPECT_EQ(token.length, text.size());
}

// `\` before a line ending continues the literal and contributes nothing. LF and
// CRLF are one rule, because the source may come from either kind of machine.
TEST(LexerTest, ABackslashContinuesTheLine) {
  for (const std::string_view text : {"\"a\\\nb\"", "\"a\\\r\nb\"", "'\\\na'"}) {
    const Token token = lexFirst(text);
    EXPECT_FALSE(token.hasAnyFlag()) << text.size();
    EXPECT_EQ(token.length, text.size()) << text.size();
  }
}

// The digit separators are *spelling*: claimed into the token, removed before the
// value is read, and legal on both sides of a point or an exponent.
TEST(LexerTest, DigitSeparatorsArePartOfTheToken) {
  for (const std::string_view spelling :
       {"1_000", "1'000", "0xFE'DC'BA'98", "0b1111_0000", "0o755_000", "1_000.5", "1.414'213'562",
        "1e1_0", "0xF_Fp1_0"}) {
    const Token token = lexFirst(spelling);
    EXPECT_FALSE(token.hasAnyFlag()) << spelling;
    EXPECT_EQ(token.length, spelling.size()) << spelling;
  }
}

// A separator that is not between two digits is one token with one flag -- not a
// number and a name, which would get the sentence about the wrong mistake.
TEST(LexerTest, AMisplacedSeparatorIsFlagged) {
  for (const std::string_view spelling : {"1000_", "1__0", "0x_FF", "10_u8", "1e1_", "1.5_f32"}) {
    EXPECT_TRUE(lexFirst(spelling).has(TokenFlag::MisplacedSeparator)) << spelling;
  }
  EXPECT_FALSE(lexFirst("1_000").has(TokenFlag::MisplacedSeparator));
}

// A quote only separates when a digit of the same number follows it, so `1'a'` is
// still a number and a character literal: the spelling has no other reading, and a
// lexer that cannot be surprised by the source is the point.
TEST(LexerTest, AQuoteOnlySeparatesTwoDigits) {
  const std::string_view text = "1'a'";
  const Token number = lexFirst(text);
  EXPECT_EQ(number.kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(number.length, 1u);
  EXPECT_FALSE(number.hasAnyFlag());
  EXPECT_EQ(lexOne(text, number.end()).kind, TokenKind::CharLiteral);
}

// `5.` is `5` and a `.`: a trailing point is the one spelling whose meaning would
// change if the language grew member access (`literals.md`, decision 8).
TEST(LexerTest, ATrailingPointIsNotAFloat) {
  EXPECT_EQ(lexFirst("5.").kind, TokenKind::IntegerLiteral);
  EXPECT_EQ(lexFirst("5.").length, 1u);
  EXPECT_EQ(lexFirst(".5").kind, TokenKind::FloatLiteral);
  EXPECT_EQ(lexFirst(".5").length, 2u);
}

// A fraction with no integer part is a spelling and not a shape: everything a
// float may have after its digits -- an exponent, a suffix -- belongs to it too,
// or `.5e3` would be the same number written in a shape the language refused.
TEST(LexerTest, AFractionWithNoIntegerPartKeepsTheRestOfTheGrammar) {
  for (const std::string_view spelling : {".5e3", ".5e-2", ".5E3", ".5f32", ".5L", ".5_0"}) {
    const Token token = lexFirst(spelling);
    EXPECT_EQ(token.kind, TokenKind::FloatLiteral) << spelling;
    EXPECT_EQ(token.length, spelling.size()) << spelling;
    EXPECT_FALSE(token.hasAnyFlag()) << spelling;
  }
  // And the name that follows a *complete* number is still a name: `1else` is `1`
  // and `else`, because an exponent is only one when digits follow it.
  EXPECT_EQ(lexFirst("1else").length, 1u);
  EXPECT_EQ(lexFirst("1e").length, 1u);
}

TEST(LexerTest, HexadecimalFloatsWithoutAnExponentOrAnIntegerPart) {
  EXPECT_EQ(lexFirst("0x1.8p3").kind, TokenKind::FloatLiteral);
  EXPECT_EQ(lexFirst("0x1.8p3").length, 7u);
  EXPECT_EQ(lexFirst("0x.8p3").kind, TokenKind::FloatLiteral);
  EXPECT_EQ(lexFirst("0x.8p3").length, 6u);
  // No exponent is needed here: the point and a hex digit after it are enough.
  EXPECT_EQ(lexFirst("0x1.8").kind, TokenKind::FloatLiteral);
  EXPECT_EQ(lexFirst("0x1.8").length, 5u);
}

// The arity of `\u`/`\U` was checked but not the value, which is half a check:
// the digits can all be there and still name something that cannot be encoded.
TEST(LexerTest, UnicodeEscapeMustBeAScalarValue) {
  EXPECT_FALSE(lexFirst("\"\\u00e9\"").has(TokenFlag::InvalidEscapeValue));
  EXPECT_FALSE(lexFirst("\"\\uD7FF\"").has(TokenFlag::InvalidEscapeValue));
  EXPECT_FALSE(lexFirst("\"\\uE000\"").has(TokenFlag::InvalidEscapeValue));
  EXPECT_FALSE(lexFirst("\"\\U0010FFFF\"").has(TokenFlag::InvalidEscapeValue));

  EXPECT_TRUE(lexFirst("\"\\uD800\"").has(TokenFlag::InvalidEscapeValue));
  EXPECT_TRUE(lexFirst("\"\\uDFFF\"").has(TokenFlag::InvalidEscapeValue));
  EXPECT_TRUE(lexFirst("\"\\U00110000\"").has(TokenFlag::InvalidEscapeValue));
  EXPECT_TRUE(lexFirst("\"\\UFFFFFFFF\"").has(TokenFlag::InvalidEscapeValue));

  // A short escape is missing digits, not out of range: only one thing is
  // wrong with it, and it is reported once.
  const Token shortEscape = lexFirst("\"\\u12\"");
  EXPECT_TRUE(shortEscape.has(TokenFlag::EscapeDigits));
  EXPECT_FALSE(shortEscape.has(TokenFlag::InvalidEscapeValue));
}

TEST(LexerTest, Comments) {
  const std::string_view line = "// c\nx";
  const Token comment = lexFirst(line);
  EXPECT_EQ(comment.kind, TokenKind::LineComment);
  EXPECT_EQ(comment.length, 4u); // no newline inside
  EXPECT_EQ(lexOne(line, comment.end()).kind, TokenKind::Newline);

  const std::string_view crlf = "// c\r\n";
  EXPECT_EQ(lexFirst(crlf).length, 4u); // stops before CR so the terminator stays intact

  const std::string_view block = "/* a */b";
  const Token blockToken = lexFirst(block);
  EXPECT_EQ(blockToken.kind, TokenKind::BlockComment);
  EXPECT_EQ(blockToken.length, 7u);
  EXPECT_FALSE(blockToken.hasAnyFlag());
  EXPECT_EQ(lexOne(block, blockToken.end()).kind, TokenKind::Identifier);

  // Block comments take a newline with them: they are one token, not a line.
  const std::string_view multiline = "/* a\nb */";
  EXPECT_EQ(lexFirst(multiline).length, multiline.size());
}

TEST(LexerTest, UnterminatedBlockComment) {
  for (const std::string_view text :
       {std::string_view("/*"), std::string_view("/*/"), std::string_view("/* a")}) {
    const Token token = lexFirst(text);
    EXPECT_EQ(token.kind, TokenKind::BlockComment);
    EXPECT_TRUE(token.has(TokenFlag::UnterminatedBlockComment));
    EXPECT_EQ(token.length, text.size());
  }
  // `/**/` closes on the first two characters.
  EXPECT_FALSE(lexFirst("/**/").hasAnyFlag());
}

TEST(LexerTest, SlashIsNotAlwaysAComment) {
  EXPECT_EQ(lexFirst("/").kind, TokenKind::Slash);
  EXPECT_EQ(lexFirst("/=").kind, TokenKind::SlashEqual);
  EXPECT_EQ(lexFirst("/ 2").kind, TokenKind::Slash);
}

TEST(LexerTest, WhitespaceAndNewlines) {
  EXPECT_EQ(lexFirst(" \t\v\f").kind, TokenKind::Whitespace);
  EXPECT_EQ(lexFirst(" \t\v\f").length, 4u);

  const Token lf = lexFirst("\n");
  EXPECT_EQ(lf.kind, TokenKind::Newline);
  EXPECT_EQ(lf.length, 1u);

  // CRLF is one terminator owned by one token.
  const Token crlf = lexFirst("\r\n");
  EXPECT_EQ(crlf.kind, TokenKind::Newline);
  EXPECT_EQ(crlf.length, 2u);

  // Classic Mac endings: a lone CR is still one line break.
  const Token cr = lexFirst("\r");
  EXPECT_EQ(cr.kind, TokenKind::Newline);
  EXPECT_EQ(cr.length, 1u);
}

TEST(LexerTest, InvalidBytes) {
  EXPECT_EQ(lexFirst("@").kind, TokenKind::Invalid);
  EXPECT_EQ(lexFirst("`").kind, TokenKind::Invalid);
  EXPECT_EQ(lexFirst("\x01").kind, TokenKind::Invalid);
}

TEST(LexerTest, PreprocessorOperatorsAreTokens) {
  // `#` and `##` are punctuators, and longest match applies to them like any
  // other: `# #` is two spellings and `##` is one. The preprocessor's whole
  // notion of a paste operator rests on that distinction, so it is pinned here
  // rather than discovered downstream.
  const std::string_view spaced = "# #";
  EXPECT_EQ(lexOne(spaced, 0).kind, TokenKind::Hash);
  EXPECT_EQ(lexOne(spaced, 0).length, 1u);
  EXPECT_EQ(lexOne(spaced, 2).kind, TokenKind::Hash);
  EXPECT_EQ(lexFirst("##").kind, TokenKind::HashHash);
  EXPECT_EQ(lexFirst("##").length, 2u);

  // A lone `#` is a token, not an error, so a file full of directives lexes
  // cleanly. Whether the `#` means anything is decided later, by position.
  EXPECT_EQ(lexFirst("#define X 1\n").kind, TokenKind::Hash);
}

TEST(LexerTest, PreprocessorOperatorsAreTheirOwnCategory) {
  EXPECT_TRUE(isPreprocessorOp(TokenKind::Hash));
  EXPECT_TRUE(isPreprocessorOp(TokenKind::HashHash));
  EXPECT_FALSE(isPreprocessorOp(TokenKind::Plus));
  EXPECT_FALSE(isPreprocessorOp(TokenKind::Invalid));
}

TEST(LexerTest, NonAsciiCharacterIsOneToken) {
  // "é" is two bytes and must not be split into two Invalid tokens, or the
  // caret would underline only half the character.
  const std::string_view text = "\xC3\xA9";
  const Token token = lexFirst(text);
  EXPECT_EQ(token.kind, TokenKind::Invalid);
  EXPECT_EQ(token.length, 2u);

  // Robust even for input that is not valid UTF-8 at all.
  EXPECT_EQ(lexOne("\xC3\xC3", 0).length, 1u);
}

TEST(LexerTest, ThreeDotsAreOneTokenAndSpacedOnesAreThree) {
  // The variadic marker, and the same spelling a variadic *macro* uses. The rule
  // is one token kind here and nowhere else: the preprocessor used to rebuild it
  // from three adjacent `Dot`s, and `. . .` was never an ellipsis to either
  // reader -- so the rule was stated twice and agreed by luck.
  const Token marker = lexFirst("...");
  EXPECT_EQ(marker.kind, TokenKind::Ellipsis);
  EXPECT_EQ(marker.length, 3u);
  EXPECT_EQ(spellingOf("...", marker), "...");

  EXPECT_EQ(
      significantKinds("a..."),
      (std::vector<TokenKind>{TokenKind::Identifier, TokenKind::Ellipsis, TokenKind::EndOfFile}));
  // A spaced form is three tokens, and a fourth dot is a second token: longest
  // match, the same rule every punctuator follows.
  EXPECT_EQ(significantKinds(". . ."),
            (std::vector<TokenKind>{TokenKind::Dot, TokenKind::Dot, TokenKind::Dot,
                                    TokenKind::EndOfFile}));
  EXPECT_EQ(significantKinds("...."),
            (std::vector<TokenKind>{TokenKind::Ellipsis, TokenKind::Dot, TokenKind::EndOfFile}));
  // A leading dot is still a number, which is why the marker cannot be confused
  // with one: `.5` is scanned before the punctuator table is consulted.
  EXPECT_EQ(lexFirst(".5").kind, TokenKind::FloatLiteral);
}

TEST(LexerTest, EveryTokenAdvances) {
  // The one property a caller looping on lexOne depends on: progress.
  const std::string_view text = "fn i32 main() { let x = 0x; }/*\"'\\\xC3\xA9 \xC3";
  for (std::uint32_t offset = 0; offset < text.size();) {
    const Token token = lexOne(text, offset);
    ASSERT_NE(token.kind, TokenKind::EndOfFile) << offset;
    ASSERT_GT(token.length, 0u) << offset;
    EXPECT_EQ(token.offset, offset);
    offset = token.end();
  }
  // ...and it always terminates exactly at the end of the input.
  EXPECT_EQ(lexOne(text, static_cast<std::uint32_t>(text.size())).kind, TokenKind::EndOfFile);
}

TEST(LexerTest, SignificantKindsSkipTrivia) {
  const std::vector<TokenKind> kinds = significantKinds("let x: i32 = 1 + 2;\n");
  const std::vector<TokenKind> expected = {
      TokenKind::KwLet,     TokenKind::Identifier,     TokenKind::Colon, TokenKind::Identifier,
      TokenKind::Equal,     TokenKind::IntegerLiteral, TokenKind::Plus,  TokenKind::IntegerLiteral,
      TokenKind::Semicolon, TokenKind::EndOfFile,
  };
  EXPECT_EQ(kinds, expected);
}

} // namespace
} // namespace minc::lex
