// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Strict UTF-8 validation (rejects overlong, surrogates, >U+10FFFF).
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace minc::support::utf8 {

[[nodiscard]] bool isValid(std::string_view text);
[[nodiscard]] std::optional<std::size_t> firstInvalidOffset(std::string_view text);

} // namespace minc::support::utf8
