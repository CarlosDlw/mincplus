// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/target.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minc::sema {
namespace {
// The canonical spelling, plus the aliases LLVM recognizes for the same machine.
//
// Two names for one architecture is what this file argues against, so the rule
// is stated precisely instead of "canonical only": the *identity* has one
// spelling (`toString`, and therefore `Triple::text`, the dump, and what
// `codegen` hands LLVM), and the aliases exist on the **input** side, where the
// alternative is refusing a name the platform's own tools print. See
// `parseTriple` in `target.h` for why `arm64` in particular has to parse.
bool archFromName(std::string_view name, Arch& out) {
  struct Alias {
    std::string_view name;
    Arch arch;
  };
  // Ordered by architecture so the canonical spelling is the one listed first,
  // which is also the order `toString` prints them in.
  static constexpr Alias kArchAliases[] = {
      {"x86_64", Arch::x86_64}, {"amd64", Arch::x86_64},    {"aarch64", Arch::aarch64},
      {"arm64", Arch::aarch64}, {"riscv64", Arch::riscv64}, {"i386", Arch::i386},
      {"i486", Arch::i386},     {"i586", Arch::i386},       {"i686", Arch::i386},
  };
  for (const Alias& alias : kArchAliases) {
    if (alias.name == name) {
      out = alias.arch;
      return true;
    }
  }
  return false;
}

bool osFromName(std::string_view name, OsFamily& out) {
  for (const OsFamily os :
       {OsFamily::linux, OsFamily::windows, OsFamily::darwin, OsFamily::freebsd}) {
    if (toString(os) == name) {
      out = os;
      return true;
    }
  }
  return false;
}

bool envFromName(std::string_view name, Env& out) {
  for (const Env env : {Env::gnu, Env::musl, Env::msvc}) {
    if (toString(env) == name) {
      out = env;
      return true;
    }
  }
  return false;
}

// Which environments an OS can have, and why: an environment is not decoration
// where it changes the ABI. `msvc` and `gnu` disagree about `long double` on
// Windows, so Windows has no default and the two would be a silent difference;
// Darwin has exactly one; a C library flavour is what `gnu`/`musl` mean on the
// Unices, and neither changes the type widths this stage states.
bool envAllowed(OsFamily os, Env env) {
  switch (os) {
  case OsFamily::windows:
    return env == Env::msvc || env == Env::gnu;
  case OsFamily::darwin:
    return env == Env::none;
  case OsFamily::linux:
    return env == Env::none || env == Env::gnu || env == Env::musl;
  case OsFamily::freebsd:
    return env == Env::none || env == Env::gnu;
  }
  return false;
}

std::vector<std::string_view> split(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t dash = text.find('-', start);
    if (dash == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, dash - start));
    start = dash + 1;
  }
}

// The one parser. `error` is filled with a sentence a diagnostic can print, or
// left empty on success -- one function rather than two so the refusal a user
// reads and the refusal a test asserts cannot describe different parses.
std::optional<Triple> parse(std::string_view name, std::string& error) {
  const std::vector<std::string_view> parts = split(name);
  if (parts.size() < 3 || parts.size() > 4) {
    error = "not a triple: expected `arch-vendor-os` or `arch-vendor-os-env`";
    return std::nullopt;
  }
  if (parts[0].empty() || parts[1].empty() || parts[2].empty() ||
      (parts.size() == 4 && parts[3].empty())) {
    error = "not a triple: a component is empty";
    return std::nullopt;
  }

  Triple triple;
  if (!archFromName(parts[0], triple.arch)) {
    error = "unknown architecture `" + std::string(parts[0]) + "`";
    return std::nullopt;
  }
  if (!osFromName(parts[2], triple.os)) {
    // The commonest way to reach this is a three-component triple written
    // `arch-os-env`, so the sentence names the shape rather than just the word.
    error = "unknown OS `" + std::string(parts[2]) +
            "` (a triple is `arch-vendor-os[-env]`; write the vendor out)";
    return std::nullopt;
  }
  if (parts.size() == 4 && !envFromName(parts[3], triple.env)) {
    error = "unknown environment `" + std::string(parts[3]) + "`";
    return std::nullopt;
  }
  if (!envAllowed(triple.os, triple.env)) {
    if (triple.env == Env::none) {
      error = "`" + std::string(toString(triple.os)) +
              "` has no default environment; name one (`msvc` or `gnu`)";
    } else {
      error = "`" + std::string(toString(triple.env)) + "` is not an environment `" +
              std::string(toString(triple.os)) + "` has";
    }
    return std::nullopt;
  }

  triple.vendor = std::string(parts[1]);
  // The canonical spelling: the components in order, the environment omitted when
  // the OS has only one, so the same target has one string.
  triple.text = std::string(toString(triple.arch)) + "-" + triple.vendor + "-" +
                std::string(toString(triple.os));
  if (triple.env != Env::none) {
    triple.text += "-";
    triple.text += toString(triple.env);
  }
  return triple;
}

} // namespace

std::string_view toString(Arch arch) {
  switch (arch) {
  case Arch::x86_64:
    return "x86_64";
  case Arch::aarch64:
    return "aarch64";
  case Arch::riscv64:
    return "riscv64";
  case Arch::i386:
    return "i386";
  }
  return "unknown";
}

std::string_view toString(OsFamily os) {
  switch (os) {
  case OsFamily::linux:
    return "linux";
  case OsFamily::windows:
    return "windows";
  case OsFamily::darwin:
    return "darwin";
  case OsFamily::freebsd:
    return "freebsd";
  }
  return "unknown";
}

std::string_view toString(Env env) {
  switch (env) {
  case Env::none:
    // Not a component the canonical spelling prints: an OS with one environment
    // omits it. The word exists so a diagnostic can name the absence.
    return "none";
  case Env::gnu:
    return "gnu";
  case Env::musl:
    return "musl";
  case Env::msvc:
    return "msvc";
  }
  return "unknown";
}

std::optional<Triple> parseTriple(std::string_view name) {
  std::string error;
  return parse(name, error);
}

std::string targetRefusal(std::string_view name) {
  std::string error;
  if (parse(name, error).has_value()) {
    return {};
  }
  return error;
}

std::optional<TargetInfo> targetInfo(const Triple& triple) {
  if (!envAllowed(triple.os, triple.env)) {
    return std::nullopt;
  }
  TargetInfo info;
  info.triple = triple;
  // The architecture states the pointer width and the `long double` format;
  // `-Wswitch` makes a new architecture a build error rather than a silent 64.
  switch (triple.arch) {
  case Arch::x86_64:
    info.pointerBits = 64;
    info.longDoubleBits = 80;
    break;
  case Arch::i386:
    info.pointerBits = 32;
    info.longDoubleBits = 80;
    break;
  case Arch::aarch64:
  case Arch::riscv64:
    info.pointerBits = 64;
    info.longDoubleBits = 128;
    break;
  }
  // ... and the OS states `long`, which is the one width that follows a rule
  // rather than a number: LP64/ILP32 on the Unices (the pointer width), 32 on
  // Windows (LLP64 at 64 bits, ILP32 at 32).
  switch (triple.os) {
  case OsFamily::windows:
    info.longBits = 32;
    // MSVC calls `long double` a `double`; MinGW keeps the x87 80-bit format.
    if (triple.env == Env::msvc) {
      info.longDoubleBits = 64;
    }
    break;
  case OsFamily::darwin:
    info.longBits = info.pointerBits;
    // `long double` is `double` on Darwin, on both architectures it runs.
    info.longDoubleBits = 64;
    break;
  case OsFamily::linux:
  case OsFamily::freebsd:
    info.longBits = info.pointerBits;
    break;
  }
  return info;
}

std::optional<TargetInfo> targetFromName(std::string_view name) {
  const std::optional<Triple> triple = parseTriple(name);
  if (!triple.has_value()) {
    return std::nullopt;
  }
  return targetInfo(*triple);
}

std::string_view hostTriple() {
  return kHostTriple;
}

bool sameAbi(const TargetInfo& left, const TargetInfo& right) {
  // The three components that select an ABI, and the six widths the table
  // derives from them. Comparing the widths as well is not redundant: it is what
  // makes this function answer whether the two *objects* agree, so a caller
  // cannot be misled by a triple that parsed but has no row.
  return left.triple.arch == right.triple.arch && left.triple.os == right.triple.os &&
         left.triple.env == right.triple.env && left.pointerBits == right.pointerBits &&
         left.longBits == right.longBits && left.longDoubleBits == right.longDoubleBits &&
         left.shortBits == right.shortBits && left.intBits == right.intBits &&
         left.charBits == right.charBits;
}

TargetInfo defaultTarget() {
  const std::optional<TargetInfo> info = targetFromName(kDefaultTriple);
  // Unreachable: `kDefaultTriple` is either the host CMake generated -- which is
  // a triple this file states, and CMake says so at configure time -- or
  // `kFallbackTriple`, which is one by construction. A test asserts both. A
  // default-constructed `TargetInfo` is the answer rather than an assertion
  // because this function is called from a constructor's default argument, where
  // aborting would be a worse failure than a target whose name is empty.
  return info.value_or(TargetInfo{});
}

} // namespace minc::sema
