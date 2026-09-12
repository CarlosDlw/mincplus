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
  const auto builtin = [&](std::string_view name, BuiltinKind kind, MacroKind macroKind) {
    MacroInfo info;
    info.name = session_->symbols().intern(name);
    info.kind = macroKind;
    info.builtin = kind;
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
}

bool Preprocessor::expandBuiltin(const MacroInfo& macro, const PPToken& nameToken,
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
  case BuiltinKind::None:
    break;
  }
  error =
      PPError{caret.span(), "internal error: unknown builtin macro", PPErrorCode::InvalidDirective};
  return false;
}

} // namespace minc::pp
