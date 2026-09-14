// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/help_text.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include "backend/codegen.h"
#include "driver/exit_code.h"
#include "driver/help_render.h"
#include "driver/version.h"
#include "sema/target.h"

namespace minc::driver {
namespace {

// The two terminal questions, asked once each: how wide, and whether escape
// sequences will be understood here. Both come from `support/term`, which is the
// only module that knows how to ask, and the *choice* the user made is applied
// on top of the answer rather than instead of it.
[[nodiscard]] PageStyle styleFor(std::FILE* stream, support::ColorChoice choice) {
  const bool supports =
      stream == stdout ? support::stdoutSupportsColor() : support::stderrSupportsColor();
  return PageStyle{support::terminalWidth(stream), support::colorModeFrom(supports, choice)};
}

} // namespace

std::string usageLine() {
  return "Usage: " + std::string(programSpec().name) + " <command> [options] [files...]";
}

std::string helpText(std::optional<Command> topic) {
  const PageStyle style;
  return topic.has_value() ? renderCommand(*topic, style) : renderOverview(style);
}

std::string versionLine() {
  return std::string(kProgName) + " " + kVersion;
}

std::string versionBlock() {
  // The two facts are kept in named strings first: `VersionFacts` holds views,
  // and a view into a temporary is a use-after-free waiting for a change of
  // optimisation level to start mattering.
  //
  // The host comes from `sema` -- the triple this compiler was built for, in the
  // canonical spelling `--target` accepts -- and is empty only in a build whose
  // host this compiler has no ABI row for, which is the one case where the line
  // has something to say. `unknown` says it, rather than leaving a blank column
  // that reads as a rendering fault.
  const std::string_view hostTriple = sema::hostTriple();
  const std::string host = hostTriple.empty() ? std::string("unknown") : std::string(hostTriple);
  const std::string llvm = backend::llvmVersion();
  const VersionFacts facts{kProgName, kVersion, host, llvm};
  return renderVersion(facts, /*verbose=*/true);
}

int runHelp(support::ColorChoice choice, std::optional<Command> topic) {
  const PageStyle style = styleFor(stdout, choice);
  std::cout << (topic.has_value() ? renderCommand(*topic, style) : renderOverview(style));
  return exitCode(ExitCode::Ok);
}

int runBareInvocation(support::ColorChoice choice) {
  const PageStyle style = styleFor(stderr, choice);
  std::cerr << renderOverview(style);
  return exitCode(ExitCode::Usage);
}

int runVersion(support::ColorChoice choice, bool verbose) {
  (void)choice; // version output has no columns to align and nothing to color
  std::cout << (verbose ? versionBlock() : versionLine() + "\n");
  return exitCode(ExitCode::Ok);
}

} // namespace minc::driver
