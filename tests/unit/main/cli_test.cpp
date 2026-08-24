// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <gtest/gtest.h>

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
