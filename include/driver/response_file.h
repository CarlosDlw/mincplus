// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// `@file`: a command line too long to type, kept in a file.
//
// A response file is how a build system passes a command line that has outgrown
// what a shell and an operating system will carry -- `ARG_MAX` on POSIX, 32767
// characters to `CreateProcess` on Windows -- and it is a *textual* feature: the
// words in the file are exactly the words that would have been typed, so a
// response file can hold options, a command name and inputs alike, and nothing
// downstream knows one was used.
//
// That is also why the expansion happens before the parse rather than inside it:
// `parseArgs` is pure and reads no file, and a feature whose whole job is to
// splice words into a line belongs before the line is read. The one thing the
// two must agree about is what happens when the file cannot be read, and that is
// answered in `cli.h`: it is a usage error like any other, and `-h`/`-V` on the
// part of the line that *was* readable still outranks it.
#pragma once

#include <string>
#include <vector>

namespace minc::driver {

// One command line, after every `@file` in it has been replaced by the words that
// file holds.
struct CommandLine {
  // `argv[0]` first, exactly as it arrived: the program name is not a response
  // file and is never expanded.
  std::vector<std::string> words;
  // Empty when everything expanded. Otherwise a sentence naming the file and
  // what was wrong with it, ready for `usageError`.
  //
  // The words collected before the failure are kept, which is what lets a
  // caller report the error *and* honor a `-h` that was already on the line.
  std::string error;
};

// Expands every argument that begins with `@`, depth-first and in place, so the
// order of the resulting line is the order of the words that produced it.
//
// A null `argv` or a null element is skipped rather than dereferenced: some C
// runtimes pass a null for a program with no arguments, and a compiler should
// not be the program that crashes on it.
[[nodiscard]] CommandLine expandResponseFiles(int argc, const char* const* argv);

// The `const char*` array `parseArgs` takes, borrowing from `words`. The pointers
// are into `words`, so the result must not outlive it -- and it exists so the
// loop that builds it is written once instead of at every call site.
[[nodiscard]] std::vector<const char*> wordPointers(const std::vector<std::string>& words);

} // namespace minc::driver
