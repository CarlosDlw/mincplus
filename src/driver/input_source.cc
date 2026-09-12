// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "driver/input_source.h"

#include <string>
#include <utility>

#include "support/source/file_io.h"

namespace minc::driver {

support::Fallible<support::FileId> loadInput(support::Session& session, const std::string& name) {
  // `-` is the one input name that is not a path: it means standard input,
  // which is what makes `mincc parse -` work in a pipe.
  if (name != "-") {
    return session.loadFromDisk(name);
  }
  support::Fallible<std::string> bytes = support::readStdinBytes();
  if (!bytes.hasValue()) {
    return support::makeUnexpected(bytes.error());
  }
  return session.addFile("<stdin>", std::move(bytes).value());
}

} // namespace minc::driver
