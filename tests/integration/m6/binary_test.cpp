// SPDX-License-Identifier: GPL-3.0-or-later
//
// M6 as the user meets it: the shipped binary searches for the worst case with
// real worker processes. An objective on a metric this machine cannot read is
// refused before anything runs, and a search that ends reports the worst case
// with its replicates, its spread and its interval, and names it again when it
// is repeated.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace loadforge::m6 {
namespace {

namespace fs = std::filesystem;

struct Outcome {
  int code = -1;
  std::string console;
  double seconds = 0.0;
};

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class Scratch {
 public:
  Scratch() : path_(fs::temp_directory_path() / ("loadforge-m6-" + std::to_string(::getpid()))) {
    fs::create_directories(path_);
  }
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  Scratch(const Scratch&) = delete;
  Scratch(Scratch&&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  Scratch& operator=(Scratch&&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

Outcome loadforge(const Scratch& scratch, const std::string& arguments) {
  const fs::path console = scratch.path() / "console.txt";
  const std::string command =
      std::string{"'"} + LOADFORGE_BINARY + "' " + arguments + " > '" + console.string() + "' 2>&1";
  const auto begun = std::chrono::steady_clock::now();
  const int status = std::system(command.c_str());
  const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - begun;
  return Outcome{WIFEXITED(status) ? WEXITSTATUS(status) : -1, slurp(console), taken.count()};
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

/// The line of `text` that starts with `prefix`, without its newline.
std::string line_of(const std::string& text, const std::string& prefix) {
  const std::size_t at = text.find("\n" + prefix);
  if (at == std::string::npos) {
    return {};
  }
  const std::size_t from = at + 1;
  return text.substr(from, text.find('\n', from) - from);
}

/// A search small enough to run in under a minute: one worker against three, over
/// the objective `objective`, with real workers verifying every unit they compute.
std::string search_over(const std::string& objective) {
  return "explore --objective " + objective +
         " --candidate one:sort:1 --candidate three:sort:3 --duration 3s --footprint 64MiB "
         "--sample-interval 250ms --replicates 1 --final-replicates 3 --reference-replicates 2 "
         "--interleave 2";
}

TEST(M6Binary, AnObjectiveOnAMetricThisMachineCannotReadIsRefusedBeforeAnythingRuns) {
  const Scratch scratch;
  const Outcome ran = loadforge(
      scratch, "explore --objective thermal.package:max:higher --candidate a:sort:1 --verbose");
  if (contains(ran.console, "thermal: 1 of 1 present")) {
    GTEST_SKIP() << "this machine has a temperature sensor";
  }
  EXPECT_EQ(ran.code, 1) << ran.console;
  EXPECT_TRUE(contains(ran.console, "objective metric unavailable")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "hwmon"))
      << "the source's own reason and remedy: " << ran.console;
  EXPECT_FALSE(contains(ran.console, "trial ")) << "no trial ran";
  EXPECT_FALSE(contains(ran.console, "worker(s)")) << "no worker started: " << ran.console;
  EXPECT_LT(ran.seconds, 5.0);
}

TEST(M6Binary, ASettlePhaseOnAMetricThisMachineCannotReadIsRefusedTheSameWay) {
  const Scratch scratch;
  const Outcome ran =
      loadforge(scratch,
                "explore --objective memory.available:min:lower --candidate a:sort:1 "
                "--settle thermal.package --verbose");
  if (contains(ran.console, "thermal: 1 of 1 present")) {
    GTEST_SKIP() << "this machine has a temperature sensor";
  }
  EXPECT_EQ(ran.code, 1) << ran.console;
  EXPECT_TRUE(contains(ran.console, "settle metric thermal.package")) << ran.console;
  EXPECT_FALSE(contains(ran.console, "worker(s)")) << ran.console;
}

TEST(M6Binary, AnObjectiveOnAMetricNoSourceKnowsIsRefusedToo) {
  const Scratch scratch;
  const Outcome ran =
      loadforge(scratch, "explore --objective vibration.chassis:max:higher --candidate a:sort:1");
  EXPECT_EQ(ran.code, 1) << ran.console;
  EXPECT_TRUE(contains(ran.console, "objective metric unknown")) << ran.console;
}

TEST(M6Binary, AMisusedOptionIsAUsageErrorThatNamesIt) {
  const Scratch scratch;
  const Outcome ran = loadforge(scratch, "explore --candidate a:sort:1 --duration soon");
  EXPECT_EQ(ran.code, 2) << ran.console;
  EXPECT_TRUE(contains(ran.console, "--duration:")) << ran.console;
  const Outcome help = loadforge(scratch, "--help");
  EXPECT_TRUE(contains(help.console, "loadforge explore --objective")) << help.console;
}

TEST(M6Binary, ACandidateTheMachineCannotRunIsRefusedAsSuchByTheSearch) {
  const Scratch scratch;
  const Outcome ran = loadforge(
      scratch, "explore --objective memory.available:min:lower --candidate a:no_such_workload:1");
  EXPECT_EQ(ran.code, 3) << ran.console;
  EXPECT_TRUE(contains(ran.console, "no workload named 'no_such_workload'")) << ran.console;
}

/// What every search that ends says, whatever it found.
void expect_a_complete_report(const Outcome& ran) {
  ASSERT_EQ(ran.code, 0) << ran.console;
  const std::string worst = line_of(ran.console, "worst case: ");
  EXPECT_TRUE(contains(worst, "worst case: one: ") || contains(worst, "worst case: three: "))
      << "a candidate of the set: " << worst;
  EXPECT_TRUE(contains(worst, "+/-")) << "an interval: " << worst;
  EXPECT_TRUE(contains(worst, "95% CI, n=3")) << "the replicates behind it: " << worst;
  EXPECT_TRUE(contains(ran.console, "ranking: ")) << "whether it can be told from the rival";
  EXPECT_TRUE(contains(ran.console, "candidate one: ")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "candidate three: ")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "  replicates: ")) << "every replicate is recorded";
  EXPECT_TRUE(contains(ran.console, "reference: anchor ")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "readings (midpoint of the load window in ns, value):"));
  EXPECT_TRUE(contains(ran.console, "explore: 8 trial(s) in 1 round(s)")) << ran.console;
}

TEST(M6Binary, ASearchOverRealWorkersReportsItsWorstCaseWithReplicatesSpreadAndInterval) {
  const Scratch scratch;
  expect_a_complete_report(loadforge(scratch, search_over("memory.available:min:lower")));
}

TEST(M6Binary, ASearchOnAQuantityNoCandidateChangesRanksNothingAndSaysSo) {
  // The machine's total memory is the same in every window of every trial: the one objective
  // whose answer is known without measuring anything, so it holds on any machine and under
  // any load. Nothing may be called the worst, and the report says it cannot tell.
  const Scratch scratch;
  const Outcome ran = loadforge(scratch, search_over("memory.total:max:higher"));
  expect_a_complete_report(ran);
  EXPECT_TRUE(contains(ran.console, "ranking: not distinguishable from the best rival"))
      << ran.console;
  EXPECT_TRUE(contains(ran.console, "sd 0")) << "no spread at all: " << ran.console;
}

}  // namespace
}  // namespace loadforge::m6
