// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/utf8/bom.h"

namespace minc::support::utf8 {
namespace {

constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";

[[nodiscard]] bool startsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

} // namespace

ByteOrderMark detectByteOrderMark(std::string_view text) {
  // UTF-32 first: its little-endian mark starts with the UTF-16LE mark.
  if (startsWith(text, std::string_view("\xFF\xFE\x00\x00", 4))) {
    return ByteOrderMark::Utf32LittleEndian;
  }
  if (startsWith(text, std::string_view("\x00\x00\xFE\xFF", 4))) {
    return ByteOrderMark::Utf32BigEndian;
  }
  if (startsWith(text, std::string_view("\xFF\xFE", 2))) {
    return ByteOrderMark::Utf16LittleEndian;
  }
  if (startsWith(text, std::string_view("\xFE\xFF", 2))) {
    return ByteOrderMark::Utf16BigEndian;
  }
  if (startsWith(text, std::string_view(kUtf8Bom, 3))) {
    return ByteOrderMark::Utf8;
  }
  return ByteOrderMark::None;
}

std::size_t byteOrderMarkLength(ByteOrderMark mark) {
  switch (mark) {
  case ByteOrderMark::None:
    return 0;
  case ByteOrderMark::Utf8:
    return 3;
  case ByteOrderMark::Utf16LittleEndian:
  case ByteOrderMark::Utf16BigEndian:
    return 2;
  case ByteOrderMark::Utf32LittleEndian:
  case ByteOrderMark::Utf32BigEndian:
    return 4;
  }
  return 0;
}

bool isUnsupportedEncoding(ByteOrderMark mark) {
  switch (mark) {
  case ByteOrderMark::Utf16LittleEndian:
  case ByteOrderMark::Utf16BigEndian:
  case ByteOrderMark::Utf32LittleEndian:
  case ByteOrderMark::Utf32BigEndian:
    return true;
  case ByteOrderMark::None:
  case ByteOrderMark::Utf8:
    return false;
  }
  return false;
}

const char* toString(ByteOrderMark mark) {
  switch (mark) {
  case ByteOrderMark::None:
    return "no byte-order mark";
  case ByteOrderMark::Utf8:
    return "UTF-8";
  case ByteOrderMark::Utf16LittleEndian:
    return "UTF-16 little-endian";
  case ByteOrderMark::Utf16BigEndian:
    return "UTF-16 big-endian";
  case ByteOrderMark::Utf32LittleEndian:
    return "UTF-32 little-endian";
  case ByteOrderMark::Utf32BigEndian:
    return "UTF-32 big-endian";
  }
  return "unknown encoding";
}

} // namespace minc::support::utf8
