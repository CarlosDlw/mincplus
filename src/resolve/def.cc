// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "resolve/def.h"

#include <string_view>

#include "resolve/map.h"

namespace minc::resolve {

std::string_view toString(Namespace value) {
  switch (value) {
  case Namespace::Ordinary:
    return "ordinary";
  case Namespace::Tag:
    return "tag";
  case Namespace::Label:
    return "label";
  case Namespace::Member:
    return "member";
  }
  return "unknown";
}

std::string_view toString(DefKind value) {
  switch (value) {
  case DefKind::Function:
    return "fn";
  case DefKind::Variable:
    return "let";
  case DefKind::Constant:
    return "const";
  case DefKind::Parameter:
    return "param";
  case DefKind::TypeAlias:
    return "type";
  }
  return "unknown";
}

std::string_view toString(Linkage value) {
  switch (value) {
  case Linkage::None:
    return "none";
  case Linkage::Internal:
    return "internal";
  case Linkage::External:
    return "external";
  }
  return "unknown";
}

std::string_view toString(ScopeKind value) {
  switch (value) {
  case ScopeKind::File:
    return "file";
  case ScopeKind::Function:
    return "function";
  case ScopeKind::Block:
    return "block";
  case ScopeKind::FunctionPrototype:
    return "prototype";
  case ScopeKind::Loop:
    return "loop";
  case ScopeKind::Switch:
    return "switch";
  }
  return "unknown";
}

std::string_view toString(UnresolvedReason value) {
  switch (value) {
  case UnresolvedReason::None:
    return "resolved";
  case UnresolvedReason::NotFound:
    return "not-found";
  case UnresolvedReason::WrongNamespace:
    return "wrong-namespace";
  case UnresolvedReason::InErrorRegion:
    return "in-error";
  case UnresolvedReason::LimitReached:
    return "limit";
  }
  return "unknown";
}

} // namespace minc::resolve
