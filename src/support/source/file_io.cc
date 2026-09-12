// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/source/file_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

#include "support/limits.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace minc::support {
namespace {

// Builds a filesystem path from a UTF-8 argument. On POSIX this is a
// byte-for-byte copy; on Windows it bypasses the ANSI code page so paths with
// non-ASCII characters still resolve.
[[nodiscard]] std::filesystem::path toPath(std::string_view text) {
  std::u8string utf8;
  utf8.reserve(text.size());
  for (const char byte : text) {
    utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
  }
  return std::filesystem::path(utf8);
}

[[nodiscard]] std::string tooLargeMessage(const std::string& path, std::uintmax_t size) {
  return "source file '" + path + "' is " + std::to_string(size) + " bytes; the limit is " +
         kMaxSourceBytesText;
}

} // namespace

Fallible<std::string> readFileBytes(const std::string& path) {
  if (path.empty()) {
    return makeUnexpected<std::string>("empty file path");
  }

  const std::filesystem::path filePath = toPath(path);
  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::status(filePath, ec);
  if (ec) {
    return makeUnexpected<std::string>("cannot open '" + path + "': " + ec.message());
  }
  if (std::filesystem::is_directory(status)) {
    return makeUnexpected<std::string>("cannot read '" + path + "': it is a directory");
  }

  const std::uintmax_t fileSize = std::filesystem::file_size(filePath, ec);
  if (ec) {
    return makeUnexpected<std::string>("cannot size '" + path + "': " + ec.message());
  }
  if (fileSize > static_cast<std::uintmax_t>(kMaxSourceBytes)) {
    return makeUnexpected<std::string>(tooLargeMessage(path, fileSize));
  }

  std::ifstream in(filePath, std::ios::binary);
  if (!in) {
    return makeUnexpected<std::string>("cannot open '" + path + "'");
  }

  const std::size_t length = static_cast<std::size_t>(fileSize);
  std::string bytes(length, '\0');
  if (length > 0) {
    in.read(bytes.data(), static_cast<std::streamsize>(length));
    // A short read means the file shrank (or the medium failed) between the
    // size check and the read; surface it rather than returning partial input.
    if (in.gcount() != static_cast<std::streamsize>(length)) {
      return makeUnexpected<std::string>("cannot read '" + path + "': incomplete read");
    }
  }
  return bytes;
}

Fallible<std::string> readStdinBytes() {
#if defined(_WIN32)
  // _O_BINARY disables the CRT's CRLF translation. Failing to set it is not
  // worth an error: the read still works, it just sees translated bytes.
  (void)_setmode(_fileno(stdin), _O_BINARY);
#endif

  constexpr std::size_t kChunkBytes = std::size_t{64} * 1024U;
  std::string bytes;
  std::array<char, kChunkBytes> buffer{};
  while (true) {
    const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), stdin);
    if (count > 0) {
      if (count > kMaxSourceBytes - bytes.size()) {
        return makeUnexpected<std::string>("standard input exceeds the limit of " +
                                           std::string(kMaxSourceBytesText));
      }
      bytes.append(buffer.data(), count);
    }
    if (count < buffer.size()) {
      if (std::ferror(stdin) != 0) {
        return makeUnexpected<std::string>("cannot read standard input");
      }
      break; // short read means end of input
    }
  }
  return bytes;
}

} // namespace minc::support
