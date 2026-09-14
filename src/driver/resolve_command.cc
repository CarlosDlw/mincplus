// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/resolve_command.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "ast/dump.h"
#include "ast/lower.h"
#include "ast/validate.h"
#include "driver/error_report.h"
#include "driver/exit_code.h"
#include "driver/input_source.h"
#include "lex/token_stream.h"
#include "parse/parse_report.h"
#include "pp/pp_report.h"
#include "pp/preprocessor.h"
#include "resolve/dump.h"
#include "resolve/resolve.h"
#include "resolve/resolve_report.h"
#include "resolve/source_to_def.h"
#include "resolve/store.h"
#include "support/diag/diag_renderer.h"
#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/source/source_file.h"
#include "support/term/terminal.h"
#include "syntax/store.h"
#include "syntax/tree.h"

namespace minc::driver {
namespace {

// The byte offset of a 1-based (line, column), handling LF, CRLF and a lone CR
// the same way the line table does. Nullopt when the position is past the end.
[[nodiscard]] std::optional<std::uint32_t> offsetOf(const support::SourceFile& file,
                                                    std::uint32_t line, std::uint32_t col) {
  if (line == 0 || col == 0) {
    return std::nullopt;
  }
  const std::string_view text = file.text;
  std::vector<std::size_t> starts{0};
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      starts.push_back(i + 1);
    } else if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        starts.push_back(i + 2);
        ++i;
      } else {
        starts.push_back(i + 1);
      }
    }
  }
  if (line - 1 >= starts.size()) {
    return std::nullopt;
  }
  const std::size_t offset = starts[line - 1] + (col - 1);
  if (offset > text.size()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(offset);
}

[[nodiscard]] bool toNumber(const std::string& text, std::uint32_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint32_t>(c - '0');
  }
  out = value;
  return true;
}

// `[file:]line:col`. The file part is optional and may itself contain `:`, so
// the split is from the right: the last colon separates the column, the one
// before it (if any) separates the file from the line.
[[nodiscard]] bool parsePosition(const std::string& spec, std::string& path, std::uint32_t& line,
                                 std::uint32_t& col) {
  const std::size_t last = spec.rfind(':');
  if (last == std::string::npos) {
    return false;
  }
  const std::size_t previous = last > 0 ? spec.rfind(':', last - 1) : std::string::npos;
  std::string lineText;
  if (previous == std::string::npos) {
    path.clear();
    lineText = spec.substr(0, last);
  } else {
    path = spec.substr(0, previous);
    lineText = spec.substr(previous + 1, last - previous - 1);
  }
  const std::string colText = spec.substr(last + 1);
  return toNumber(lineText, line) && toNumber(colText, col);
}

void printAt(const resolve::DefMap& map, const support::Session& session, const std::string& spec,
             support::FileId mainFile, std::ostream& out) {
  std::string path;
  std::uint32_t line = 0;
  std::uint32_t col = 0;
  if (!parsePosition(spec, path, line, col)) {
    out << "mincc resolve: --at expects [file:]line:col, got '" << spec << "'\n";
    return;
  }
  // `path` is optional; without it the position is read in the main file.
  support::FileId file = mainFile;
  if (!path.empty()) {
    bool found = false;
    for (std::uint32_t id = 0; id < session.sources().fileCount(); ++id) {
      const support::SourceFile* candidate = session.sources().find(id);
      if (candidate != nullptr && candidate->path == path) {
        file = id;
        found = true;
        break;
      }
    }
    if (!found) {
      out << spec << ": no such file is part of this translation unit\n";
      return;
    }
  }
  const support::SourceFile* source = session.sources().find(file);
  if (source == nullptr) {
    out << spec << ": no such file is part of this translation unit\n";
    return;
  }
  const std::optional<std::uint32_t> offset = offsetOf(*source, line, col);
  if (!offset.has_value()) {
    out << spec << ": position is past the end of the file\n";
    return;
  }
  const std::optional<resolve::DefId> def = resolve::defAt(map, file, *offset);
  if (!def.has_value()) {
    out << resolve::locationOf(session.sources(), support::Span::at(file, *offset))
        << ": no declaration starts here\n";
    return;
  }
  const resolve::Def& found = map.defs[def->index];
  out << resolve::locationOf(session.sources(), support::Span::at(file, *offset)) << "  -> defs#"
      << def->index << "  " << resolve::toString(found.kind) << " "
      << session.symbols().lookup(found.name) << "  declared at "
      << resolve::locationOf(session.sources(), found.nameSpan) << "\n";
}

void printAst(const ast::LoweredFile& file, std::string_view path, std::ostream& out) {
  out << "# " << path << ": lowered AST (" << file.nodeCount() << " nodes, "
      << file.children().size() << " child slots)\n";
  ast::AstDumpOptions itemOptions;
  itemOptions.items = true;
  out << "# item tree\n" << ast::dumpAst(file, itemOptions);
  out << "# nodes\n" << ast::dumpAst(file, {});
}

} // namespace

int resolveInputs(const ResolveRequest& request, std::ostream& out, std::ostream& err) {
  // One session, one tree store and one resolve store for the whole invocation:
  // together they own the sources, the diagnostics, the arena, the green node
  // cache, and the revision-keyed def maps.
  support::Session session;
  syntax::TreeStore trees(session.arena());
  resolve::ResolveStore resolves;

  const support::DiagRenderer renderer(&session.sources(),
                                       support::RenderOptions{request.diagnosticColor, 4});

  // The preprocessed units, alive for the whole invocation: a token stream is a
  // view into the preprocessed text, and the text belongs to the `PPResult`.
  // `reserve` is load-bearing for the same reason it is in `parse`.
  std::vector<pp::PPResult> preprocessed;
  preprocessed.reserve(request.inputs.size());

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

    pp::PPOptions ppOptions;
    ppOptions.defines = request.defines;
    ppOptions.undefines = request.undefines;
    ppOptions.includes.quote = request.includeDirs;
    ppOptions.includes.system = request.systemDirs;
    pp::Preprocessor preprocessor(session, std::move(ppOptions));

    session.diags().clear();
    preprocessed.push_back(preprocessor.run(file->id));
    const pp::PPResult& result = preprocessed.back();
    const std::size_t lexical = pp::reportLexedFileErrors(result, session.diags());
    (void)pp::reportPPErrors(result.errors, preprocessor.expansions(), session.diags(),
                             &session.symbols());
    (void)pp::reportPPWarnings(result.warnings, preprocessor.expansions(), session.diags(),
                               &session.symbols(), result.systemRegions);

    const lex::TokenStream stream = pp::preprocessedStream(result);
    const syntax::SyntaxTree* tree = trees.parse(stream, file->revision);
    if (tree == nullptr) {
      printError(err, name + ": out of memory while building the syntax tree");
      failed = true;
      continue;
    }
    const std::size_t syntax = parse::reportParseErrors(tree->errors(), session.diags());

    // Lower, then validate, then resolve. Each stage takes the previous one's
    // artifact and returns one of its own; the errors are values, converted to
    // diagnostics here and nowhere earlier.
    const ast::OriginTable origins{&stream};
    ast::LowerOutput lowered = ast::lowerFile(*tree, session.symbols(), origins);
    const std::vector<ast::AstError> structural = ast::validate(lowered.file);
    const std::size_t loweringErrors = resolve::reportAstErrors(lowered.errors, session.diags());
    const std::size_t structuralErrors = resolve::reportAstErrors(structural, session.diags());

    resolve::ResolveOptions options;
    options.warnUnused = request.warnUnused;
    options.warnShadow = request.warnShadow;
    // Through the store, so the cache the language server will live on is
    // exercised by the command that proves the stage.
    const resolve::ResolveOutput* resolved =
        resolves.outputFor(file->id, file->revision, lowered.file, session.symbols(), options);
    const std::size_t resolveErrors =
        resolve::reportResolveErrors(resolved->errors, session.diags());
    const std::size_t resolveWarnings =
        resolve::reportResolveWarnings(resolved->warnings, session.diags());

    if (request.showAst) {
      printAst(lowered.file, file->path, out);
    } else {
      const std::size_t errorCount = lexical + syntax + result.errors.size() + loweringErrors +
                                     structuralErrors + resolveErrors;
      out << "# " << file->path << "  (scopes " << resolved->map.scopes.size() << ", defs "
          << resolved->map.defs.size() << ", refs " << resolved->map.refs.size() << ", "
          << errorCount << " error(s), " << resolveWarnings << " warning(s))\n";
      resolve::DefMapDumpOptions dumpOptions;
      dumpOptions.showRefs = request.showRefs;
      dumpOptions.showUnresolved = request.showUnresolved;
      out << resolve::dumpDefMap(resolved->map, lowered.file, session.symbols(), session.sources(),
                                 dumpOptions);
    }
    if (!request.at.empty()) {
      printAt(resolved->map, session, request.at, file->id, out);
    }
    out.flush();

    if (!session.diags().empty()) {
      err << renderer.renderAll(session.diags());
      err.flush();
    }
    if (lexical != 0 || syntax != 0 || !result.errors.empty() || loweringErrors != 0 ||
        structuralErrors != 0 || resolveErrors != 0) {
      failed = true;
    }
  }

  return exitCode(failed ? ExitCode::Failure : ExitCode::Ok);
}

int runResolve(const CliOptions& options) {
  if (options.inputs.empty()) {
    return usageError("no input files");
  }

  ResolveRequest request;
  request.inputs = options.inputs;
  request.defines = splitDefines(options.defines);
  request.undefines = options.undefines;
  request.includeDirs = options.includeDirs;
  request.systemDirs = options.systemDirs;
  request.showRefs = options.showRefs;
  request.showUnresolved = options.showUnresolved;
  request.showAst = options.showAst;
  request.warnUnused = options.warnUnused;
  request.warnShadow = options.warnShadow;
  request.at = options.at;
  request.diagnosticColor =
      support::colorModeFrom(support::stderrSupportsColor(), options.colorChoice);
  return resolveInputs(request, std::cout, std::cerr);
}

} // namespace minc::driver
