// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The one table of refusal codes, and the enumeration derived from it.
//
// Same shape as every stage above: one table is the list, `allDiagnosticCodes`
// is derived from it, and a test asserts both that every code has a row and that
// every row is reachable. Adding a code without a name is therefore a failing
// test rather than a diagnostic that prints its number.
#include "backend/codegen.h"

namespace minc::backend {

namespace {
// NOLINTBEGIN(readability-identifier-naming): table names follow the project's
// convention for the other stages' tables.
constexpr CodegenDiagnosticCodeInfo kDiagnosticCodeInfos[] = {
    {CodegenDiagnosticCode::TargetUnavailable, "codegen-target-unavailable"},
    {CodegenDiagnosticCode::EmitUnsupported, "codegen-emit-unsupported"},
    {CodegenDiagnosticCode::LinkerNotFound, "codegen-linker-not-found"},
    {CodegenDiagnosticCode::LinkerUnavailable, "codegen-linker-unavailable"},
    {CodegenDiagnosticCode::LinkFailed, "codegen-link-failed"},
    {CodegenDiagnosticCode::ObjectWriteFailed, "codegen-object-write-failed"},
    {CodegenDiagnosticCode::Internal, "codegen-internal"},
};
// NOLINTEND(readability-identifier-naming)
} // namespace

const char* nameOf(CodegenDiagnosticCode code) {
  for (const CodegenDiagnosticCodeInfo& info : kDiagnosticCodeInfos) {
    if (info.code == code) {
      return info.name;
    }
  }
  return "codegen-internal";
}

std::string_view toString(CodegenDiagnosticCode code) {
  return nameOf(code);
}

const std::vector<CodegenDiagnosticCode>& allDiagnosticCodes() {
  static const std::vector<CodegenDiagnosticCode> codes = [] {
    std::vector<CodegenDiagnosticCode> out;
    out.reserve(std::size(kDiagnosticCodeInfos));
    for (const CodegenDiagnosticCodeInfo& info : kDiagnosticCodeInfos) {
      out.push_back(info.code);
    }
    return out;
  }();
  return codes;
}

const char* toString(EmitKind kind) {
  switch (kind) {
  case EmitKind::Object:
    return "object";
  case EmitKind::Assembly:
    return "assembly";
  case EmitKind::None:
    return "none";
  }
  return "none";
}

const char* toString(OptLevel level) {
  switch (level) {
  case OptLevel::O0:
    return "O0";
  case OptLevel::O1:
    return "O1";
  case OptLevel::O2:
    return "O2";
  case OptLevel::O3:
    return "O3";
  case OptLevel::Os:
    return "Os";
  case OptLevel::Oz:
    return "Oz";
  }
  return "O0";
}

std::optional<OptLevel> optLevelFromName(std::string_view name) {
  if (name == "O0" || name == "0") {
    return OptLevel::O0;
  }
  if (name == "O1" || name == "1") {
    return OptLevel::O1;
  }
  if (name == "O2" || name == "2") {
    return OptLevel::O2;
  }
  if (name == "O3" || name == "3") {
    return OptLevel::O3;
  }
  if (name == "Os" || name == "s") {
    return OptLevel::Os;
  }
  if (name == "Oz" || name == "z") {
    return OptLevel::Oz;
  }
  return std::nullopt;
}

// `--emit=` names what *this stage* writes. `exe` is not one of them: an
// executable is an object plus a link, which is the driver's business and not a
// file type LLVM has. The driver therefore reads `exe` itself and asks for an
// object; only `obj`/`asm`/`none` reach here.
std::optional<EmitKind> emitKindFromName(std::string_view name) {
  if (name == "obj" || name == "object") {
    return EmitKind::Object;
  }
  if (name == "asm" || name == "assembly") {
    return EmitKind::Assembly;
  }
  if (name == "none") {
    return EmitKind::None;
  }
  return std::nullopt;
}

} // namespace minc::backend
