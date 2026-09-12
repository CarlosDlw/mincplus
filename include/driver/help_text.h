// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Help and version text. Kept ASCII-only on purpose: Windows consoles render
// UTF-8 unpredictably, and this output must be readable everywhere.
#pragma once

#include <string>

namespace minc::driver {

[[nodiscard]] std::string usageLine();
[[nodiscard]] std::string versionLine();
[[nodiscard]] std::string helpText();

// Write to stdout and return the process exit code.
[[nodiscard]] int runHelp();
[[nodiscard]] int runVersion();

} // namespace minc::driver
