// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// Constraints: `<T: Number>` and the six other classes (`generics.md`, § 6).
//
// The two halves are both here, because they are two rules over one table and a test
// that pinned only one would let the other rot:
//
//   * the **body check**, which is what a class grants -- a binder under `+` is legal
//     under `Number` and refused under `Ordered`, and the refusal names the word to
//     write;
//   * the **satisfaction check**, which is what a class admits -- `twice::<str>(s)` is
//     refused at the instantiation, so the body (checked once, against the class)
//     never has to be re-checked.
//
// The interesting cases are the ones where the two halves *disagree in shape*:
// `Number` and `Ordered` admit the same types and grant different operations, and
// `Float` admits a subset of `Number`'s types and grants all of its operations. Those
// are the rows a derivation would have collapsed, so each has a test of its own.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "sema/sema_fixture.h"
#include "support/typenames/type_name.h"

namespace minc::sema {
namespace {

using test::SemaFixture;

// One class, its members and the types it does not admit.
//
// `members` are the spellings a test instantiates the binder at -- every type the
// class admits, and at least one of each *kind* inside it, because the predicates are
// per kind (`isArithmetic` is one question about three kinds of type). `outsiders`
// are types the class does not admit, one per kind it excludes, so the satisfaction
// check is exercised in both directions.
struct ClassCase {
  std::string_view klass;
  // The body, which must use every operation the class grants: a class that granted
  // one more than its body used would be untested in the direction that matters.
  std::string body;
  std::string result; // the declared return type of the helper
  std::vector<std::string> members;
  std::vector<std::string> outsiders;
  // A value of each member type, so the helper can be *called*: an instance exists
  // only where a call reaches it, and a binder that is never instantiated is a binder
  // whose satisfaction check never ran.
  std::vector<std::string> memberValues;
};

// The whole table, in the order the classes are declared.
const std::vector<ClassCase>& classCases() {
  static const std::vector<ClassCase> cases = {
      // --- Eq: the comparison, and nothing else ------------------------------
      {
          "Eq",
          "  return x == y || x != y;",
          "bool",
          {"i32", "u8", "i64", "f32", "f64", "bool", "char", "str", "*i32"},
          {"(i32, i32)", "[2]i32"},
          {"1", "1", "1", "1.0", "1.0", "true", "'A'", "\"a\"", "&probe"},
      },
      // --- Ordered: the four, and no arithmetic ------------------------------
      {
          "Ordered",
          "  return x < y || x <= y || x > y || x >= y;",
          "bool",
          {"i32", "u8", "i64", "f32", "f64", "char"},
          {"str", "bool", "*i32"},
          {"1", "1", "1", "1.0", "1.0", "'A'"},
      },
      // --- Number: the arithmetic, and the ordering it implies ----------------
      {
          "Number",
          "  let a: T = x + y;\n"
          "  let b: T = x - y;\n"
          "  let c: T = x * y;\n"
          "  let d: T = x / y;\n"
          "  let e: T = -x;\n"
          "  let f: T = -(-x);\n"
          "  x++;\n"
          "  x--;\n"
          "  return a + b + c + d + e + f + ++x;",
          "T",
          {"i32", "u8", "i64", "f32", "f64", "char"},
          {"bool", "str", "*i32", "(i32, i32)"},
          {"1", "1", "1", "1.0", "1.0", "'A'"},
      },
      // --- Integer: the arithmetic plus the integer-only operators ------------
      {
          "Integer",
          "  let a: T = x + y;\n"
          "  let b: T = x - y;\n"
          "  let c: T = x * y;\n"
          "  let d: T = x / y;\n"
          "  let e: T = x % y;\n"
          "  let f: T = x & y;\n"
          "  let g: T = x | y;\n"
          "  let h: T = x ^ y;\n"
          "  let i: T = ~x;\n"
          "  let j: T = x << y;\n"
          "  let k: T = x >> y;\n"
          "  let l: T = -x;\n"
          "  y++;\n"
          "  --y;\n"
          "  return a + b + c + d + e + f + g + h + i + j + k + l;",
          "T",
          {"i32", "u8", "i64", "usize", "char"},
          {"f32", "f64"},
          {"1", "1", "1", "1", "'A'"},
      },
      // --- Float: fewer types than `Number`, the same operations --------------
      // `f80` is a member and not an outsider: it is the x87 format (`typespec`), it
      // is a `Float` on every target that has it, and a target that does not refuses
      // the *type name* before a class is ever asked.
      {
          "Float",
          "  let a: T = x + y;\n"
          "  let b: T = x - y;\n"
          "  let c: T = x * y;\n"
          "  let d: T = x / y;\n"
          "  let e: T = -x;\n"
          "  x++;\n"
          "  return a + b + c + d + e + --x;",
          "T",
          {"f32", "f64", "f80"},
          {"i32", "u8", "i64"},
          {"1.0", "1.0", "1.0"},
      },
      // --- Pointer: the comparisons, and the whole of what it grants ----------
      // One member, and the reason is in the language rather than in this table: two
      // pointer types are two types, and a value of one is not a value of the other
      // (`*i32` does not convert to `*u8`), so a case with two members would be two
      // probes and two literals and not two instantiations of one call.
      {
          "Pointer",
          "  return x == y || x != y || x < y || x <= y || x > y || x >= y;",
          "bool",
          {"*i32"},
          {"i32", "str", "bool", "(i32, i32)"},
          {"&probe"},
      },
  };
  return cases;
}

// A value of a type, for the cases that have to *write* one -- a call to a generic
// takes values, and a type argument only exists where a call reaches it. The list is
// exhaustive over the types the table above names, and the default is the integer
// literal, which is what every integer type takes.
std::string valueFor(std::string_view type) {
  if (type == "str") {
    return "\"a\"";
  }
  if (type == "bool") {
    return "true";
  }
  if (type == "*i32") {
    return "&probe";
  }
  if (type == "(i32, i32)") {
    return "(1, 1)";
  }
  if (type == "[2]i32") {
    return "[2]i32{1, 1}";
  }
  if (type == "f32" || type == "f64" || type == "f80") {
    return "1.0";
  }
  return "1";
}

// The program a case needs: a helper with the binder and a body that uses it, a
// `probe` for `&probe`, and a `main` that instantiates the helper **once per member**,
// because the satisfaction check runs at the instantiation and not at the declaration.
std::string programFor(const ClassCase& one, std::span<const std::string> types) {
  std::string out = "let probe: i32 = 7;\n\n";
  out += "fn " + one.result + " use<T: " + std::string(one.klass) + ">(x: T, y: T) {\n";
  out += one.body;
  out += "\n}\n\nfn i32 main() {\n";
  for (std::size_t i = 0; i < types.size(); ++i) {
    const std::string& type = types[i];
    out += "  let a" + std::to_string(i) + ": " + type + " = " + one.memberValues[i] + ";\n";
    out += "  let r" + std::to_string(i) + " = use::<" + type + ">(a" + std::to_string(i) + ", a" +
           std::to_string(i) + ");\n";
  }
  out += "  return 0;\n}\n";
  return out;
}

// --- the body check: what a class grants ----------------------------------------

TEST(ConstraintTest, EveryClassGrantsEveryOperationItsMembersAdmit) {
  // The matrix, and the reason it is a matrix: a class is a promise about a set of
  // types, so a body using the operations the class grants must be accepted for
  // **every** type in it. A class that granted one operation too many would fail here
  // for the member that does not admit it (`Number` granting `%` fails at `f64`), and
  // one that granted too few would fail at the declaration.
  for (const ClassCase& one : classCases()) {
    SemaFixture fixture;
    fixture.source(programFor(one, one.members));
    ASSERT_TRUE(fixture.build()) << one.klass;
    EXPECT_EQ(fixture.errorCount(), 0u)
        << one.klass << ": " << (fixture.errorCount() > 0 ? fixture.firstError().message : "")
        << "\n"
        << programFor(one, one.members);
    // One instance per member, so the acceptance above is a statement about
    // instantiations and not only about the abstract body.
    EXPECT_EQ(fixture.instanceCount(), one.members.size()) << one.klass;
  }
}

TEST(ConstraintTest, AClassAlsoGrantsWhatItIncludesByNamingIt) {
  // The inclusion is **declared** and not derived (`support/constraint`'s header): a
  // class says which classes it contains, and a body under `Integer` may therefore
  // compare and divide at once. The property that makes this safe is not at run time
  // -- it is the invariant that every member of `Integer` admits both, which is the
  // matrix above.
  //
  // A body under `Number` that compares and a body under `Integer` that takes a
  // remainder *and* compares: the second is the interesting one, because it is where
  // a derivation-by-operations would have produced two sentences about one program.
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"Number", "fn bool use<T: Number>(x: T, y: T) {\n  return x < y || x == y;\n}\n"},
      {"Integer", "fn bool use<T: Integer>(x: T, y: T) {\n  return (x % y) == x && x >= y;\n}\n"},
      {"Pointer", "fn bool use<T: Pointer>(x: T, y: T) {\n  return x == y || x > y;\n}\n"},
  };
  for (const auto& [klass, helper] : cases) {
    SemaFixture fixture;
    fixture.source(helper + R"(
fn i32 main() {
  return 0;
}
)");
    ASSERT_TRUE(fixture.build()) << klass;
    // The declaration alone: what is granted is granted, and nothing needs to be
    // instantiated for the body to be legal.
    EXPECT_EQ(fixture.errorCount(), 0u)
        << klass << ": " << (fixture.errorCount() > 0 ? fixture.firstError().message : "");
  }
}

TEST(ConstraintTest, AnUnconstrainedBinderGrantsNothingButTheUniversalRules) {
  // `Any` is the class of a binder that wrote no constraint, and it is a class rather
  // than the absence of one: the universal rules -- store, copy, pass, return, address
  // -- still hold, and everything else is refused with the sentence that names the
  // class to write.
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  let copy: T = value;
  let copy2: T = identity(value);
  return copy;
}

fn i32 main() {
  return 0;
}
)");
  ASSERT_TRUE(fixture.build());
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.firstError().message;
}

TEST(ConstraintTest, TheRefusalNamesTheClassTheOperationNeeds) {
  // Each operation is granted by exactly the classes that admit it for *all* their
  // members, and the sentence names the **least powerful** one: telling a reader to
  // write `Number` for a `==` would make the declaration promise arithmetic its body
  // never uses.
  struct Case {
    std::string_view klass;
    std::string_view op;
    std::string_view needed;
  };
  const std::vector<Case> cases = {
      {"Ordered", "+", "Number"},  {"Ordered", "%", "Integer"}, {"Eq", "+", "Number"},
      {"Eq", "<", "Ordered"},      {"Number", "%", "Integer"},  {"Number", "&", "Integer"},
      {"Number", "<<", "Integer"}, {"Eq", "&", "Integer"},      {"Float", "%", "Integer"},
  };
  for (const Case& one : cases) {
    SemaFixture fixture;
    fixture.source("fn i32 f<T: " + std::string(one.klass) + ">(x: T, y: T) {\n  return x " +
                   std::string(one.op) + " y;\n}\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(fixture.build()) << one.klass << one.op;
    ASSERT_TRUE(fixture.hasError("sema-generic-operation")) << one.klass << one.op;
    const std::string& message = fixture.firstError().message;
    EXPECT_NE(message.find(std::string(one.needed)), std::string::npos)
        << one.klass << one.op << ": " << message;
  }
}

TEST(ConstraintTest, ABinderWithNoConstraintIsToldWhichClassToWrite) {
  SemaFixture fixture;
  fixture.source("fn T twice<T>(x: T) {\n  return x + x;\n}\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-generic-operation"));
  const std::string& message = fixture.firstError().message;
  // The class to write, and the *shape* of the declaration it goes in: a reader whose
  // program is one word from being legal is told the word and where it goes.
  EXPECT_NE(message.find("Number"), std::string::npos) << message;
  EXPECT_NE(message.find("<T: Number>"), std::string::npos) << message;
}

TEST(ConstraintTest, TheBoolOnlyPositionsNameTheTypeAndNotAClass) {
  // `!`, `&&`, `||` and a condition are `bool`-only, and no class grants them on
  // purpose: a class whose members are one type is not a constraint, it is the type.
  // So the sentence has to say to write `bool` -- or it would send the reader looking
  // for a class that does not exist.
  const std::vector<std::string> bodies = {
      "  if x {\n    return 1;\n  }\n  return 0;",
      "  return !x ? 1 : 0;",
  };
  for (const std::string& body : bodies) {
    SemaFixture fixture;
    fixture.source("fn i32 f<T>(x: T) {\n" + body + "\n}\nfn i32 main() { return 0; }\n");
    ASSERT_TRUE(fixture.build());
    ASSERT_TRUE(fixture.hasError("sema-generic-operation"));
    const std::string& message = fixture.firstError().message;
    EXPECT_NE(message.find("`bool`"), std::string::npos) << message;
    EXPECT_EQ(message.find("Constraint it"), std::string::npos) << message;
  }
}

TEST(ConstraintTest, APointerBinderIsToldAboutThePointeeAndNotAboutNumbers) {
  // `*p`, `p[i]` and `p + i` are the pointee's type, and an abstract pointer does not
  // name one -- so the refusal must not say "widen the class to `Number`", which is
  // advice about a different kind of value.
  SemaFixture fixture;
  fixture.source(
      "fn T step<T: Pointer>(p: T) {\n  return p + 1;\n}\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-generic-operation"));
  const std::string& message = fixture.firstError().message;
  EXPECT_NE(message.find("pointee"), std::string::npos) << message;
  EXPECT_EQ(message.find("Number"), std::string::npos) << message;
}

// --- the satisfaction check: what a class admits ---------------------------------

TEST(ConstraintTest, ATypeArgumentOutsideTheClassIsRefusedAtTheInstantiation) {
  // The other half, and the reason the body check can be once: the instance is only
  // made when the argument belongs to the class, so no stage below has to re-check
  // anything. One sentence, at the call, naming the argument and the class.
  // Every outsider, not one per class: the check is one predicate per class, and the
  // predicate is what a *kind* of type falls outside of -- so the row that matters is
  // the one for the kind the class excludes, and there is one per kind it excludes.
  for (const ClassCase& one : classCases()) {
    for (const std::string& outside : one.outsiders) {
      SemaFixture fixture;
      std::string source = "let probe: i32 = 7;\n\nfn " + one.result +
                           " use<T: " + std::string(one.klass) + ">(x: T, y: T) {\n";
      source += one.body;
      source += "\n}\n\nfn i32 main() {\n  let v: " + outside + " = " + valueFor(outside) +
                ";\n  let r = use::<" + outside + ">(v, v);\n  return 0;\n}\n";
      fixture.source(source);
      ASSERT_TRUE(fixture.build()) << one.klass << outside;
      EXPECT_TRUE(fixture.hasError("sema-constraint-unsatisfied"))
          << one.klass << " accepts " << outside << ": "
          << (fixture.errorCount() > 0 ? fixture.firstError().message : "") << "\n"
          << source;
      // The sentence names the argument and the class, because a reader who wrote the
      // argument has to be able to find it: an unnamed type argument in a message
      // about a list of them is not a message about the program.
      if (fixture.errorCount() > 0) {
        EXPECT_NE(fixture.firstError().message.find(outside), std::string::npos)
            << one.klass << " " << outside << ": " << fixture.firstError().message;
        EXPECT_NE(fixture.firstError().message.find(std::string(one.klass)), std::string::npos)
            << one.klass << " " << outside << ": " << fixture.firstError().message;
      }
      // And the instance was **not** made: a refused argument produces no function.
      EXPECT_EQ(fixture.instanceCount(), 0u) << one.klass << outside;
    }
  }
}

TEST(ConstraintTest, AnUnconstrainedBinderAdmitsEveryTypeArgument) {
  // `Any` is the default, so every program written before classes existed means what
  // it meant -- and `identity::<str>` is legal because nothing was promised about it.
  SemaFixture fixture;
  fixture.source(R"(
fn T identity<T>(value: T) {
  return value;
}

fn i32 main() {
  let s: str = "a";
  let t: (i32, i32) = (1, 2);
  let a = identity::<str>(s);
  let b = identity::<(i32, i32)>(t);
  return 0;
}
)");
  ASSERT_TRUE(fixture.build());
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.firstError().message;
  EXPECT_EQ(fixture.instanceCount(), 2u);
}

TEST(ConstraintTest, AnInadmissibleArgumentIsOneSentencePerCallSite) {
  // A call inside a generic body is expanded once per instance of the declaration
  // around it. Two expansions asking for the same inadmissible argument are **one**
  // fact about the source, so they are one sentence -- and the dedupe is keyed on the
  // node and the argument list, so a call in a second place still gets its own.
  SemaFixture fixture;
  fixture.source(R"(
fn T twice<T: Number>(x: T) {
  return x + x;
}

fn T once<T>(x: T) {
  return twice::<str>("z");
}

fn i32 main() {
  let a: i32 = 1;
  let b: f64 = 1.0;
  let p = once(a);
  let q = once(b);
  return 0;
}
)");
  ASSERT_TRUE(fixture.build());
  std::size_t count = 0;
  for (const SemaError& error : fixture.errors()) {
    if (toString(error.code) == std::string_view("sema-constraint-unsatisfied")) {
      ++count;
    }
  }
  // One call site, two instantiations of the enclosing declaration.
  EXPECT_EQ(count, 1u) << fixture.firstError().message;
}

// --- the literal rule -------------------------------------------------------------

TEST(ConstraintTest, ALiteralIsStoredInABinderOnlyInTheClasssOwnKind) {
  // The one rule in the body check that needs the class for something other than "may
  // this operator be used", and the reason it is stated in terms of the class: `1` is
  // an integer literal, so it means `1i32` for an `Integer` instantiation and nothing
  // at all for an `Integer`+`Float` one. `Float` takes `1.0` and not `1`.
  // `context` is a type the class admits, and it is here because `zero()` has no
  // argument to infer from: the call's answer is what decides `T`, and a context
  // *outside* the class would refuse the instantiation before the literal rule was
  // ever asked -- a different sentence about a different mistake.
  struct Case {
    std::string_view klass;
    std::string_view literal;
    std::string_view context;
    bool accepted;
    std::string_view mustMention;
  };
  const std::vector<Case> cases = {
      {"Integer", "1", "i32", true, {}},
      {"Float", "1.0", "f64", true, {}},
      {"Integer", "1.0", "i32", false, "float literal"},
      {"Float", "1", "f64", false, "integer literal"},
      {"Number", "1", "i32", false, "Integer"},
      {"Number", "1.0", "f64", false, "Integer"},
      {"Ordered", "1", "i32", false, "Integer"},
      {"Any", "1", "i32", false, "no constraint"},
  };
  for (const Case& one : cases) {
    SemaFixture fixture;
    fixture.source("fn T zero<T: " + std::string(one.klass) + ">() {\n  return " +
                   std::string(one.literal) + ";\n}\nfn i32 main() {\n  let z: " +
                   std::string(one.context) + " = zero();\n  return 0;\n}\n");
    ASSERT_TRUE(fixture.build()) << one.klass << one.literal;
    if (one.accepted) {
      EXPECT_EQ(fixture.errorCount(), 0u)
          << one.klass << " " << one.literal << ": " << fixture.firstError().message;
      continue;
    }
    ASSERT_TRUE(fixture.hasError("sema-generic-operation"))
        << one.klass << " " << one.literal << ": "
        << (fixture.errorCount() > 0 ? fixture.firstError().message : "no error at all");
    EXPECT_NE(fixture.firstError().message.find(std::string(one.mustMention)), std::string::npos)
        << one.klass << " " << one.literal << ": " << fixture.firstError().message;
  }
}

TEST(ConstraintTest, ALiteralDecidedByAClassIsTheInstancesValue) {
  // `fn T zero<T: Integer>() { return 1; }` is one body and one text, and the *value*
  // is right for each instance because the literal's type is the binder: the text is
  // substituted at the boundary like every other type of the node.
  SemaFixture fixture;
  fixture.source(R"(
fn T one<T: Integer>() {
  return 1;
}

fn i32 main() {
  let a: i32 = one();
  let b: u8 = one();
  let c: i64 = one();
  return a;
}
)");
  ASSERT_TRUE(fixture.build());
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.firstError().message;
  EXPECT_EQ(fixture.instanceCount(), 3u);
}

TEST(ConstraintTest, ALiteralBesideABinderIsDecidedByTheClassNotByTheBinder) {
  // `x + 1` with `T: Integer` is legal and the result is `T`; with `T: Number` it is
  // refused, because for one instantiation the body would have to mean `1.0`.
  SemaFixture fixture;
  fixture.source("fn T add<T: Integer>(x: T) {\n  return x + 1;\n}\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  EXPECT_EQ(fixture.errorCount(), 0u) << fixture.firstError().message;

  SemaFixture refused;
  refused.source("fn T add<T: Number>(x: T) {\n  return x + 1;\n}\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(refused.build());
  EXPECT_TRUE(refused.hasError("sema-generic-operation")) << "no error at all";
}

TEST(ConstraintTest, TwoDifferentBindersAreTwoTypesAndNotAConstrainedOperation) {
  // `T + K` is not a class question: there is no conversion between two type
  // parameters, so the sentence is about the operands and not about a class.
  SemaFixture fixture;
  fixture.source("fn T add<T: Number, K: Number>(x: T, y: K) {\n  return x + y;\n}\nfn i32 main() "
                 "{ return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-invalid-operands"));
  EXPECT_NE(fixture.firstError().message.find("two different type parameters"), std::string::npos)
      << fixture.firstError().message;
}

// --- the vocabulary ---------------------------------------------------------------

TEST(ConstraintTest, EveryClassIsAWordTheLanguageHasAndNoTypeIsAWordOfAClass) {
  // A class and a type can never be the same spelling, or `T: i32` would be a class
  // and `let Number = 5;` would be shadowing a word of the language.
  for (const std::string_view name : support::constraintClassNames()) {
    EXPECT_FALSE(support::isTypeNameWord(name)) << name;
    EXPECT_EQ(support::constraintClassFromName(name).has_value(), true) << name;
  }
  // Case-sensitive, like every other word: `number` is not `Number`, and the sentence
  // for it lists the classes.
  SemaFixture fixture;
  fixture.source("type P<T: number> = (T, T);\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-constraint-not-a-class"));
  const std::string& message = fixture.firstError().message;
  for (const std::string_view name : support::constraintClassNames()) {
    EXPECT_NE(message.find(name), std::string::npos) << name << ": " << message;
  }
}

TEST(ConstraintTest, ALowercaseWordThatIsNotAClassIsRefusedOnceAndNotSilentlyIgnored) {
  // A constraint that was read and then ignored would be a declaration promising a
  // guarantee nobody checks -- the one outcome worse than an error. So it is refused,
  // and the refusal names the classes.
  SemaFixture fixture;
  fixture.source("fn T f<T: Ord>(x: T) {\n  return x;\n}\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  ASSERT_TRUE(fixture.hasError("sema-constraint-not-a-class"));
  EXPECT_NE(fixture.firstError().message.find("Ordered"), std::string::npos)
      << fixture.firstError().message;
}

TEST(ConstraintTest, AConstraintOnAnAliasIsCheckedLikeOneOnAFunction) {
  // The binder list is one shape in two declarations, so a class on an alias is the
  // same question -- and a word that is not a class is refused there too.
  SemaFixture ok;
  ok.source("type Pair<T: Number, K> = (T, K);\n");
  ASSERT_TRUE(ok.build());
  EXPECT_EQ(ok.errorCount(), 0u) << ok.firstError().message;

  SemaFixture bad;
  bad.source("type Pair<T: Numeric, K> = (T, K);\n");
  ASSERT_TRUE(bad.build());
  EXPECT_TRUE(bad.hasError("sema-constraint-not-a-class"));
}

TEST(ConstraintTest, AnAliasUseIsCheckedAgainstTheClassLikeACallIs) {
  // The half of a class that a generic **type** can have, and the reason it is a
  // check and not decoration: a `type` body never performs an operation, so the
  // body check has nothing to say about it -- what the declaration *does* promise is
  // which types may fill the hole, and the use is where a hole gets filled.
  SemaFixture ok;
  ok.source(R"(
fn i32 sum(v: Vec<i32>) {
  return v[0];
}

type Vec<T: Number> = [4]T;

fn i32 main() {
  let v: Vec<i32> = [4]i32{1, 2, 3, 4};
  return sum(v);
}
)");
  ASSERT_TRUE(ok.build());
  // A use above the declaration, which file scope allows -- the check is the
  // *use's* and it does not depend on the order the two were written in.
  EXPECT_EQ(ok.errorCount(), 0u) << ok.firstError().message;

  SemaFixture bad;
  bad.source(R"(
fn i32 sum(v: Vec<i32>) {
  return v[0];
}

type Vec<T: Number> = [4]T;

fn i32 main() {
  let v: Vec<bool> = [4]bool{true, false, true, false};
  return sum(v);
}
)");
  ASSERT_TRUE(bad.build());
  ASSERT_TRUE(bad.hasError("sema-constraint-unsatisfied")) << bad.firstError().message;
  const std::string& message = bad.firstError().message;
  // The argument, the binder and the class, because a reader who wrote a type
  // argument has to be able to find it in the sentence.
  EXPECT_NE(message.find("`bool`"), std::string::npos) << message;
  EXPECT_NE(message.find("`T`"), std::string::npos) << message;
  EXPECT_NE(message.find("`Number`"), std::string::npos) << message;
  EXPECT_NE(message.find("`Vec`"), std::string::npos) << message;
}

TEST(ConstraintTest, TheClassIsCheckedWhereverAUseIsWritten) {
  // Three positions, one check: the target of another alias, a parameter, and a
  // `let`'s annotation. A use that escapes one of them would be a hole in the rule
  // rather than a smaller feature.
  const std::vector<std::string> programs = {
      "type Vec<T: Number> = [4]T;\ntype Bad = Vec<bool>;\n",
      "type Vec<T: Number> = [4]T;\nfn bool f(v: Vec<str>) { return true; }\n",
      "type Vec<T: Number> = [4]T;\nfn i32 main() {\n  let v: Vec<(i32, i32)>;\n  "
      "return 0;\n}\n",
  };
  for (const std::string& program : programs) {
    SemaFixture fixture;
    fixture.source(program);
    ASSERT_TRUE(fixture.build()) << program;
    EXPECT_TRUE(fixture.hasError("sema-constraint-unsatisfied"))
        << program << ": "
        << (fixture.errorCount() > 0 ? fixture.firstError().message : "no error at all");
  }
}

TEST(ConstraintTest, AConstraintIsOneSentenceEvenThoughTheBindersAreReadTwice) {
  // The signature pass and the body pass both enter the binder list, so the read is
  // done once and the table answers the second entry. Without that, a fault in a
  // constraint would be two identical sentences at the same span.
  SemaFixture fixture;
  fixture.source("fn T f<T: Numeric>(x: T) {\n  return x;\n}\nfn i32 main() { return 0; }\n");
  ASSERT_TRUE(fixture.build());
  std::size_t count = 0;
  for (const SemaError& error : fixture.errors()) {
    if (toString(error.code) == std::string_view("sema-constraint-not-a-class")) {
      ++count;
    }
  }
  EXPECT_EQ(count, 1u) << fixture.firstError().message;
}

} // namespace
} // namespace minc::sema
