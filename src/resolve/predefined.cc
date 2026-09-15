// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/predefined.h"

namespace minc::resolve {

std::string_view toString(Predefined value) {
  switch (value) {
  case Predefined::None:
    return "none";
  case Predefined::False:
    return "false";
  case Predefined::True:
    return "true";
  case Predefined::Null:
    return "null";
  }
  return "unknown";
}

} // namespace minc::resolve
