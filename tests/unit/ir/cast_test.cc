// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// A cast in the module: the one guarded conversion, the fold of a constant, and
// the refusal of one the destination cannot hold.
//
// The checker decides *whether* a pair may cast (`cast_test.cc` in `sema`); this
// file is the other half -- what the module looks like when it does, and the
// single row of the matrix whose shape is more than an instruction.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "casts/universe.h"
#include "ir/ir_fixture.h"
#include "sema/convert.h"
#include "sema/type.h"
#include "sema/type_store.h"

namespace minc::ir {
namespace {

// --- reading the module ---------------------------------------------------------
//
// The module is text, and the question every test here asks of it is "which cast
// instructions does this function hold". Two details make that readable:
//
//   * `bool` is an `i1` as a *value* and a byte as an *object*, so a `bool`
//     parameter or binding carries `zext i1 ... to i8` and `trunc i8 ... to i1`
//     instructions that are the *storage* form and not a conversion the source
//     wrote. They are named `bool.store`/`bool.load` (`expr.cc`), which is what
//     the filter below reads.
//   * a conversion is named after its own opcode (`%sext = sext ...`), so a match
//     on `" = sext "` is a match on the conversion and on nothing else.
constexpr std::string_view kCastOpcodes[] = {"sext",    "zext",     "trunc",   "sitofp",
                                             "uitofp",  "fptosi",   "fptoui",  "fpext",
                                             "fptrunc", "ptrtoint", "inttoptr"};

[[nodiscard]] std::string bodyOf(const std::string& module, const std::string& name) {
  const std::size_t at = module.find("@" + name + "(");
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t start = module.find('\n', at);
  const std::size_t end = module.find("\n}", start);
  if (start == std::string::npos || end == std::string::npos) {
    return {};
  }
  return module.substr(start, end - start);
}

// Lines, so a match can be checked against the name on its left-hand side.
[[nodiscard]] std::vector<std::string_view> linesOf(const std::string& body) {
  std::vector<std::string_view> out;
  for (std::size_t at = 0; at < body.size();) {
    const std::size_t end = body.find('\n', at);
    const std::size_t stop = end == std::string::npos ? body.size() : end;
    out.emplace_back(body.data() + at, stop - at);
    at = stop + 1;
  }
  return out;
}

// A line that *is* the conversion: has the opcode, and is not the storage form of
// a `bool`.
[[nodiscard]] bool hasConversionOpcode(const std::string& body, std::string_view opcode) {
  const std::string needle = " = " + std::string(opcode) + " ";
  for (const std::string_view line : linesOf(body)) {
    const std::size_t at = line.find(needle);
    if (at == std::string_view::npos) {
      continue;
    }
    if (line.substr(0, at).find("bool.") != std::string_view::npos) {
      continue;
    }
    return true;
  }
  return false;
}

[[nodiscard]] std::string_view anyCastOpcode(const std::string& body) {
  for (const std::string_view opcode : kCastOpcodes) {
    if (hasConversionOpcode(body, opcode)) {
      return opcode;
    }
  }
  return {};
}

[[nodiscard]] std::size_t countOccurrences(const std::string& text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

// A runtime float, so the conversion cannot be folded away: the value has to come
// out of memory for the guard to exist at all.
[[nodiscard]] std::string runtimeDoubleCast(const std::string& type) {
  return "fn i32 main()\n"
         "{\n"
         "  let x: f64 = 1.5;\n"
         "  let y: f64 = 2.5;\n"
         "  let z: f64 = x + y;\n"
         "  let a: " +
         type + " = z as " + type +
         ";\n"
         "  return 0;\n"
         "}\n";
}

TEST(IrCastTest, TheGuardTestsTheOperandAgainstTheDestinationsOwnBounds) {
  test::IrFixture fixture;
  fixture.source(runtimeDoubleCast("i32"));
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string module = fixture.module();
  // The bounds are `-2^31` and `2^31`, *exact* and each on the side of the range
  // it belongs to: a guard whose low bound is positive refuses every negative
  // value, which is the bug this test exists for.
  EXPECT_NE(module.find("fcmp ult double %load2, 0xC1E0000000000000"), std::string::npos) << module;
  EXPECT_NE(module.find("fcmp uge double %load2, 0x41E0000000000000"), std::string::npos) << module;
  EXPECT_NE(module.find("br i1 %f2i.bad, label %f2i.trap, label %f2i.ok"), std::string::npos);
  EXPECT_NE(module.find("call void @llvm.trap()"), std::string::npos) << module;
  EXPECT_NE(module.find("fptosi double %load2 to i32"), std::string::npos) << module;
  // **Unordered** comparisons, which is what makes NaN one of the values that
  // trap: `ult` and `uge` are both true for a NaN, and an ordered pair would call
  // it neither.
  EXPECT_EQ(module.find("fcmp olt"), std::string::npos) << module;
  EXPECT_EQ(fixture.violations(), 0u) << fixture.module();
}

TEST(IrCastTest, AnUnsignedDestinationGuardsFromZeroAndUsesTheUnsignedOpcode) {
  test::IrFixture fixture;
  fixture.source(runtimeDoubleCast("u8"));
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string module = fixture.module();
  // The low bound of an unsigned destination is **zero**, which is neither of the
  // two powers of two the signed case has, and the high bound is `2^8` and not
  // `255`: a value that is inside `[0, 256)` is one the conversion is defined for.
  EXPECT_NE(module.find("fcmp ult double %load2, 0.000000e+00"), std::string::npos) << module;
  EXPECT_NE(module.find("fcmp uge double %load2, 2.560000e+02"), std::string::npos) << module;
  EXPECT_NE(module.find("fptoui double %load2 to i8"), std::string::npos) << module;
  EXPECT_EQ(fixture.violations(), 0u) << fixture.module();
}

TEST(IrCastTest, ANarrowerSourceIsWidenedOnceAndTheTestMovesWithIt) {
  // `2^128` is past what an `f32` can name, so the operand moves to `f64` -- and
  // the *moved* value is what the test compares and what converts, which is what
  // keeps the scan's rule true by pointer and not by a second name for the value.
  test::IrFixture fixture;
  fixture.source("fn i32 main()\n"
                 "{\n"
                 "  let x: f32 = 1.5;\n"
                 "  let y: f32 = 2.5;\n"
                 "  let z: f32 = x + y;\n"
                 "  let w: u128 = z as u128;\n"
                 "  return 0;\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string module = fixture.module();
  EXPECT_NE(module.find("fpext float"), std::string::npos) << module;
  EXPECT_NE(module.find("fcmp ult double %f2i.wide"), std::string::npos) << module;
  EXPECT_NE(module.find("fcmp uge double %f2i.wide"), std::string::npos) << module;
  EXPECT_NE(module.find("fptoui double %f2i.wide to i128"), std::string::npos) << module;
  EXPECT_EQ(fixture.violations(), 0u) << fixture.module();
}

TEST(IrCastTest, AConversionThatFitsTheSourcesOwnFormatIsNotWidened) {
  // The promotion is a repair and not a policy: `2^64` is exact in `f64`, so
  // `f64 → u64` converts at its own width and pays no `fpext`.
  test::IrFixture fixture;
  fixture.source(runtimeDoubleCast("u64"));
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string module = fixture.module();
  EXPECT_EQ(module.find("f2i.wide"), std::string::npos) << module;
  EXPECT_NE(module.find("fptoui double %load2 to i64"), std::string::npos) << module;
}

TEST(IrCastTest, AConstantIsFoldedAndNoConversionIsEmitted) {
  // `casts.md`: a constant operand is decided and never guarded. The assertion is
  // the *absence* of the whole guard -- an `fcmp` of two constants folds, and a
  // conversion left standing after that fold would be the unguarded `fptosi` the
  // scan forbids.
  test::IrFixture fixture;
  fixture.source("fn i32 main() { let a = 1.5 as i32; return a; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string module = fixture.module();
  EXPECT_EQ(module.find("fptosi"), std::string::npos) << module;
  EXPECT_EQ(module.find("f2i."), std::string::npos) << module;
  EXPECT_NE(module.find("store i32 1, ptr %a"), std::string::npos) << module;
}

TEST(IrCastTest, ANegatedLiteralIsAValueAndSoIsFoldedToo) {
  // A negated literal is a **sign bit** (`typed_ast.h`), not an `fsub`: the
  // negation is folded in the lowering, so the cast sees a constant and the value
  // is `-1` and not a run-time subtraction of zero. Without that fold, `-1.5 as
  // u8` would be a run-time trap instead of the refusal below.
  test::IrFixture fixture;
  fixture.source("fn i32 main() { let a = -1.5 as i32; return a; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();

  const std::string module = fixture.module();
  EXPECT_EQ(module.find("fptosi"), std::string::npos) << module;
  EXPECT_NE(module.find("store i32 -1, ptr %a"), std::string::npos) << module;
}

TEST(IrCastTest, AConstantTheDestinationCannotHoldIsRefusedAndNoModuleIsBuilt) {
  // The two directions of the same mistake: above `i32`'s range, and below an
  // unsigned destination's floor. Both are *diagnostics* and not traps, because
  // the compiler can see the value -- which leaves the trap for the values it
  // cannot see.
  for (const std::string_view source : {"fn i32 main() { let a = 1e30 as i32; return a; }\n",
                                        "fn i32 main() { let a = (-300.0) as u8; return a; }\n",
                                        "fn i32 main() { let a = (-1.0) as u8; return a; }\n"}) {
    test::IrFixture fixture;
    fixture.source(std::string(source));
    ASSERT_TRUE(fixture.build()) << source;
    EXPECT_TRUE(fixture.hasError("ir-cast-out-of-range")) << source;
    EXPECT_FALSE(fixture.moduleBuilt()) << source;
  }
}

// --- the whole matrix, in one module ---------------------------------------------
//
// `sema/cast_test.cc` pins what each pair *is*; this is the other half -- what
// each pair *emits*. Every pair the matrix accepts is written into one program, as
// a function taking the source as a **parameter**: a parameter is a run-time value,
// so nothing folds and no pair can pass this test by having been decided as a
// constant. Then the module is read back, one function per pair.
//
// The expected instruction comes from the same shared table the `sema` suite
// checks against the matrix (`casts/universe.h`), so the two suites cannot drift
// into testing different alphabets.
TEST(IrCastTest, EveryPairTheMatrixAcceptsIsMaterialisedInTheModule) {
  sema::TypeStore types;
  std::vector<std::pair<std::string_view, std::string_view>> pairs;
  std::string source = "fn i32 main()\n{\n  return 0;\n}\n";
  for (const std::string_view from : test::casts::universe()) {
    if (!test::casts::hasValues(from)) {
      continue;
    }
    for (const std::string_view to : test::casts::universe()) {
      if (!test::casts::canBeDestination(to)) {
        continue;
      }
      const sema::TypeId fromId = test::casts::typeOf(types, from);
      const sema::TypeId toId = test::casts::typeOf(types, to);
      ASSERT_TRUE(fromId.valid()) << from;
      ASSERT_TRUE(toId.valid()) << to;
      if (!sema::castResult(types, fromId, toId).ok) {
        continue;
      }
      pairs.emplace_back(from, to);
      source += "\nfn i32 cast" + std::to_string(pairs.size()) + "(v: " + std::string(from) +
                ")\n{\n  let r: " + std::string(to) + " = v as " + std::string(to) +
                ";\n  return 0;\n}\n";
    }
  }
  ASSERT_FALSE(pairs.empty());

  test::IrFixture fixture;
  fixture.source(source);
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  // LLVM's verifier has already run inside the lowering; this is the scan of this
  // project's own rules over the same module.
  EXPECT_EQ(fixture.violations(), 0u) << fixture.module();

  const std::string module = fixture.module();
  std::size_t guardedPairs = 0;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    const std::string name = "cast" + std::to_string(i + 1);
    const std::string body = bodyOf(module, name);
    const std::string what =
        name + ": " + std::string(pairs[i].first) + " as " + std::string(pairs[i].second);
    ASSERT_FALSE(body.empty()) << what;

    const sema::TypeId fromId = test::casts::typeOf(types, pairs[i].first);
    const sema::TypeId toId = test::casts::typeOf(types, pairs[i].second);
    const std::string_view expected = test::casts::instructionFor(types, fromId, toId);

    if (expected.empty()) {
      // The identity family, and the whole point of decision 14: a cast that
      // changes nothing emits nothing -- not even a bitcast for a pointer.
      const std::string_view stray = anyCastOpcode(body);
      EXPECT_TRUE(stray.empty()) << what << ": " << stray;
      continue;
    }
    if (expected == "guarded") {
      // The one row whose shape is more than an instruction, and the reason the
      // scan forbids a bare `fptosi` anywhere: two comparisons, one branch, one
      // trap, and then the conversion -- never the other way round.
      ++guardedPairs;
      EXPECT_TRUE(hasConversionOpcode(body, "fptosi") || hasConversionOpcode(body, "fptoui"))
          << what;
      EXPECT_NE(body.find("fcmp ult"), std::string::npos) << what;
      EXPECT_NE(body.find("or i1"), std::string::npos) << what;
      EXPECT_NE(body.find("call void @llvm.trap()"), std::string::npos) << what;
      // An operand the destination cannot be compared at (`f32` to `u128`) is
      // *widened* first, so `fpext` is allowed here and nothing else is.
      for (const std::string_view opcode : kCastOpcodes) {
        if (hasConversionOpcode(body, opcode)) {
          EXPECT_TRUE(opcode == "fpext" || opcode == "fptosi" || opcode == "fptoui")
              << what << ": " << opcode;
        }
      }
      continue;
    }
    EXPECT_TRUE(hasConversionOpcode(body, expected)) << what << " wants `" << expected << "`";
    // One pair, one instruction: a second conversion in the same body would be a
    // conversion nobody wrote, which is exactly what a cast must not produce.
    if (expected == "icmp ne") {
      // The integer-to-`bool` row is `x != 0`, which is not a cast instruction at
      // all -- and the absence of any other conversion is the assertion beside it.
      EXPECT_TRUE(anyCastOpcode(body).empty()) << what << ": " << anyCastOpcode(body);
    } else {
      EXPECT_EQ(anyCastOpcode(body), expected) << what;
    }
  }
  // The trap is emitted once per guarded pair and nowhere else -- the count is how
  // a site that lost its guard is caught even though LLVM would still verify it.
  EXPECT_EQ(countOccurrences(module, "call void @llvm.trap()"), guardedPairs);
}

// --- nesting, the bottom type, and constants -------------------------------------
//
// The matrix is one conversion at a time. What a program writes is chains of them
// (`((u8)((x as i64) as u32)) as u8`), a value that never comes back (`!`, whose
// conversion is vacuous), and casts the checker folded before this stage saw them
// (a file-scope binding). One build, because all three have to hold in the same
// module at once.
TEST(IrCastTest, NestedCastsTheBottomTypeAndConstantsAreOneWellFormedModule) {
  test::IrFixture fixture;
  fixture.source("const Narrow: u8 = (u8)300;\n"
                 "const Wide: i64 = Narrow as i64;\n"
                 "\n"
                 "fn ! stop()\n"
                 "{\n"
                 "  return stop();\n"
                 "}\n"
                 "\n"
                 "fn i32 main()\n"
                 "{\n"
                 "  let x: i32 = 1000;\n"
                 "  let a: u8 = ((u8)((x as i64) as u32)) as u8;\n"
                 "  let b: f32 = (f32)((a as i32) as f64);\n"
                 "  let c: i64 = ((i64)((b as f64) * 2.0)) as i64;\n"
                 "  let d: i32 = stop();\n"
                 "  let e: i32 = stop() as i32;\n"
                 "  return (i32)((c as i64) as i32) + (i32)a + d + e + (Wide as i32);\n"
                 "}\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.moduleBuilt()) << fixture.module();
  EXPECT_EQ(fixture.violations(), 0u) << fixture.module();

  const std::string module = fixture.module();
  const std::string main = bodyOf(module, "main");
  ASSERT_FALSE(main.empty());
  // The chain, in order: `i32` → `i64` (`sext`), → `u32` (`trunc`), → `u8`
  // (`trunc`); then `u8` → `i32` (`zext`) → `f64` (`sitofp`) → `f32` (`fptrunc`).
  // Each step is the one the pair asks for, and the nesting adds no instruction of
  // its own.
  for (const std::string_view opcode : {"sext", "trunc", "zext", "sitofp", "fptrunc"}) {
    EXPECT_TRUE(hasConversionOpcode(main, opcode)) << opcode << " is missing from: " << main;
  }
  // The float → integer step of the same chain is guarded, at the width of the
  // value it converts: the `f64` sum, checked against `i64`'s bounds.
  EXPECT_TRUE(hasConversionOpcode(main, "fptosi")) << main;
  EXPECT_NE(main.find("fcmp ult double"), std::string::npos) << main;
  EXPECT_NE(main.find("call void @llvm.trap()"), std::string::npos) << main;

  // A `!` value is a **poison of the type the consumer asked for**, written with
  // no instruction: `let d: i32 = stop();` and `let e: i32 = stop() as i32;` are
  // the same answer through two spellings (the implicit conversion and the cast).
  EXPECT_EQ(countOccurrences(main, "store i32 poison"), 2U) << main;
  // And a `!` function returns nothing at all: `ret void`, never a `ret` of the
  // void-typed call the body ends in, which is the malformed module this test
  // exists for (`call void @stop()` fed to a `ret`).
  const std::string stop = bodyOf(module, "stop");
  ASSERT_FALSE(stop.empty());
  EXPECT_NE(stop.find("ret void"), std::string::npos) << stop;
  EXPECT_NE(module.find("noreturn"), std::string::npos) << stop;
}

TEST(IrCastTest, TheTwoSpellingsProduceTheSameModule) {
  // `casts.md`: the two spellings are one node, and the proof is the module. The
  // sources differ by four characters and nothing in the module may differ at all.
  test::IrFixture suffix;
  suffix.source("fn i32 main()\n"
                "{\n"
                "  let x: f64 = 1.5;\n"
                "  let y: f64 = 2.5;\n"
                "  let z: f64 = x + y;\n"
                "  let a: i32 = z as i32;\n"
                "  return a;\n"
                "}\n");
  ASSERT_TRUE(suffix.build());
  ASSERT_TRUE(suffix.moduleBuilt()) << suffix.module();

  test::IrFixture prefix;
  prefix.source("fn i32 main()\n"
                "{\n"
                "  let x: f64 = 1.5;\n"
                "  let y: f64 = 2.5;\n"
                "  let z: f64 = x + y;\n"
                "  let a: i32 = (i32)z;\n"
                "  return a;\n"
                "}\n");
  ASSERT_TRUE(prefix.build());
  ASSERT_TRUE(prefix.moduleBuilt()) << prefix.module();

  EXPECT_EQ(suffix.module(), prefix.module());
}

} // namespace
} // namespace minc::ir
