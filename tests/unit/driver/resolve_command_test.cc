// Copyright (c) 2026 minc+ contributors.
// SPDX-License-Identifier: MIT
// The `resolve` subcommand's contract: which stream carries what, in what order,
// and which exit code -- tested directly instead of by spawning a process.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "driver/exit_code.h"
#include "driver/resolve_command.h"
#include "tests/examples_dir.h"

namespace minc::driver {
namespace {

struct ResolveRun {
  int code = 0;
  std::string out;
  std::string err;
};

[[nodiscard]] ResolveRun run(const std::vector<std::string>& inputs, const std::string& at = {}) {
  ResolveRequest request;
  request.inputs = inputs;
  request.at = at;
  // With injected streams there is no terminal to ask, and asking the real one
  // would be the wrong question.
  request.diagnosticColor = support::ColorMode::Plain;

  std::ostringstream out;
  std::ostringstream err;
  const int code = resolveInputs(request, out, err);
  return ResolveRun{code, out.str(), err.str()};
}

// Deletes the file on scope exit, so a failing assertion cannot leave litter
// behind for the next run to trip over.
class TempFile {
public:
  TempFile(const std::string& name, std::string_view bytes) {
    path_ = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream out(path_, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  ~TempFile() {
    static_cast<void>(std::remove(path_.c_str()));
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const {
    return path_;
  }

private:
  std::string path_;
};

[[nodiscard]] std::string exampleFile(const std::string& name) {
  return (std::filesystem::path(test::kExamplesDir) / name).string();
}

TEST(ResolveCommandTest, ExamplePrintsScopesAndDefsOnStdout) {
  const ResolveRun r = run({exampleFile("002_variables.mx")});

  EXPECT_EQ(r.code, exitCode(ExitCode::Ok)) << r.err;
  EXPECT_TRUE(r.err.empty()) << r.err;
  EXPECT_NE(r.out.find("scopes"), std::string::npos);
  EXPECT_NE(r.out.find("defs"), std::string::npos);
  EXPECT_NE(r.out.find("function"), std::string::npos);
  EXPECT_NE(r.out.find("main"), std::string::npos);
}

TEST(ResolveCommandTest, RefsShowEveryUseAndItsTarget) {
  const TempFile file("minc_resolve_refs.mx",
                      "fn i32 main()\n{\n  let total = 1;\n  return total;\n}\n");
  ResolveRequest request;
  request.inputs = {file.path()};
  request.showRefs = true;

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(resolveInputs(request, out, err), exitCode(ExitCode::Ok)) << err.str();
  EXPECT_NE(out.str().find("refs"), std::string::npos);
  EXPECT_NE(out.str().find("-> defs#"), std::string::npos);
}

TEST(ResolveCommandTest, UnresolvedGoesToStderrAndFailsTheRun) {
  const TempFile file("minc_resolve_bad.mx", "fn i32 main() { return mystery; }\n");
  const ResolveRun r = run({file.path()});

  EXPECT_EQ(r.code, exitCode(ExitCode::Failure));
  EXPECT_NE(r.err.find("error[resolve-unknown-name]"), std::string::npos) << r.err;
  EXPECT_NE(r.err.find("mystery"), std::string::npos) << r.err;
  // The tables are still printed: a file with an unresolved name still has every
  // definition and every other use to inspect.
  EXPECT_NE(r.out.find("defs"), std::string::npos);
}

TEST(ResolveCommandTest, AstModePrintsTheLoweredTreeAndTheItemTree) {
  const TempFile file("minc_resolve_ast.mx", "fn i32 main() { return 0; }\n");
  ResolveRequest request;
  request.inputs = {file.path()};
  request.showAst = true;

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(resolveInputs(request, out, err), exitCode(ExitCode::Ok)) << err.str();
  EXPECT_NE(out.str().find("item tree"), std::string::npos);
  EXPECT_NE(out.str().find("FnDecl"), std::string::npos);
  EXPECT_NE(out.str().find("Declaration"), out.str().find("FnDecl"));
  EXPECT_NE(out.str().find("lowered AST"), std::string::npos);
}

TEST(ResolveCommandTest, AtResolvesAPositionToADefinition) {
  const TempFile file("minc_resolve_at.mx", "fn i32 main()\n{\n  let total = 1;\n  return 0;\n}\n");
  // Line 3, column 7: the second byte of `total` (`t` is byte 7 with 0-based
  // columns, as the line table reports them).
  const ResolveRun r = run({file.path()}, "3:7");

  EXPECT_EQ(r.code, exitCode(ExitCode::Ok)) << r.err;
  EXPECT_NE(r.out.find("-> defs#"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("total"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("declared at"), std::string::npos) << r.out;
}

TEST(ResolveCommandTest, AtOutsideADeclarationSaysSo) {
  const TempFile file("minc_resolve_at2.mx", "fn i32 main()\n{\n  return 0;\n}\n");
  const ResolveRun r = run({file.path()}, "3:7");

  EXPECT_EQ(r.code, exitCode(ExitCode::Ok)) << r.err;
  EXPECT_NE(r.out.find("no declaration starts here"), std::string::npos) << r.out;
}

TEST(ResolveCommandTest, MissingFileIsAnError) {
  const ResolveRun r = run({"definitely-not-here.mx"});

  EXPECT_EQ(r.code, exitCode(ExitCode::Failure));
  EXPECT_NE(r.err.find("mincc: error:"), std::string::npos);
  EXPECT_TRUE(r.out.empty());
}

TEST(ResolveCommandTest, EveryExampleSucceeds) {
  const std::vector<std::string> files{
      exampleFile("001_main_func.mx"), exampleFile("002_variables.mx"), exampleFile("003_types.mx"),
      exampleFile("004_operators.mx"), exampleFile("005_literals.mx")};
  const ResolveRun r = run(files);

  EXPECT_EQ(r.code, exitCode(ExitCode::Ok)) << r.err;
  EXPECT_TRUE(r.err.empty()) << r.err;
  for (const std::string& file : files) {
    EXPECT_NE(r.out.find(file), std::string::npos) << file;
  }
}

TEST(ResolveCommandTest, NoInputsIsAUsageError) {
  const CliOptions options;
  EXPECT_EQ(runResolve(options), exitCode(ExitCode::Usage));
}

} // namespace
} // namespace minc::driver
