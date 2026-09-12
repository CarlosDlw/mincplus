// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The preprocessor: the facade that sequences one translation unit.
//
// It is a *client of the lexer*. A file is lexed by `lex::TokenStream` once and
// this class walks that stream, so there is exactly one definition of "what is an
// identifier", `##` re-lexes with the same `lexOne`, and the `#`-is-not-a-token
// decision from the lexer pays off: the `#` of a directive belongs to this stage
// and nowhere else.
//
// The whole class is a state machine over two stacks and nothing else:
//
//   * `files_` -- the include stack. The top frame's token stream is what the
//     `#define`s and directives come from; trivia is still present here, because
//     a directive ends at a newline and `#` stringification reads the whitespace
//     that was written between two tokens.
//   * `contexts_` -- the macro expansion stack (GCC's model). A context is the
//     replacement list of a macro being rescanned; the macro is disabled while
//     its own context is live and re-enabled when it is popped, and a token that
//     was *not* replaced because its macro was disabled is marked ineligible for
//     the rest of the scan ("blue paint").
//
// Two stacks also give the things the references have to work around: lookahead
// is a walk over the two stacks (so "backing up more than one token is not
// permitted" does not apply), and directives are only recognized when
// `contexts_` is empty, which is exactly "this token was written in a file, not
// produced by a macro".
//
// Arguments and `#if` lines are expanded by the *same* loop: both push their
// tokens as a context and drain it. That is why there is one expansion
// implementation rather than three, and why `defined` is handled in one place.
//
// Errors are values (`PPError`), not diagnostics: this stage links no `DiagBag`,
// so an expander bug is reproducible in a test that never constructs a `Session`
// ... except that it does, because sources live there. The point stands: nothing
// here prints, and `pp_report` is the only file that knows about severity.
//
// Design record: `docs/architectures/preprocessor.md`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lex/header_name.h"
#include "lex/token.h"
#include "lex/token_stream.h"
#include "pp/conditionals.h"
#include "pp/const_expr.h"
#include "pp/expansion_table.h"
#include "pp/include_resolver.h"
#include "pp/macro.h"
#include "pp/pp_error.h"
#include "pp/pp_record.h"
#include "pp/pp_token.h"
#include "pp/stringify.h"
#include "pp/token_text.h"
#include "support/fs/fs.h"
#include "support/intern/sym_id.h"
#include "support/limits.h"
#include "support/session/session.h"
#include "support/span/file_id.h"

namespace minc::pp {

struct PPOptions {
  // The budgets, in one place so a driver (or a test) can lower them for a
  // constrained environment. Every value is clamped to the compile-time ceiling
  // in `support/limits.h` on use, so no configuration can raise them past what
  // the language is willing to do -- "they can be lowered, never disabled" is
  // then true of the code and not just of the comment.
  struct Budgets {
    std::size_t expandedTokens = support::kMaxExpandedTokens;
    std::size_t preprocessedBytes = support::kMaxPreprocessedBytes;
    std::size_t includesPerUnit = support::kMaxIncludesPerUnit;
    std::size_t includeDepth = support::kMaxIncludeDepth;
  };

  IncludeSearchLists includes;
  // `-D name[=body]`, applied after the builtins so a command line can override
  // one (which is how `-D__STDC__=0` behaves in every other compiler).
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<std::string> undefines;
  // `SOURCE_DATE_EPOCH`, in seconds. Absent means `__DATE__`/`__TIME__` are an
  // error rather than "now": a build that silently embeds the current time is a
  // build that cannot be reproduced.
  std::optional<std::int64_t> sourceDateEpoch;
  // Accepts the GNU extensions that are named and tested; everything else that
  // only GCC accepts is a diagnostic. Silently accepting them is how a header
  // becomes unportable.
  bool gnuExtensions = false;
  // `-Wundef`.
  bool warnUndef = false;
  // `-Wunknown-pragmas`. Off by default: a pragma is each target's business, and
  // a compiler that diagnoses every `#pragma` it does not implement is loud
  // about other people's extensions.
  bool warnUnknownPragma = false;
  Budgets budgets;
  // Diagnose a file read twice without a guard (see the design record: no
  // mainstream compiler does, and it is a real bug class).
  bool warnMissingGuard = true;
  bool record = true;
  bool recordExpansionMap = true;
  // The multiple-include optimization. Only ever turned off by a test that
  // asserts the optimization changes nothing, which is what makes it safe.
  bool optimizeIncludes = true;
};

struct PPResult {
  // Every token of the preprocessed translation unit, in order, trivia included.
  // The parser reads the significant ones and the tree builder reads all of
  // them, so this is the whole output and not a filtered view of it: dropping
  // trivia here is what would leave the tree unable to hold whitespace.
  std::vector<PPToken> tokens;
  // The preprocessed text the tokens tile, and what `preprocessedStream` builds
  // a `lex::TokenStream` over. Owned here because the stream holds a view of it.
  std::string text;
  // `tokens` as lexer tokens over `text`, plus the source span each one was
  // *written* at. That pair is what makes a caret point at a header while the
  // tree is built over the preprocessed text.
  std::vector<lex::Token> stream;
  std::vector<support::Span> origins;
  // Every file this run lexed, in read order. Kept so the driver can report the
  // lexical problems of *all* of them: a bad byte in a header is a real error,
  // and the preprocessor is the only stage that knows the header was read.
  std::vector<std::shared_ptr<const lex::TokenStream>> lexed;
  // The spans this run read as header-names: the `<...>` or `"..."` operand of
  // `#include`, `#include_next`, and `__has_include`.
  //
  // Kept because those bytes are *not* the tokens the plain lexer made of them.
  // Inside a header-name `//` is not a comment and `\d` is not an escape, so
  // reporting the lexer's reading of them would be reporting on a reading nobody
  // used -- `#include "c:\dir\x.h"` would draw an invalid-escape error on valid
  // code. `reportLexedFileErrors` consults this instead.
  std::vector<support::Span> headerNames;
  std::vector<PPError> errors;
  std::vector<PPError> warnings;
  support::FileId mainFile = support::kInvalidFile;
};

// The front end's view of a preprocessor run: the tokens the parser consumes,
// tiled over the preprocessed text, with each token's origin for diagnostics.
// `result` owns the text, so the stream must not outlive it. Trivia is in the
// stream; `TokenStreamSource` is what skips it for the grammar.
[[nodiscard]] lex::TokenStream preprocessedStream(const PPResult& result);

class Preprocessor final : public TokenText {
public:
  Preprocessor(support::Session& session, PPOptions options);
  ~Preprocessor() override;
  Preprocessor(const Preprocessor&) = delete;
  Preprocessor& operator=(const Preprocessor&) = delete;

  // Preprocesses `mainFile`, which must already be loaded in the session.
  // Always returns; the result is a valid prefix plus diagnostics when a budget
  // was hit.
  [[nodiscard]] PPResult run(support::FileId mainFile);

  // Drops every piece of per-translation-unit state, keeping the options. The
  // language server preprocesses the same file repeatedly.
  void reset();

  // --- TokenText ------------------------------------------------------------

  [[nodiscard]] std::string_view spelling(const PPToken& token) const override;
  [[nodiscard]] std::uint32_t addScratch(std::string text) override;
  [[nodiscard]] bool relex(std::string_view text, PPToken& out) override;

  // --- inspection (the `pp` command and the tests) ---------------------------

  [[nodiscard]] const MacroTable& macros() const {
    return macros_;
  }
  [[nodiscard]] const PPRecord& record() const {
    return record_;
  }
  [[nodiscard]] const ExpansionTable& expansions() const {
    return expansions_;
  }
  [[nodiscard]] const IncludeResolver& includes() const {
    return resolver_;
  }
  // How many times `name` was expanded, for `pp --defines`.
  [[nodiscard]] std::uint32_t useCount(support::SymId name) const;
  [[nodiscard]] std::string_view symbol(support::SymId name) const {
    return session_->symbols().lookup(name);
  }
  // The path of a loaded file, for diagnostics and the include graph.
  [[nodiscard]] std::string_view pathOf(support::FileId file) const;

private:
  // --- the two stacks -------------------------------------------------------

  struct Context {
    std::vector<PPToken> tokens;
    std::size_t index = 0;
    // The macro whose replacement list this is, so it can be disabled while the
    // context is live and re-enabled when it is popped.
    support::SymId macro = support::kInvalidSym;
    ExpansionId expansion = kNoExpansion;
    // Where this replacement list was invoked, which is what `__LINE__` inside a
    // macro body has to report.
    SourceLoc invocation;
    // Index into the record's expansion sites, or `kNoSite`. Closed when this
    // context is popped, which is when the range of output it produced is
    // finally known.
    std::uint32_t siteIndex = kNoSite;
  };

  struct FileFrame {
    support::FileId id = support::kInvalidFile;
    std::string path;
    std::string dir;
    support::FileIdentity identity;
    bool isSystem = false;
    // Shared, not unique: every file the run lexed stays alive in the result, so
    // the driver can report a header's lexical problems after the include has
    // ended. Popping the frame must not destroy the evidence.
    std::shared_ptr<const lex::TokenStream> stream;
    std::size_t index = 0;
    // Conditional nesting when this file was entered, so an `#endif` cannot
    // close a conditional opened by another file and an unterminated one is
    // reported where it starts.
    std::size_t conditionalDepth = 0;
    // The includer's line-start state when this file was entered. Line starts
    // are per file: the included file's last token must not decide whether the
    // *includer's* next `#` begins a directive, which is why `closeFile` puts
    // this back instead of leaving one shared flag where the include left it.
    bool savedAtLineStart = true;
  };

  // --- cursor ---------------------------------------------------------------

  // The next token, popping exhausted contexts and files. Only returns an
  // end-of-file token when there is no input left at all, so a caller that sees
  // it has really consumed the translation unit.
  [[nodiscard]] PPToken take();
  // Closes every file whose last token has been read, so the top of the file
  // stack is the file the *next* decision is about. `take` closes them lazily,
  // which is invisible while reading tokens and wrong before deciding whether a
  // directive starts: an included file that has not been popped yet is what the
  // directive check would ask about, and the answer would come from the include.
  void settle();
  // The next significant token without consuming it. Scans contexts above
  // `lookaheadFloor_` and, only while `lookaheadFile_` is set, the current file.
  // Both exist so pre-expanding an argument cannot see tokens the standard does
  // not let it see: an argument is a closed world, and a token that escaped it
  // would turn `#define foo(x) x` + `foo(foo) (2)` into `2`.
  [[nodiscard]] PPToken peekSignificant() const;
  [[nodiscard]] PPToken eofToken() const;
  void pushFile(support::FileId file, std::string path, std::string dir, bool isSystem,
                const support::FileIdentity& identity);
  void closeFile();
  [[nodiscard]] PPToken toPP(const lex::Token& token, support::FileId file) const;
  void updateAtLineStart(const lex::Token& token);
  [[nodiscard]] bool atEndOfInput() const {
    return files_.empty() && contexts_.empty();
  }

  // --- emit -----------------------------------------------------------------

  // Appends one token (trivia or significant) to the preprocessed output,
  // inserting the separator space if this token does not continue the previous
  // bytes verbatim. See the body for why that one rule is the whole text model.
  void emit(const PPToken& token);
  // True when `origin` starts exactly where the last appended token's ended, in
  // the same file: the bytes were written next to each other, so no separator is
  // needed and the output stays byte-identical to the source.
  [[nodiscard]] bool continuesOutput(const support::Span& origin) const;
  void appendSeparator();
  void pushError(PPError error);
  void pushWarning(PPError error);
  [[nodiscard]] bool overExpansionBudget(std::size_t produced);
  [[nodiscard]] std::size_t expandedTokenBudget() const;
  [[nodiscard]] std::size_t preprocessedByteBudget() const;
  [[nodiscard]] PPToken makeScratchToken(lex::TokenKind kind, std::string text,
                                         const SourceLoc& where, PPTokenFlag flag);

  // --- directives -----------------------------------------------------------

  [[nodiscard]] bool startsDirective() const;
  // Consumes one whole directive line (from the `#` through the newline) from the
  // current file. Trivia is included, because a definition's body and an
  // `#error` message are read from it.
  void readDirectiveLine(std::vector<PPToken>& line);
  // Rewrites a directive line so each header-name is one `HeaderName` token,
  // replacing the run of tokens the plain lexer made of it. Runs on every
  // directive line: it is what makes `#include <a//b.h>` resolvable and what
  // keeps `#include` and `__has_include` from disagreeing about a name.
  void spliceHeaderNames(std::vector<PPToken>& line, std::string_view text);
  void spliceHeaderName(std::vector<PPToken>& line, std::size_t index, const lex::HeaderName& name,
                        support::FileId file);
  void handleDirective();
  void handleDefine(const std::vector<PPToken>& line, const SourceLoc& directive);
  void handleUndef(const std::vector<PPToken>& line, const SourceLoc& directive);
  void handleInclude(const std::vector<PPToken>& line, const SourceLoc& directive, bool next);
  void handleConditional(const std::vector<PPToken>& line, const SourceLoc& directive,
                         std::string_view name);
  void handleLine(const std::vector<PPToken>& line, const SourceLoc& directive);
  void handleErrorOrWarning(const std::vector<PPToken>& line, const SourceLoc& directive,
                            bool warning);
  void handlePragma(const std::vector<PPToken>& line, const SourceLoc& directive);
  [[nodiscard]] std::string rawTextOf(const std::vector<PPToken>& line, std::size_t from) const;
  // Turns a definition's tokens (name, optional `(params)`, body) into a
  // `MacroInfo`, diagnosing the parts that are structurally wrong (`#` before
  // something that is not a parameter, `##` at either end, a missing name, too
  // many parameters). Shared by `#define` and by `-D` so the two cannot drift.
  [[nodiscard]] bool buildMacroInfo(const std::vector<PPToken>& definition, const SourceLoc& define,
                                    const SourceLoc& defineName, MacroInfo& out);
  [[nodiscard]] bool buildParameterList(const std::vector<PPToken>& definition, std::size_t& index,
                                        MacroInfo& out);
  [[nodiscard]] bool buildReplacementList(const std::vector<PPToken>& definition, std::size_t index,
                                          MacroInfo& out);
  // `-D`: `NAME`, `NAME=body`, or `NAME(a, b)=body`.
  void defineFromText(std::string_view text);

  // --- expansion ------------------------------------------------------------

  // `emitPath` is true when this invocation's replacement list goes to the
  // output stream, which is the only case where an output range is worth
  // recording: an argument's pre-expansion produces no output of its own.
  [[nodiscard]] bool tryExpand(PPToken& token, bool emitPath);
  void expandMacro(const MacroInfo& macro, const PPToken& nameToken, bool emitPath);
  // The replacement list, with arguments substituted and `#`/`##` applied.
  void substitute(const MacroInfo& macro, const PPToken& nameToken, ExpansionId expansion,
                  std::vector<std::vector<PPToken>>& arguments, std::vector<PPToken>& out);
  // Pops the innermost context, closing its recorded output range and
  // re-enabling its macro. The only place a context is discarded.
  void popContext();
  // Takes the next significant token from the contexts above `base`, popping
  // exhausted ones. Used by the `defined`/`__has_include` operators, whose
  // operands may be in an enclosing context but never in the file.
  [[nodiscard]] std::optional<PPToken> takeAbove(std::size_t base);
  void handleDefined(std::vector<PPToken>& out, std::size_t base, const PPToken& operatorToken);
  void handleHasInclude(std::vector<PPToken>& out, std::size_t base, const PPToken& operatorToken);
  struct Arguments {
    std::vector<std::vector<PPToken>> values;
    bool ok = false;
    PPError error;
  };
  // Consumes the `(` and everything up to the matching `)`.
  [[nodiscard]] Arguments collectArguments(const MacroInfo& macro, const PPToken& nameToken);
  // Expands contexts pushed above `base` into `out`, never reading the file.
  void drainContexts(std::size_t base, std::vector<PPToken>& out, bool expression);
  [[nodiscard]] bool preExpand(const std::vector<PPToken>& raw, std::vector<PPToken>& out);
  [[nodiscard]] bool tryExpandDefined(std::vector<PPToken>& out);
  // `arguments` is the collected argument list for a function-like builtin. Only
  // `_Pragma` reads it -- it needs the operand itself, not a replacement -- and
  // it is passed rather than re-collected because the caller has just taken it.
  [[nodiscard]] bool expandBuiltin(const MacroInfo& macro, const PPToken& nameToken,
                                   const std::vector<std::vector<PPToken>>& arguments,
                                   std::vector<PPToken>& out, std::optional<PPError>& error);
  [[nodiscard]] bool isMacroDisabled(support::SymId name) const;
  [[nodiscard]] bool isIneligible(const PPToken& token) const;

  // --- `#if` ----------------------------------------------------------------

  // Expands and evaluates the controlling expression of an `#if`-like directive.
  [[nodiscard]] bool evaluateCondition(const std::vector<PPToken>& line, std::size_t from,
                                       bool& ok);

  // --- helpers --------------------------------------------------------------

  [[nodiscard]] SourceLoc locOf(const PPToken& token) const {
    return token.loc.spelling;
  }
  [[nodiscard]] support::Span spanOf(const PPToken& token) const {
    return token.loc.spelling.span();
  }
  [[nodiscard]] std::int64_t currentLine(const SourceLoc& where) const;
  void installPredefined();

  // --- state ----------------------------------------------------------------

  support::Session* session_;
  PPOptions options_;
  MacroTable macros_;
  ExpansionTable expansions_;
  IncludeResolver resolver_;
  ConditionalStack conditionals_;
  PPRecord record_;

  std::vector<FileFrame> files_;
  std::vector<Context> contexts_;
  bool atLineStart_ = true;
  // Contexts below this index are invisible to lookahead; see peekSignificant.
  std::size_t lookaheadFloor_ = 0;
  // Whether lookahead may leave the context stack and read the current file.
  // Cleared while an argument is pre-expanded, where the argument's own tokens
  // are the whole world; the file would expose the tokens that *follow* the
  // invocation, which is exactly the boundary the standard draws.
  bool lookaheadFile_ = true;

  std::vector<PPToken> output_;
  // The preprocessed text being built, and what each output token is in it.
  // Parallel to `output_`: `outputOffset_[i]` is where `output_[i]`'s spelling
  // starts and `outputLength_[i]` is how long it is, so the result can be handed
  // to the front end as one lossless tiling rather than as a token list whose
  // positions mean nothing.
  std::string outputText_;
  std::vector<std::uint32_t> outputOffset_;
  std::vector<std::uint32_t> outputLength_;
  std::vector<support::Span> outputOrigin_;
  // The origin of the last real token appended, and whether a separator space
  // was written after it. Together they answer "does this token continue the
  // previous bytes?", which is the whole separator rule.
  support::Span lastOrigin_;
  bool wroteSeparator_ = false;
  // Every file lexed by this run, in read order; becomes `PPResult::lexed`.
  std::vector<std::shared_ptr<const lex::TokenStream>> lexed_;
  // Spans read as header-names; becomes `PPResult::headerNames`.
  std::vector<support::Span> headerNames_;
  // Spellings of synthesized tokens, indexed by `PPToken::scratch`. A deque so
  // the views handed out by `spelling` stay valid as more are added.
  std::deque<std::string> scratch_;
  // True while draining an `#if` expression, which is the only place the two
  // operator-like builtins (`defined`, `__has_include`) are allowed.
  bool expressionMode_ = false;

  std::vector<PPError> errors_;
  std::vector<PPError> warnings_;
  // The end-of-file location reported when the input runs out, kept because the
  // include stack is empty by the time a caller sees that token.
  SourceLoc lastEof_;
  // Set when a budget was hit: the token stream so far is a valid prefix, but
  // continuing would be unbounded work, so the run stops instead.
  bool aborted_ = false;
  std::unordered_map<support::SymId, std::uint32_t> useCounts_;
  std::uint32_t counter_ = 0;
  std::size_t expandedTokens_ = 0;
  std::size_t emittedBytes_ = 0;
  // Line adjustment from `#line` / line markers.
  std::int64_t lineOffset_ = 0;
  std::optional<std::string> fileNameOverride_;
  bool budgetReported_ = false;
  bool selfReferenceReported_ = false;
};

} // namespace minc::pp
