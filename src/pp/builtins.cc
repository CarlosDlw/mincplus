// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
//
// The predefined macros.
//
// Two kinds live here. The first are ordinary constants (`__STDC__`, ...): they
// are installed with a body like any other object-like macro, so nothing
// downstream has to know they are special. The second are the ones whose
// replacement depends on *where* they are invoked (`__FILE__`, `__LINE__`,
// `__COUNTER__`, `__DATE__`, `__TIME__`): those are marked with a `BuiltinKind`
// and computed at the invocation, because storing a body for them would store
// the wrong answer.
//
// `__DATE__` and `__TIME__` come from `SOURCE_DATE_EPOCH` and are otherwise an
// *error*. A compiler that silently embeds the current time produces a build
// nobody can reproduce, and reproducible-builds.org exists because that is a
// real, common problem rather than a theoretical one.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "lex/lexer.h"
#include "pp_internal.h"
#include "support/limits.h"

#include "pp/preprocessor.h"

namespace minc::pp {
namespace {

constexpr std::string_view kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Days since 1970-01-01 to a calendar date, by Howard Hinnant's `civil_from_days`
// (public domain, and the reason this needs no `<ctime>` and no timezone, which
// is what makes the answer the same on every platform).
void civilFromDays(std::int64_t days, int& year, int& month, int& day) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t dayOfEra = days - era * 146097;
  const std::int64_t yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const std::int64_t y = yearOfEra + era * 400;
  const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const std::int64_t monthPrime = (5 * dayOfYear + 2) / 153;
  const std::int64_t d = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
  const std::int64_t m = monthPrime + (monthPrime < 10 ? 3 : -9);
  year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  month = static_cast<int>(m);
  day = static_cast<int>(d);
}

// A path or a date inside a string literal: quotes and backslashes escaped so
// the result is a valid literal on every platform (Windows paths are full of
// backslashes).
[[nodiscard]] std::string quoted(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

[[nodiscard]] std::string twoDigits(int value) {
  std::string out;
  out.push_back(static_cast<char>('0' + (value / 10)));
  out.push_back(static_cast<char>('0' + (value % 10)));
  return out;
}

} // namespace

void Preprocessor::installPredefined() {
  const auto constant = [&](std::string_view name, std::string text) {
    MacroInfo info;
    info.name = session_->symbols().intern(name);
    info.kind = MacroKind::ObjectLike;
    const PPToken token = makeScratchToken(lex::TokenKind::IntegerLiteral, std::move(text),
                                           SourceLoc{}, PPTokenFlag::None);
    MacroBodyToken entry;
    entry.token = token;
    entry.spellingOffset = 0;
    entry.spellingLength = token.length;
    info.spellings.assign(spelling(token));
    info.body.push_back(entry);
    macros_.install(std::move(info));
  };
  const auto builtin = [&](std::string_view name, BuiltinKind kind, MacroKind macroKind,
                           std::string_view parameter = {}) {
    MacroInfo info;
    info.name = session_->symbols().intern(name);
    info.kind = macroKind;
    info.builtin = kind;
    // A builtin that takes a parenthesized operand declares the parameter, so
    // the ordinary argument collector hands the operand over; one that finds its
    // own operand (`__has_include`) declares none and is exempted instead.
    if (!parameter.empty()) {
      info.params.push_back(MacroParam{session_->symbols().intern(parameter)});
    }
    macros_.install(std::move(info));
  };

  // The fixed ones. Deliberately short: a compiler that defines hundreds of
  // compatibility macros is claiming to be something it is not.
  constant("__STDC__", "1");
  constant("__STDC_VERSION__", "201710L");
  constant("__STDC_HOSTED__", "1");
  constant("__minc__", "1");

  // The ones that depend on the invocation site.
  builtin("__FILE__", BuiltinKind::File, MacroKind::ObjectLike);
  builtin("__LINE__", BuiltinKind::Line, MacroKind::ObjectLike);
  builtin("__COUNTER__", BuiltinKind::Counter, MacroKind::ObjectLike);
  builtin("__DATE__", BuiltinKind::Date, MacroKind::ObjectLike);
  builtin("__TIME__", BuiltinKind::Time, MacroKind::ObjectLike);

  // `__has_include` is function-like because it takes a parenthesized operand,
  // but it is not *callable*: it exists only inside `#if`, and the expander
  // reports a stray use instead of pretending it has a value.
  builtin("__has_include", BuiltinKind::HasInclude, MacroKind::FunctionLike);

  // `_Pragma` (C99) is the operator form of `#pragma`. Registering it as a
  // function-like builtin rather than special-casing it in the scanner is what
  // makes the macro case work at all: `#define PUSH _Pragma("once")` expands to
  // `_Pragma ( "once" )` in the token stream, and rescanning meets it exactly as
  // it meets a written one. It produces no tokens -- the pragma is an effect,
  // not a value -- so nothing of it reaches the parser.
  builtin("_Pragma", BuiltinKind::Pragma, MacroKind::FunctionLike, "__operand");
}

namespace {

// `_Pragma`'s operand, destringized (C11 6.10.9p1): strip the quotes, then turn
// `\"` into `"` and `\\` into `\`. *Only* those two, and only in that order --
// the string is not a string literal here and none of the other escapes exist,
// which is what lets `_Pragma("once")` and `_Pragma("a\\nb")` mean different
// things. Reading the raw spelling rather than a decoded value is deliberate for
// the same reason.
[[nodiscard]] std::string destringize(std::string_view literal) {
  const std::string_view inner =
      literal.size() >= 2 ? literal.substr(1, literal.size() - 2) : std::string_view{};
  std::string out;
  out.reserve(inner.size());
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] == '\\' && i + 1 < inner.size() && (inner[i + 1] == '"' || inner[i + 1] == '\\')) {
      ++i;
    }
    out.push_back(inner[i]);
  }
  return out;
}

} // namespace

bool Preprocessor::expandBuiltin(const MacroInfo& macro, const PPToken& nameToken,
                                 const std::vector<std::vector<PPToken>>& arguments,
                                 std::vector<PPToken>& out, std::optional<PPError>& error) {
  // The site that matters is the innermost invocation, not where the builtin's
  // own name was written: that is what makes `__LINE__` inside a macro body
  // report the line the macro was *used* on, which is the classic bug.
  const SourceLoc where = contexts_.empty() ? nameToken.loc.spelling : contexts_.back().invocation;
  const SourceLoc caret = where.valid() ? where : nameToken.loc.spelling;

  switch (macro.builtin) {
  case BuiltinKind::File: {
    const std::string_view path =
        fileNameOverride_.has_value() ? std::string_view(*fileNameOverride_) : pathOf(caret.file);
    PPToken token =
        makeScratchToken(lex::TokenKind::StringLiteral, quoted(path), caret, PPTokenFlag::None);
    out.push_back(token);
    return true;
  }
  case BuiltinKind::Line:
    // `currentLine` already applied any `#line` adjustment.
    out.push_back(makeScratchToken(lex::TokenKind::IntegerLiteral,
                                   std::to_string(currentLine(caret)), caret, PPTokenFlag::None));
    return true;
  case BuiltinKind::Counter:
    out.push_back(makeScratchToken(lex::TokenKind::IntegerLiteral, std::to_string(counter_++),
                                   caret, PPTokenFlag::None));
    return true;
  case BuiltinKind::Date:
  case BuiltinKind::Time: {
    if (!options_.sourceDateEpoch.has_value()) {
      error = PPError{caret.span(),
                      "'" + std::string(session_->symbols().lookup(macro.name)) +
                          "' needs SOURCE_DATE_EPOCH to be set: a build that embeds the current "
                          "time cannot be reproduced",
                      PPErrorCode::DateWithoutEpoch};
      return false;
    }
    const std::int64_t seconds = *options_.sourceDateEpoch;
    // Floor division, so a negative epoch lands on the day that contains it
    // rather than the one after.
    std::int64_t days = seconds / 86400;
    std::int64_t remainder = seconds % 86400;
    if (remainder < 0) {
      remainder += 86400;
      --days;
    }
    int year = 1970;
    int month = 1;
    int day = 1;
    civilFromDays(days, year, month, day);
    if (macro.builtin == BuiltinKind::Date) {
      std::string text;
      text.reserve(12);
      text += kMonthNames[static_cast<std::size_t>(month - 1)];
      text.push_back(' ');
      if (day < 10) {
        text.push_back(' ');
      }
      text += std::to_string(day);
      text.push_back(' ');
      text += std::to_string(year);
      out.push_back(
          makeScratchToken(lex::TokenKind::StringLiteral, quoted(text), caret, PPTokenFlag::None));
    } else {
      const auto hours = static_cast<int>(remainder / 3600);
      const auto minutes = static_cast<int>((remainder % 3600) / 60);
      const auto secs = static_cast<int>(remainder % 60);
      std::string text;
      text.reserve(9);
      text += twoDigits(hours);
      text += ":";
      text += twoDigits(minutes);
      text += ":";
      text += twoDigits(secs);
      out.push_back(
          makeScratchToken(lex::TokenKind::StringLiteral, quoted(text), caret, PPTokenFlag::None));
    }
    return true;
  }
  case BuiltinKind::HasInclude:
    error = PPError{caret.span(), "'__has_include' is handled as an operator inside '#if'",
                    PPErrorCode::ExpressionSyntax};
    return false;
  case BuiltinKind::Pragma: {
    // One operand, and it must be one string literal: `_Pragma` is an operator
    // over a literal, not a function that takes an expression.
    const bool oneLiteral = arguments.size() == 1 && [&] {
      const std::vector<PPToken> operand = detail::withoutTrivia(arguments.front());
      return operand.size() == 1 && operand.front().is(lex::TokenKind::StringLiteral);
    }();
    if (!oneLiteral) {
      error = PPError{caret.span(), "'_Pragma' expects a single string literal operand",
                      PPErrorCode::InvalidPragmaOperand};
      return false;
    }

    const std::vector<PPToken> operand = detail::withoutTrivia(arguments.front());
    const std::string text = destringize(spelling(operand.front()));

    // The text is lexed and handed to the same handler `#pragma` uses, so the
    // two spellings of a pragma cannot mean different things. `#pragma once`
    // written as `_Pragma("once")` marks the file for exactly that reason.
    std::vector<PPToken> line;
    line.push_back(makeScratchToken(lex::TokenKind::Hash, "#", caret, PPTokenFlag::None));
    line.push_back(
        makeScratchToken(lex::TokenKind::Identifier, "pragma", caret, PPTokenFlag::None));
    for (std::uint32_t at = 0; at < text.size();) {
      const lex::Token raw = lex::lexOne(text, at);
      if (raw.length == 0) {
        break;
      }
      line.push_back(
          makeScratchToken(raw.kind, text.substr(at, raw.length), caret, PPTokenFlag::None));
      at += raw.length;
    }
    handlePragma(line, caret);
    return true;
  }
  case BuiltinKind::None:
    break;
  }
  error =
      PPError{caret.span(), "internal error: unknown builtin macro", PPErrorCode::InvalidDirective};
  return false;
}

} // namespace minc::pp
