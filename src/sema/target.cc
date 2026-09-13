// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/target.h"

#include <string_view>

namespace minc::sema {

std::string_view toString(Target target) {
  switch (target) {
  case Target::SystemVAmd64:
    return "systemv-amd64";
  case Target::WindowsX64:
    return "windows-x64";
  }
  return "unknown";
}

std::optional<Target> targetFromName(std::string_view name) {
  // The two rows, walked from the enumerators so a target added to the enum
  // without a name here is caught by the test that round-trips every one.
  for (const Target target : {Target::SystemVAmd64, Target::WindowsX64}) {
    if (toString(target) == name) {
      return target;
    }
  }
  return std::nullopt;
}

TargetInfo targetInfo(Target target) {
  TargetInfo info;
  info.target = target;
  switch (target) {
  case Target::SystemVAmd64:
    info.pointerBits = 64;
    info.longBits = 64;
    info.longDoubleBits = 80;
    break;
  case Target::WindowsX64:
    // LLP64: `long` is 32 bits even though pointers are 64, and `long double`
    // is what MSVC calls `double`. Both are exactly the differences that make
    // reading the width off the compiler's own machine wrong.
    info.pointerBits = 64;
    info.longBits = 32;
    info.longDoubleBits = 64;
    break;
  }
  return info;
}

} // namespace minc::sema
