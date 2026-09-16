// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "sema/dump.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ast/node.h"
#include "parse/syntax_kind.h"
#include "sema/type.h"
#include "sema/type_store.h"
#include "sema/typed_ast.h"
#include "support/consteval/const_int.h"
#include "support/intern/interner.h"
#include "support/span/span.h"

namespace minc::sema {
namespace {

[[nodiscard]] std::string lexeme(std::string_view text, std::size_t maxBytes) {
  if (text.size() <= maxBytes) {
    return std::string(text);
  }
  return std::string(text.substr(0, maxBytes)) + "...";
}

[[nodiscard]] std::string rangeText(const support::Span& span) {
  return std::to_string(span.begin) + ".." + std::to_string(span.end);
}

[[nodiscard]] std::string valueText(const support::ConstInt& value) {
  if (value.isUnsigned) {
    return std::to_string(value.bits);
  }
  return std::to_string(value.signedValue());
}

// The facts an expression carries, in one short suffix, so a reader can see at
// the node where a conversion happened that the value was known. Deliberately
// not printed for a node that is not an expression: a declaration's "lvalue"
// would be a lie.
// One access, in the short form a reader scans for. `object` and `foreign` are
// the whole answer the record gives about what the optimizer may assume, so they
// are printed as words rather than as numbers.
//
// The extent is printed between the provenance and the type, and only when there
// is one: `count 4` is the checked build's bounds check for `a[i]`, `length` is
// the one for `s[i]` and names no number because the number is the descriptor's,
// and an `*p` prints neither -- `unknown` is the record saying what it cannot
// see rather than a value to be read (`checks.md`).
[[nodiscard]] std::string accessText(const AccessObligation& access, const TypeStore& types) {
  std::string out = " [access ";
  out += std::string(toString(access.kind));
  out += ' ';
  out += std::string(toString(access.provenance));
  out += ' ';
  if (access.extentKind != ExtentKind::Unknown) {
    out += std::string(toString(access.extentKind));
    if (access.extentKind == ExtentKind::Count) {
      out += ' ';
      out += std::to_string(access.extent);
    }
    out += ' ';
  }
  out += types.spelling(access.type);
  out += ']';
  return out;
}

[[nodiscard]] std::string infoText(const ExprInfo& info, const TypeStore& types) {
  std::string out;
  if (info.isLvalue) {
    out += " [lvalue]";
  }
  if (info.isConstant) {
    out += " [const]";
  }
  if (info.hasIntValue) {
    out += " =" + valueText(info.value);
  }
  if (info.opType.valid() && types.known(info.opType)) {
    // The type a compound assignment operates at when it is not the assignment's
    // own type. Printed because it is the one fact the tree cannot show: the
    // node's type is the store's, and `x <<= 9` on a `u16` works at `i32`.
    out += " [op=" + types.spelling(info.opType) + "]";
  }
  return out;
}

} // namespace

std::string dumpTypeStore(const TypeStore& types) {
  std::string out;
  out += "# types " + std::to_string(types.count()) + "  target " + types.target().name() +
         "  long=" + std::to_string(types.target().longBits) +
         "  pointer=" + std::to_string(types.target().pointerBits) + "\n";
  for (std::uint32_t i = 0; i < types.count(); ++i) {
    const TypeId id{i};
    const Type& type = types.get(id);
    out += "  #" + std::to_string(i);
    out += "  " + types.spelling(id);
    out += "  " + std::string(toString(type.kind));
    // A size of zero is meaningful (`void` has no object representation), so it
    // is printed rather than hidden.
    out += "  size=" + std::to_string(types.sizeOf(id));
    out += "  align=" + std::to_string(types.alignOf(id));
    if (type.kind == TypeKind::Function) {
      out += "  params=" + std::to_string(type.paramCount);
    }
    out += '\n';
  }
  return out;
}

std::string dumpTypedFile(const ast::LoweredFile& file, const TypedFile& typed,
                          const TypeStore& types, TypedDumpOptions options) {
  std::string out;

  if (options.functions) {
    out += "# functions " + std::to_string(typed.functionTable.size()) + "\n";
    for (const FunctionInfo& info : typed.functionTable) {
      out += "  #" + std::to_string(info.decl.index) + "  ";
      out += types.spelling(info.returnType);
      out += "  ";
      out += "(" + std::to_string(types.get(info.functionType).paramCount) + " param(s))";
      out += info.body.valid() ? "  body" : "  no-body";
      out += '\n';
    }
  }

  if (!options.nodes) {
    return out;
  }

  // The conversions, before the tree: they are a short list that says what the
  // tree will do between an operand and its consumer, and a reader looking for
  // "where does a conversion happen" should not have to read every node for it.
  out += "# coercions " + std::to_string(typed.coercions().size()) + "\n";
  for (const Coercion& coercion : typed.coercions()) {
    out += "  " + std::to_string(coercion.consumer.index) + ":" + std::to_string(coercion.operand) +
           "  " + std::to_string(coercion.node.index) + "  " + types.spelling(coercion.from) +
           " -> " + types.spelling(coercion.to) + "\n";
  }

  // The accesses, before the tree, for the same reason the conversions are: it
  // is the short list that says what memory the tree touches through a pointer,
  // and a reader looking for "what may be assumed here" should not have to read
  // every node for it.
  out += "# accesses " + std::to_string(typed.accesses().size()) + "\n";
  for (const AccessObligation& access : typed.accesses()) {
    out += "  " + std::to_string(access.place.index) + "  " + std::string(toString(access.kind)) +
           "  " + std::string(toString(access.provenance)) + "  " + types.spelling(access.type) +
           "\n";
  }

  out += "# typed " + std::to_string(file.nodeCount()) + " nodes\n";

  // An explicit stack for the same reason `ast::dumpAst` uses one: a deep tree
  // is a long dump and not a deep call.
  struct Frame {
    ast::AstId id;
    std::uint32_t depth;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{file.root(), 0});
  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();

    const ast::Node& node = file.at(frame.id);
    out += std::to_string(frame.id.index);
    out += ' ';
    out.append(static_cast<std::size_t>(frame.depth) * 2, ' ');
    out += parse::toString(node.kind);

    if (node.name != support::kInvalidSym) {
      out += " <";
      out += file.spellingOf(node);
      out += ">";
    } else if (node.isToken()) {
      out += " \"";
      out += lexeme(file.spellingOf(node), options.maxTextBytes);
      out += '"';
    }

    const TypeId type = typed.typeOf(frame.id);
    if (type.valid()) {
      out += "  : ";
      out += types.spelling(type);
      out += infoText(typed.infoOf(frame.id), types);
    }
    if (const AccessObligation* access = typed.accessAt(frame.id)) {
      out += accessText(*access, types);
    }

    out += "  ";
    out += rangeText(node.unit);
    if (node.inError) {
      out += "  [error region]";
    }
    out += '\n';

    const std::span<const ast::AstId> kids = file.childrenOf(frame.id);
    for (std::size_t i = kids.size(); i > 0; --i) {
      stack.push_back(Frame{kids[i - 1], frame.depth + 1});
    }
  }
  return out;
}

} // namespace minc::sema
