// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 4, as the user meets it: the shipped binary, run twice on this
// machine with `--output json,csv`. The result files exist and say what a result
// says (M3 exit criteria 1 and 5), and the two runs' fingerprints agree in
// everything that describes the machine and not the moment.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "controller/results/fingerprint.hpp"

namespace loadforge::m3 {
namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Outcome {
  int code = -1;
  std::string console;
};

Outcome run_binary(const fs::path& directory, const fs::path& console_directory,
                   const std::string& more) {
  const fs::path console = console_directory / "console.txt";
  const std::string command = std::string{"'"} + LOADFORGE_BINARY + "' stress --config '" +
                              LOADFORGE_SOURCE_DIR +
                              "/tests/fixtures/config/short-stress.toml' --output json,csv "
                              "--output-dir '" +
                              directory.string() + "' --sample-interval 100ms " + more + " > '" +
                              console.string() + "' 2>&1";
  const int status = std::system(command.c_str());
  return Outcome{WIFEXITED(status) ? WEXITSTATUS(status) : -1, slurp(console)};
}

/// The members of the JSON "fingerprint" object, one per line, without those
/// that describe the moment. The writer indents one member to a line.
std::set<std::string> fingerprint_lines(const std::string& json) {
  const std::size_t begin = json.find("\"fingerprint\": {");
  const std::size_t end = json.find("\n  },", begin);
  std::set<std::string> lines;
  std::istringstream in(json.substr(begin, end - begin));
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t quote = line.find('"');
    const std::size_t close = line.find('"', quote + 1);
    if (quote == std::string::npos || close == std::string::npos) {
      continue;
    }
    if (!results::is_moment_field(line.substr(quote + 1, close - quote - 1))) {
      lines.insert(line);
    }
  }
  return lines;
}

class ScratchDirectory {
 public:
  ScratchDirectory()
      : path_(fs::temp_directory_path() / ("loadforge-m3-" + std::to_string(::getpid()))) {
    fs::create_directories(path_);
  }
  ~ScratchDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory(ScratchDirectory&&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(ScratchDirectory&&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

TEST(RealBinaryTest, ARunWritesItsResultInBothFormatsAndTwoRunsShareTheirFingerprint) {
  const ScratchDirectory directory;
  const fs::path first = directory.path() / "first";
  const fs::path second = directory.path() / "second";
  fs::create_directories(first);
  fs::create_directories(second);

  const Outcome one = run_binary(first, first, "");
  ASSERT_EQ(one.code, 0) << one.console;
  const Outcome two = run_binary(second, second, "");
  ASSERT_EQ(two.code, 0) << two.console;

  for (const Outcome* outcome : {&one, &two}) {
    EXPECT_NE(outcome->console.find("telemetry capability: "), std::string::npos);
    EXPECT_NE(outcome->console.find("2 of 2 worker(s) completed"), std::string::npos);
    EXPECT_NE(outcome->console.find("verdict: incomplete"), std::string::npos)
        << "the null workload verifies nothing, and a run that verified nothing is not clean";
    EXPECT_NE(outcome->console.find("result written: "), std::string::npos);
  }

  const std::string json = slurp(first / "run.json");
  ASSERT_FALSE(json.empty());
  EXPECT_NE(json.find("\"schema_version\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"sample_interval_ns\": 100000000"), std::string::npos);
  EXPECT_NE(json.find("\"memory.available\""), std::string::npos);
  EXPECT_NE(json.find("\"fate\""), std::string::npos);
  const std::string csv = slurp(first / "run.csv");
  EXPECT_NE(csv.find("schema_version"), std::string::npos);
  EXPECT_NE(csv.find("memory.available"), std::string::npos);

  const std::set<std::string> a = fingerprint_lines(json);
  const std::set<std::string> b = fingerprint_lines(slurp(second / "run.json"));
  ASSERT_GT(a.size(), 5U) << "the fingerprint was found";
  EXPECT_EQ(a, b) << "two runs on one machine are comparable (M3 exit criterion 5)";
  EXPECT_TRUE(fs::exists(first / "run.csv"));
  EXPECT_FALSE(fs::exists(first / "run.json.partial"));
}

TEST(RealBinaryTest, AnOutputDirectoryThatDoesNotExistRefusesTheRunBeforeItStarts) {
  const ScratchDirectory directory;
  const fs::path missing = directory.path() / "missing";
  const Outcome outcome = run_binary(missing, directory.path(), "");
  EXPECT_EQ(outcome.code, 1) << outcome.console;
  EXPECT_NE(outcome.console.find("cannot write the result: \"" + missing.string() +
                                 "\" is not a directory"),
            std::string::npos)
      << outcome.console;
  EXPECT_EQ(outcome.console.find("spawned"), std::string::npos) << "no worker was started";
}

}  // namespace
}  // namespace loadforge::m3
