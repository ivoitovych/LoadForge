// SPDX-License-Identifier: GPL-3.0-or-later
//
// `loadforge selftest` as the user meets it: the shipped binary checks itself on the machine it
// runs on. It must pass here, say what it checked line by line, and keep its exit codes:
// 0 passed, 1 a path this machine cannot run, 2 usage. The failing exit (8) needs a build or a
// machine that is wrong, which the unit tests make with doubles; nothing here asserts a time.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace loadforge::selftest_binary {
namespace {

namespace fs = std::filesystem;

struct Outcome {
  int code = -1;
  std::string console;
};

Outcome loadforge(const std::string& arguments) {
  const fs::path console =
      fs::temp_directory_path() / ("loadforge-selftest-" + std::to_string(::getpid()) + ".txt");
  const std::string command =
      std::string{"'"} + LOADFORGE_BINARY + "' " + arguments + " > '" + console.string() + "' 2>&1";
  const int status = std::system(command.c_str());
  std::ifstream in(console, std::ios::binary);
  Outcome outcome{
      WIFEXITED(status) ? WEXITSTATUS(status) : -1,
      std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}};
  std::error_code ignored;
  fs::remove(console, ignored);
  return outcome;
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

TEST(SelftestBinary, PassesOnThisMachineAndSaysWhatItChecked) {
  const Outcome ran = loadforge("selftest");
  ASSERT_EQ(ran.code, 0) << ran.console;
  EXPECT_TRUE(contains(ran.console, "PASS  golden corpus")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "PASS  golden kernels")) << ran.console;
  for (const char* workload :
       {"adaptive_mesh", "compression", "dense_linear", "fft", "graph", "monte_carlo", "particles",
        "raytrace", "sort", "sparse", "stencil"}) {
    EXPECT_TRUE(contains(ran.console, std::string{"PASS  "} + workload + " [scalar]: "))
        << workload << "\n"
        << ran.console;
  }
  EXPECT_TRUE(contains(ran.console, "selftest passed: ")) << ran.console;
  EXPECT_FALSE(contains(ran.console, "FAIL")) << ran.console;
}

TEST(SelftestBinary, VerbosePrintsThePathsAndStillPasses) {
  const Outcome ran = loadforge("selftest --verbose --seed 9 --footprint 5MiB --isa scalar");
  ASSERT_EQ(ran.code, 0) << ran.console;
  EXPECT_TRUE(contains(ran.console, "paths this build holds: ")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "seed 9")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "3 unit(s) computed")) << ran.console;
}

TEST(SelftestBinary, APathThisMachineCannotRunIsARefusal) {
  // The other architecture's vector path is never runnable here.
  for (const char* path : {"neon", "avx2"}) {
    const Outcome ran = loadforge(std::string{"selftest --isa "} + path);
    if (ran.code == 1) {
      EXPECT_TRUE(contains(ran.console, std::string{"loadforge selftest: --isa "} + path))
          << ran.console;
      EXPECT_FALSE(contains(ran.console, "PASS")) << "refused before any check\n" << ran.console;
      return;
    }
    EXPECT_EQ(ran.code, 0) << ran.console;
  }
  FAIL() << "neither neon nor avx2 was refused";
}

TEST(SelftestBinary, AUsageErrorIsTwo) {
  const Outcome ran = loadforge("selftest --frobnicate");
  EXPECT_EQ(ran.code, 2) << ran.console;
  EXPECT_TRUE(contains(ran.console, "unknown argument '--frobnicate'")) << ran.console;
}

TEST(SelftestBinary, HelpListsTheCommandAndItsExitCodes) {
  const Outcome ran = loadforge("--help");
  ASSERT_EQ(ran.code, 0);
  EXPECT_TRUE(contains(ran.console, "loadforge selftest [--isa PATH]")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "selftest: 0 every check passed; 8 a check failed"))
      << ran.console;
}

}  // namespace
}  // namespace loadforge::selftest_binary
