// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Loading one command-line input into a Session.
//
// Every file-taking subcommand needs the same thing -- a path, or `-` meaning
// standard input -- and the same error surface. Keeping it here means `lex` and
// `parse` cannot drift in how they treat `-`, or in what they say when a file
// cannot be read.
#pragma once

#include <string>

#include "support/expected/fallible.h"
#include "support/session/session.h"
#include "support/span/file_id.h"

namespace minc::driver {

// Loads `name` into `session` and returns its id. `-` reads standard input
// (binary mode on Windows, so a pipe sees the same bytes as a file).
[[nodiscard]] support::Fallible<support::FileId> loadInput(support::Session& session,
                                                           const std::string& name);

} // namespace minc::driver
