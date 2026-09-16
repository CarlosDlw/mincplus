// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
#include "builtins/dump.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "builtins/builtin.h"

namespace minc::builtins {
namespace {

// A column that pads to the widest cell in it. One helper for all of them, so a
// column cannot be padded differently from its neighbour -- and the width is
// *measured* from the rows rather than guessed at.
struct Column {
  std::string_view title;
  // Empty and zero by default, so the columns below are declared as a title and
  // nothing else: a designated initializer that lists every field would have
  // seven chances to get the order wrong for no gain.
  std::vector<std::string> cells = {};
  std::size_t width = 0;
};

void measure(Column& column) {
  column.width = column.title.size();
  for (const std::string& cell : column.cells) {
    column.width = std::max(column.width, cell.size());
  }
}

[[nodiscard]] std::string pad(const std::string& text, std::size_t width) {
  if (text.size() >= width) {
    return text;
  }
  return text + std::string(width - text.size(), ' ');
}

// What a row lowers to, as text a reader can search for: the intrinsic's name for
// the intrinsic kinds, and the intrinsic's name with the reduction for a rotate
// (`ir` is where those names are looked up, and this only has to *spell* them).
[[nodiscard]] std::string loweringText(const BuiltinInfo& row) {
  std::string text(row.lowering.name);
  switch (row.lowering.kind) {
  case Lowering::Kind::Intrinsic:
    if (row.lowering.tail == TailOperand::I1False) {
      text += "(, false)";
    }
    break;
  case Lowering::Kind::RotateLeft:
  case Lowering::Kind::RotateRight:
    text += " after urem";
    break;
  }
  return text;
}

[[nodiscard]] std::string widthsText(const BuiltinInfo& row) {
  // The widths a family accepts, printed only when the row restricts them: "any"
  // for every other row would be a column of the same word.
  return row.signature.matchedWidths == MatchedWidths::Any
             ? std::string()
             : std::string(toString(row.signature.matchedWidths));
}

} // namespace

std::string dumpBuiltins() {
  Column name{.title = "name"};
  Column signature{.title = "signature"};
  Column spelling{.title = "spelling"};
  Column status{.title = "status"};
  Column widths{.title = "widths"};
  Column lowers{.title = "lowers to"};
  Column doc{.title = "what it is"};

  for (const BuiltinInfo& row : all()) {
    name.cells.emplace_back(row.spelling);
    signature.cells.push_back(signatureText(row));
    spelling.cells.emplace_back(toString(row.spellingClass));
    status.cells.emplace_back(toString(row.status));
    widths.cells.push_back(widthsText(row));
    lowers.cells.push_back(loweringText(row));
    doc.cells.emplace_back(row.doc);
  }
  for (Column* column : {&name, &signature, &spelling, &status, &widths, &lowers, &doc}) {
    measure(*column);
  }

  std::string out = "the names the language binds, and what each one is\n\n";
  out += "  " + pad(std::string(name.title), name.width) + "  " +
         pad(std::string(signature.title), signature.width) + "  " +
         pad(std::string(spelling.title), spelling.width) + "  " +
         pad(std::string(status.title), status.width) + "  " +
         pad(std::string(widths.title), widths.width) + "  " +
         pad(std::string(lowers.title), lowers.width) + "  " + std::string(doc.title) + "\n";

  for (std::size_t i = 0; i < name.cells.size(); ++i) {
    out += "  " + pad(name.cells[i], name.width) + "  " + pad(signature.cells[i], signature.width) +
           "  " + pad(spelling.cells[i], spelling.width) + "  " +
           pad(status.cells[i], status.width) + "  " + pad(widths.cells[i], widths.width) + "  " +
           pad(lowers.cells[i], lowers.width) + "  " + doc.cells[i] + "\n";
  }

  out += "\n";
  out += "  A `prelude` name is bound in the file scope like `true`: a local shadows it, and\n";
  out += "  the file scope's own declaration of it is a redeclaration. `stable` means every\n";
  out += "  input has a defined answer (`clz(0)` is the width; a rotate's count is modulo the\n";
  out += "  width) -- which is what the language promises and what the lowering has to keep.\n";
  out += "  Anything beginning with `__builtin_` is the compiler's: `resolve` refuses a\n";
  out += "  declaration of it and the preprocessor refuses a `#define` of it.\n";
  return out;
}

} // namespace minc::builtins
