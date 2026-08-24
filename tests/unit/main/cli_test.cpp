// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <gtest/gtest.h>

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace loadforge::cli {
namespace {

struct Outcome {
  int code;
  std::string out;
  std::string err;
};

Outcome invoke(const std::vector<std::string_view>& args) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = run(args, out, err);
  return {code, out.str(), err.str()};
}

TEST(CollectArgs, EmptyArgvYieldsNoArguments) {
  // POSIX permits execve with an empty argv. Unguarded, subspan(1) here would
  // be undefined behaviour.
  EXPECT_TRUE(collect_args({}).empty());
}

TEST(CollectArgs, ProgramNameOnlyYieldsNoArguments) {
  std::array<char*, 1> argv{const_cast<char*>("loadforge")};
  EXPECT_TRUE(collect_args(argv).empty());
}

TEST(CollectArgs, DropsProgramNameAndKeepsTheRest) {
  std::array<char*, 3> argv{const_cast<char*>("loadforge"), const_cast<char*>("--version"),
                            const_cast<char*>("extra")};
  const auto args = collect_args(argv);
  ASSERT_EQ(args.size(), 2U);
  EXPECT_EQ(args[0], "--version");
  EXPECT_EQ(args[1], "extra");
}

TEST(Cli, NoArgumentsPrintsUsageAndSucceeds) {
  const Outcome r = invoke({});
  EXPECT_EQ(r.code, 0);
  EXPECT_NE(r.out.find("Usage:"), std::string::npos);
  EXPECT_TRUE(r.err.empty());
}

TEST(Cli, VersionFlagPrintsBareVersion) {
  for (const std::string_view flag : {"--version", "-V"}) {
    const Outcome r = invoke({flag});
    EXPECT_EQ(r.code, 0) << flag;
    EXPECT_EQ(r.out, std::string(kVersion) + "\n") << flag;
    EXPECT_TRUE(r.err.empty()) << flag;
  }
}

TEST(Cli, HelpFlagPrintsUsage) {
  for (const std::string_view flag : {"--help", "-h"}) {
    const Outcome r = invoke({flag});
    EXPECT_EQ(r.code, 0) << flag;
    EXPECT_NE(r.out.find("Usage:"), std::string::npos) << flag;
  }
}

TEST(Cli, SelftestDigestPrintsAStableHexDigest) {
  const Outcome first = invoke({"--selftest-digest"});
  EXPECT_EQ(first.code, 0);
  EXPECT_TRUE(first.err.empty());
  EXPECT_EQ(first.out.size(), 17U) << "expected 16 hex digits and a newline";

  // The whole point of this command is that the value does not move: CI
  // compares it across architectures.
  const Outcome second = invoke({"--selftest-digest"});
  EXPECT_EQ(second.out, first.out);
}

TEST(Cli, UnknownArgumentFailsAndSuggestsHelp) {
  const Outcome r = invoke({"--wat"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.err.find("unknown argument"), std::string::npos);
  EXPECT_NE(r.err.find("--help"), std::string::npos);
}

TEST(Cli, TooManyArgumentsFails) {
  const Outcome r = invoke({"--version", "--help"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.err.find("single argument"), std::string::npos);
}

TEST(Cli, DiagnosticsGoToStderrNotStdout) {
  // A caller piping --version into a script must never receive an error message
  // on the same stream.
  const Outcome r = invoke({"--nope"});
  EXPECT_TRUE(r.out.empty());
  EXPECT_FALSE(r.err.empty());
}

}  // namespace
}  // namespace loadforge::cli
