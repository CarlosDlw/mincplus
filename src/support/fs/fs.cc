// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "support/fs/fs.h"

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace minc::support {
namespace {

constexpr bool isSeparator(char c) {
  return c == '/' || c == '\\';
}

// Windows paths are compared without case, because the filesystems that
// matter there are case-insensitive. Everywhere else the default holds.
constexpr bool platformCaseSensitive() {
#if defined(_WIN32)
  return false;
#else
  return true;
#endif
}

[[nodiscard]] std::string foldCase(std::string text) {
  for (char& c : text) {
    const auto byte = static_cast<unsigned char>(c);
    // ASCII-only folding, deliberately: full Unicode folding needs the locale
    // and would make identity depend on it.
    c = static_cast<char>(std::tolower(byte));
  }
  return text;
}

[[nodiscard]] bool samePathText(const std::string& left, const std::string& right,
                                bool caseSensitive) {
  return caseSensitive ? left == right : foldCase(left) == foldCase(right);
}

} // namespace

std::string directoryOf(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  if (cut == std::string::npos) {
    return {};
  }
  if (cut == 0) {
    return path.substr(0, 1); // "/x" -> "/"
  }
  // "a//b" -> "a/"; keep it, `joinPath` collapses the duplicate on the way out.
  return path.substr(0, cut);
}

std::string fileNameOf(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string joinPath(const std::string& directory, const std::string& name) {
  if (name.empty() || isAbsolutePath(name)) {
    return name;
  }
  if (directory.empty() || directory == ".") {
    return name;
  }
  if (isSeparator(directory.back())) {
    return directory + name;
  }
  return directory + "/" + name;
}

std::string replaceExtension(const std::string& path, const std::string& extension) {
  const std::string name = fileNameOf(path);
  const std::string directory = directoryOf(path);
  // The search stops at the first character of the base name, so a dot in a
  // *directory* is never mistaken for the start of an extension -- and a leading
  // dot is a dotfile, which has no extension to replace.
  std::size_t cut = std::string::npos;
  for (std::size_t i = name.size(); i > 0; --i) {
    if (name[i - 1] == '.') {
      if (i > 1) {
        cut = i - 1;
      }
      break;
    }
    if (isSeparator(name[i - 1])) {
      break;
    }
  }
  const std::string base = cut == std::string::npos ? name : name.substr(0, cut);
  return joinPath(directory, base + extension);
}

bool isAbsolutePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (isSeparator(path.front())) {
    return true;
  }
  // "C:/x" or "C:\x".
  return path.size() >= 3 && path[1] == ':' && isSeparator(path[2]);
}

std::string normalizePath(const std::string& path) {
  if (path.empty()) {
    return path;
  }

  const bool absolute = isAbsolutePath(path);
  std::string prefix;
  std::string rest = path;
  if (absolute) {
    // Keep "/" or "C:/" verbatim; only the remainder is normalized.
    if (path.size() >= 3 && path[1] == ':') {
      prefix = path.substr(0, 3);
      rest = path.substr(3);
    } else {
      prefix = "/";
      rest = path.substr(1);
    }
  }

  std::vector<std::string> segments;
  std::size_t begin = 0;
  while (begin <= rest.size()) {
    const std::size_t end = rest.find_first_of("/\\", begin);
    const std::string segment =
        rest.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (segment.empty() || segment == ".") {
      // Nothing to keep.
    } else if (segment == "..") {
      // ".." cancels a preceding real segment, but never pops past the root of
      // an absolute path: "/../x" is "/x", not "/..".
      if (!segments.empty() && segments.back() != "..") {
        segments.pop_back();
      } else if (!absolute) {
        segments.push_back(segment);
      }
    } else {
      segments.push_back(segment);
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }

  std::string out = prefix;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i != 0) {
      out.push_back('/');
    }
    out += segments[i];
  }
  // An absolute path with every segment cancelled is the root, and a relative
  // path with nothing left is "." -- never the empty string, which would join
  // to a bare file name.
  if (out.empty()) {
    return absolute ? "/" : ".";
  }
  if (absolute && out.back() == '/') {
    return out;
  }
  return out;
}

std::string canonicalPath(const std::string& path) {
  std::error_code error;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
  if (!error && !resolved.empty()) {
    return resolved.generic_string();
  }
  return normalizePath(path);
}

bool isRegularFile(const std::string& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool isDirectory(const std::string& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

FileIdentity identifyFile(const std::string& path) {
  FileIdentity identity;
  identity.caseSensitive = platformCaseSensitive();
  identity.canonical = canonicalPath(path);
  identity.identified = isRegularFile(path);

#if !defined(_WIN32)
  struct stat status{};
  if (::stat(path.c_str(), &status) == 0) {
    identity.device = static_cast<std::uint64_t>(status.st_dev);
    identity.inode = static_cast<std::uint64_t>(status.st_ino);
    identity.identified = true;
  }
#endif
  // Where the inode pair is unavailable the canonical path is the identity, and
  // on Windows that comparison must ignore case.
  return identity;
}

bool operator==(const FileIdentity& left, const FileIdentity& right) {
  if (left.device != 0 && right.device != 0 && left.inode != 0 && right.inode != 0) {
    return left.device == right.device && left.inode == right.inode;
  }
  const bool caseSensitive = left.caseSensitive && right.caseSensitive;
  return samePathText(left.canonical, right.canonical, caseSensitive);
}

bool operator!=(const FileIdentity& left, const FileIdentity& right) {
  return !(left == right);
}

std::size_t FileIdentityHash::operator()(const FileIdentity& identity) const {
  if (identity.device != 0 && identity.inode != 0) {
    std::uint64_t hash = identity.device * 1099511628211ULL;
    hash ^= identity.inode + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U);
    return static_cast<std::size_t>(hash);
  }
  const std::string key =
      identity.caseSensitive ? identity.canonical : foldCase(identity.canonical);
  return std::hash<std::string>{}(key);
}

} // namespace minc::support
