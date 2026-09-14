// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/pp_command.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "driver/diagnostic_options.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/input_source.h"
#include "pp/pp_record.h"
#include "pp/pp_report.h"
#include "pp/pp_token.h"
#include "pp/preprocessor.h"
#include "support/diag/diag_renderer.h"
#include "support/source/source_file.h"
#include "support/term/terminal.h"

namespace minc::driver {
namespace {

// `SOURCE_DATE_EPOCH` is the *only* environment variable this pipeline reads,
// and the driver is the only layer that reads it. A build that depends on the
// machine's environment is a build that cannot be reproduced, which is why the
// preprocessor is handed the value instead of looking it up.
[[nodiscard]] std::optional<std::int64_t> sourceDateEpoch() {
  const char* value = std::getenv("SOURCE_DATE_EPOCH");
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  std::int64_t seconds = 0;
  for (const char* digit = value; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') {
      return std::nullopt;
    }
    const std::int64_t next = seconds * 10 + (*digit - '0');
    if (next < seconds) {
      return std::nullopt; // overflowed: ignore it rather than wrap
    }
    seconds = next;
  }
  return seconds;
}

[[nodiscard]] std::string locationOf(const support::Session& session, const support::Span& span) {
  const support::SourceFile* file = session.sources().find(span.file);
  if (file == nullptr) {
    return "<unknown>";
  }
  const support::LineCol at = file->lookup(span.begin);
  return file->path + ":" + std::to_string(at.line) + ":" + std::to_string(at.col);
}

[[nodiscard]] std::string locationOf(const support::Session& session, const pp::SourceLoc& loc) {
  return locationOf(session, loc.span());
}

// The tokens a reader would call "the output": everything but trivia.
[[nodiscard]] std::size_t significantCount(const pp::PPResult& result) {
  std::size_t count = 0;
  for (const pp::PPToken& token : result.tokens) {
    if (!pp::isPPTrivia(token.kind)) {
      ++count;
    }
  }
  return count;
}

// The preprocessed stream, aligned. Same shape as `mincc lex`, because a reader
// should not have to learn a second table format for the second stage.
void printTokens(const pp::Preprocessor& preprocessor, const pp::PPResult& result,
                 const support::Session& session, std::ostream& out) {
  // The stream carries trivia so a tree can hold every byte, but a *token* dump
  // is about tokens: printing the whitespace between them is not more
  // informative, it is twice as long, and the tree is where trivia belongs. The
  // filtered list is built once and both passes walk it, so a column can never
  // be computed from a different set of rows than the one printed.
  std::vector<const pp::PPToken*> shown;
  shown.reserve(result.tokens.size());
  for (const pp::PPToken& token : result.tokens) {
    if (!pp::isPPTrivia(token.kind)) {
      shown.push_back(&token);
    }
  }

  std::size_t locationWidth = 4;
  std::size_t kindWidth = 4;
  std::vector<std::string> locations;
  locations.reserve(shown.size());
  for (const pp::PPToken* token : shown) {
    std::string location = locationOf(session, token->loc.spelling);
    locationWidth = std::max(locationWidth, location.size());
    kindWidth = std::max(kindWidth, std::string_view(lex::toString(token->kind)).size());
    locations.push_back(std::move(location));
  }

  for (std::size_t i = 0; i < shown.size(); ++i) {
    const pp::PPToken& token = *shown[i];
    std::string_view kind = lex::toString(token.kind);
    std::string_view spelling = preprocessor.spelling(token);
    out << locations[i] << std::string(locationWidth - locations[i].size(), ' ') << "  " << kind
        << std::string(kindWidth - kind.size(), ' ') << "  " << spelling;
    if (token.has(pp::PPTokenFlag::Stringified) || token.has(pp::PPTokenFlag::Pasted)) {
      out << "   [synthesized]";
    }
    out << '\n';
  }
}

void printDefines(const pp::Preprocessor& preprocessor, const support::Session& session,
                  std::ostream& out) {
  for (const pp::MacroInfo* macro : preprocessor.macros().all()) {
    out << preprocessor.symbol(macro->name);
    if (macro->builtin != pp::BuiltinKind::None) {
      out << "  [builtin]";
    } else if (macro->isFunctionLike()) {
      out << "(";
      for (std::size_t i = 0; i < macro->params.size(); ++i) {
        if (i != 0) {
          out << ", ";
        }
        out << preprocessor.symbol(macro->params[i].name);
      }
      if (macro->variadic) {
        out << (macro->params.empty() ? "..." : ", ...");
      }
      out << ")";
    } else {
      out << "  (object-like)";
    }
    if (!macro->body.empty()) {
      out << " = " << macro->bodyText();
    }
    if (macro->defineName.valid()) {
      out << "   " << locationOf(session, macro->defineName);
    }
    out << "   used " << preprocessor.useCount(macro->name) << "x\n";
  }
}

void printIncludes(const pp::PPRecord& record, const std::string& mainPath, std::ostream& out) {
  out << mainPath << "\n";
  for (const pp::InclusionRecord& inclusion : record.includes()) {
    out << std::string(static_cast<std::size_t>(inclusion.depth) * 2, ' ') << "-> "
        << inclusion.path;
    if (inclusion.isSystem) {
      out << " (system)";
    }
    if (inclusion.guardSkipped) {
      out << " (elided by the include-guard optimization)";
    } else if (!inclusion.read) {
      out << " (already read)";
    }
    out << '\n';
  }
}

void printDeps(const pp::PPRecord& record, const std::string& mainPath, std::ostream& out) {
  out << mainPath << ":";
  for (const pp::InclusionRecord& inclusion : record.includes()) {
    if (inclusion.read) {
      out << " " << inclusion.path;
    }
  }
  out << '\n';
}

// `--at [file:]line`. Splitting is done here rather than in the argument parser
// so `--at` stays one opaque string that the command interprets.
void printAt(const pp::Preprocessor& preprocessor, const pp::PPResult& result,
             const support::Session& session, const std::string& spec, std::ostream& out) {
  std::string file;
  std::string lineText = spec;
  if (const std::size_t colon = spec.rfind(':'); colon != std::string::npos) {
    file = spec.substr(0, colon);
    lineText = spec.substr(colon + 1);
  }
  std::uint32_t line = 0;
  for (const char c : lineText) {
    if (c < '0' || c > '9') {
      out << "mincc pp: --at expects [file:]line, got '" << spec << "'\n";
      return;
    }
    line = line * 10 + static_cast<std::uint32_t>(c - '0');
  }

  bool any = false;
  for (const pp::ExpansionSite& site : preprocessor.record().expansions()) {
    const support::SourceFile* source = session.sources().find(site.invocation.file);
    if (source == nullptr) {
      continue;
    }
    const support::LineCol at = source->lookup(site.invocation.offset);
    if (at.line != line || (!file.empty() && source->path != file)) {
      continue;
    }
    any = true;
    out << locationOf(session, site.invocation) << ": " << preprocessor.symbol(site.macro)
        << " expanded to";
    if (site.producedEnd == pp::kNoSite || site.producedBegin >= result.tokens.size()) {
      out << " nothing\n";
      continue;
    }
    const std::uint32_t end =
        std::min(site.producedEnd, static_cast<std::uint32_t>(result.tokens.size()));
    out << ":";
    for (std::uint32_t i = site.producedBegin; i < end; ++i) {
      out << " " << preprocessor.spelling(result.tokens[i]);
    }
    out << '\n';
  }
  if (!any) {
    out << spec << ": no macro was expanded on that line\n";
  }
}

} // namespace

int ppInputs(const PpRequest& request, std::ostream& out, std::ostream& err) {
  support::Session session;
  const support::DiagRenderer renderer(&session.sources(),
                                       diagnosticOptions(request.color, request.errorLimit));
  // The error budget spans the invocation: the bag is cleared per input.
  std::size_t errorsShown = 0;
  bool failed = false;

  for (const std::string& name : request.inputs) {
    const support::Fallible<support::FileId> id = loadInput(session, name);
    if (!id.hasValue()) {
      printError(err, id.error());
      failed = true;
      continue;
    }
    const support::SourceFile* file = session.sources().find(id.value());
    if (file == nullptr) {
      printError(err, "internal error: an input file vanished after loading");
      failed = true;
      continue;
    }

    pp::PPOptions options;
    options.defines = request.defines;
    options.undefines = request.undefines;
    options.includes.quote = request.includeDirs;
    options.includes.system = request.systemDirs;
    options.sourceDateEpoch = sourceDateEpoch();

    pp::Preprocessor preprocessor(session, std::move(options));
    // Cleared per input so the counts belong to this file and one input's
    // diagnostics never leak into the next.
    session.diags().clear();
    const pp::PPResult result = preprocessor.run(file->id);
    // Lexical problems of *every* file this run read, the main file and its
    // headers alike: the preprocessor is the only stage that knows a header was
    // read, so it is the only place they can be reported from. Reported as
    // errors and counted as such: a bad byte is a failure whether it was in this
    // file or in a file this one pulled in.
    const std::size_t lexical = pp::reportLexedFileErrors(result, session.diags());
    const std::size_t errors = pp::reportPPErrors(result.errors, preprocessor.expansions(),
                                                  session.diags(), &session.symbols());
    // Counted from what was *reported*, not from what was collected: a warning
    // inside a system header is dropped, and a summary that still counted it
    // would tell the reader to look for a diagnostic that is not there.
    const std::size_t warnings =
        pp::reportPPWarnings(result.warnings, preprocessor.expansions(), session.diags(),
                             &session.symbols(), result.systemRegions);

    if (request.showDefines) {
      printDefines(preprocessor, session, out);
    }
    if (request.showIncludes) {
      printIncludes(preprocessor.record(), file->path, out);
    }
    if (request.showDeps) {
      printDeps(preprocessor.record(), file->path, out);
    }
    if (!request.at.empty()) {
      printAt(preprocessor, result, session, request.at, out);
    }
    if (!request.showDefines && !request.showIncludes && !request.showDeps && request.at.empty()) {
      // The count matches what is printed below: trivia is in the stream, not in
      // the count, so the header cannot promise a number the table does not show.
      out << "# " << file->path << ": " << significantCount(result) << " tokens, "
          << (lexical + errors) << " error(s), " << warnings << " warning(s)\n";
      printTokens(preprocessor, result, session, out);
    }
    out.flush();

    if (!session.diags().empty()) {
      err << renderer.renderAll(session.diags(), errorsShown);
      err.flush();
    }
    if (lexical != 0 || errors != 0) {
      failed = true;
    }
  }

  return exitCode(failed ? ExitCode::Failure : ExitCode::Ok);
}

int runPp(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }

  PpRequest request;
  request.inputs = options.inputs;
  request.defines = splitDefines(options.defines);
  request.undefines = options.undefines;
  request.includeDirs = options.includeDirs;
  request.systemDirs = options.systemDirs;
  request.showDefines = options.showDefines;
  request.showIncludes = options.showIncludes;
  request.showDeps = options.showDeps;
  request.at = options.at;
  request.color = support::colorModeFrom(support::stdoutSupportsColor(), options.colorChoice);
  request.errorLimit = options.errorLimit;
  return ppInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
