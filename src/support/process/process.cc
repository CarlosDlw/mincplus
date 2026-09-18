// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The platform half of `support/process`: `posix_spawn`/`waitpid` here,
// `CreateProcessW`/`GetExitCodeProcess` below the split. Read
// `include/support/process/process.h` first -- it carries the argument for why
// this layer exists at all.
#include "support/process/process.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
// windows.h after our own headers, so it cannot shadow anything we declare, and
// lean-and-mean so the translation unit stays cheap to compile. NOMINMAX keeps
// the min/max macros out of std::min/max. Both are the convention `support/term`
// already follows.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// The parent's environment, handed to the child unchanged. Declared rather than
// taken from `<unistd.h>`: glibc exposes `environ` only under `_GNU_SOURCE`, and
// this project builds with extensions off (`-std=c++20`, `cmake/minc_cxx.cmake`),
// while POSIX itself names this symbol in `<unistd.h>` -- so the declaration is
// stated where it is used. `extern "C"` because the object is C's, under exactly
// this name; the `<unistd.h>` declaration, where a libc has one, is C's too.
extern "C" char** environ;
#endif

namespace minc::support {

#if defined(_WIN32)
namespace {

// UTF-8 in, UTF-16 out, because every Win32 entry point that names a program
// takes wide characters and the strings this compiler carries are UTF-8
// (`support/source/file_io` makes the same conversion for paths). An empty result
// means the text could not be converted, which the caller reports as a failure to
// start rather than passing an empty name to the platform.
[[nodiscard]] std::wstring toUtf16(std::string_view text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

[[nodiscard]] std::string toUtf8(std::wstring_view text) {
  if (text.empty()) {
    return std::string();
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string narrow(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size,
                        nullptr, nullptr);
  return narrow;
}

// The platform's own words for a failure, with the code appended. The words are
// the sentence a user reads; the code is what a reader can search for when the
// sentence is in a language they cannot read -- `FormatMessageW` answers in the
// machine's language, and a diagnostic is not the place to disagree with it.
[[nodiscard]] std::string describeWindowsError(unsigned long code) {
  std::string message;
  wchar_t* buffer = nullptr;
  const unsigned long length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_MAX_WIDTH_MASK,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length != 0 && buffer != nullptr) {
    message = toUtf8(std::wstring_view(buffer, static_cast<std::size_t>(length)));
    ::LocalFree(buffer);
  }
  if (message.empty()) {
    message = "the platform did not describe the failure";
  }
  constexpr const char* kDigits = "0123456789ABCDEF";
  std::string hex(8, '0');
  for (std::size_t index = 0; index < hex.size(); ++index) {
    const unsigned long shift = 28UL - (4UL * static_cast<unsigned long>(index));
    const auto nibble = static_cast<std::size_t>((code >> shift) & 0xFUL);
    hex[index] = kDigits[nibble];
  }
  return message + " (0x" + hex + ")";
}

// Whether an argument has to be quoted on the way to `CreateProcessW`. The set is
// the one the C runtime's own parser treats specially -- whitespace, a quote, and
// the metacharacters -- so a quoted argument arrives as the one word it was.
[[nodiscard]] bool needsQuotes(std::string_view argument) {
  if (argument.empty()) {
    return true;
  }
  return argument.find_first_of("\t \"&'()*<>\\`^|\n") != std::string_view::npos;
}

// One argument, quoted by the rules the C runtime's `argv` parser undoes: a run
// of backslashes is doubled when it is followed by a quote or by the end of the
// string, and a quote is escaped with one more backslash. The algorithm is
// Microsoft's (`Parsing C command-line arguments`), and writing it out is the
// price of not having an `argv` array to hand the platform.
[[nodiscard]] std::string quoteArgument(std::string_view argument) {
  std::string quoted;
  quoted.push_back('"');
  while (!argument.empty()) {
    const std::size_t firstOther = argument.find_first_not_of('\\');
    const std::size_t backslashes = firstOther;
    if (firstOther == std::string_view::npos) {
      // The rest is backslashes: all of them are doubled, and the closing quote
      // is what they are being escaped for.
      quoted.append(argument.size() * 2, '\\');
      break;
    }
    if (argument[firstOther] == '"') {
      quoted.append((backslashes * 2) + 1, '\\');
      quoted.push_back('"');
    } else {
      quoted.append(backslashes, '\\');
      quoted.push_back(argument[firstOther]);
    }
    argument.remove_prefix(firstOther + 1);
  }
  quoted.push_back('"');
  return quoted;
}

// The whole command line: `argv[0]` first (the program's own path, as the shell
// would have given it), then the arguments, each quoted only when it has to be.
[[nodiscard]] std::string commandLineOf(const std::string& program,
                                        const std::vector<std::string>& arguments) {
  std::string command;
  const auto append = [&command](std::string_view word) {
    if (needsQuotes(word)) {
      command += quoteArgument(word);
    } else {
      command += word;
    }
    command.push_back(' ');
  };
  append(program);
  for (const std::string& argument : arguments) {
    append(argument);
  }
  return command;
}

} // namespace

ProcessOutcome runAndWait(const std::string& program, const std::vector<std::string>& arguments) {
  ProcessOutcome outcome;
  if (program.empty()) {
    outcome.error = "no program to run";
    return outcome;
  }

  const std::wstring application = toUtf16(program);
  const std::wstring command = toUtf16(commandLineOf(program, arguments));
  if (application.empty() || command.empty()) {
    outcome.error = "the program path is not text this platform can name";
    return outcome;
  }

  STARTUPINFOW startup;
  std::memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process;
  std::memset(&process, 0, sizeof(process));

  // Writable, because `CreateProcessW` is allowed to modify the command line in
  // place; NUL-terminated for the same reason it exists.
  std::vector<wchar_t> commandBuffer(command.begin(), command.end());
  commandBuffer.push_back(L'\0');

  // `lpApplicationName` names the file to load, so no `PATH` search and no
  // `PATHEXT` guessing happens -- the caller resolved the path already. Handles
  // are inherited and `STARTF_USESTDHANDLES` is deliberately not set: the child
  // attaches to this process's console, which is what makes the program's own
  // output appear where the user is looking.
  if (::CreateProcessW(application.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE, 0,
                       nullptr, nullptr, &startup, &process) == 0) {
    outcome.error = describeWindowsError(::GetLastError());
    return outcome;
  }
  outcome.started = true;

  const unsigned long waited = ::WaitForSingleObject(process.hProcess, INFINITE);
  const unsigned long waitError = (waited == WAIT_FAILED) ? ::GetLastError() : 0UL;
  unsigned long status = 0;
  const BOOL got = ::GetExitCodeProcess(process.hProcess, &status);
  const unsigned long statusError = (got == 0) ? ::GetLastError() : 0UL;
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);

  if (waited == WAIT_FAILED) {
    // It ran: this is not "nothing ran", it is "the ending is unknown", which is
    // why `started` stays true and `exited` stays false.
    outcome.error = describeWindowsError(waitError);
    return outcome;
  }
  if (got == 0) {
    outcome.error = describeWindowsError(statusError);
    return outcome;
  }
  // A code above `0x7FFFFFFF` is not a status the program chose: it is an
  // NTSTATUS the platform produced when an exception ended the process
  // (`0xC0000005` for an access violation, `0xC000001D` for the `ud2` that
  // `__builtin_trap` lowers to, `0xC000013A` for a Ctrl-C). The program did not
  // exit, and saying otherwise would report a crash as a status.
  if (status >= 0xC0000000UL) {
    return outcome;
  }
  outcome.exited = true;
  outcome.status = static_cast<int>(status);
  return outcome;
}

#else

ProcessOutcome runAndWait(const std::string& program, const std::vector<std::string>& arguments) {
  ProcessOutcome outcome;
  if (program.empty()) {
    outcome.error = "no program to run";
    return outcome;
  }

  // `argv[0]` is the program's own path, exactly as if it had been invoked
  // directly: a program that prints `argv[0]` prints what the shell would have
  // given it, and one that re-executes itself finds itself.
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(const_cast<char*>(program.c_str()));
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  // `posix_spawn` rather than `fork` + `exec`: one call that creates the process
  // *and* returns why it could not (`ENOENT` for a missing file, `EACCES` for one
  // without the execute bit, `ENOEXEC` for an image this machine cannot load),
  // which is the sentence the caller has to print. It also has none of `fork`'s
  // interactions with a process that may already be threaded, and none of the
  // between-fork-and-exec restrictions that come with them.
  pid_t child = 0;
  const int failed = ::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.data(), environ);
  if (failed != 0) {
    outcome.error = std::strerror(failed);
    return outcome;
  }
  outcome.started = true;

  // EINTR and nothing else: a wait that was interrupted has not learned anything,
  // so it is asked again. Every other failure means the ending is unknown, which
  // `started` true and `exited` false already say.
  int status = 0;
  pid_t waited = ::waitpid(child, &status, 0);
  while (waited == -1 && errno == EINTR) {
    waited = ::waitpid(child, &status, 0);
  }
  if (waited == -1) {
    outcome.error = std::strerror(errno);
    return outcome;
  }
  // Exited, so the status is the program's own. `WIFSIGNALED` (a trap, a
  // segfault, a `kill`) leaves `exited` false and this layer says so -- which is
  // the question `llvm::sys::Wait` could only answer with a negative number.
  if (WIFEXITED(status)) {
    outcome.exited = true;
    outcome.status = WEXITSTATUS(status);
  }
  return outcome;
}

#endif

} // namespace minc::support
