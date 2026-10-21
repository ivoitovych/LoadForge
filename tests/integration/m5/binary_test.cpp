// SPDX-License-Identifier: GPL-3.0-or-later
//
// M5 as the user meets it: the shipped binary runs the flagship mixed workload
// and the cycling workloads with a crash journal, and the journal says what
// the run did. Verification continues across every rotation (no wrong result,
// no check not performed), and a journal an earlier run left behind abnormally
// is kept and reported, never overwritten.

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace loadforge::m5 {
namespace {

namespace fs = std::filesystem;

struct Outcome {
  int code = -1;
  std::string console;
};

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class Scratch {
 public:
  Scratch() : path_(fs::temp_directory_path() / ("loadforge-m5-" + std::to_string(::getpid()))) {
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
  const int status = std::system(command.c_str());
  return Outcome{WIFEXITED(status) ? WEXITSTATUS(status) : -1, slurp(console)};
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

TEST(M5Binary, AMixedRunRotatesWhileEveryUnitIsVerifiedAndTheJournalSaysSo) {
  const Scratch scratch;
  const std::string journal = (scratch.path() / "run.lfj").string();
  const Outcome ran = loadforge(scratch,
                                "stress --test mixed --duration 7s --workers 2 "
                                "--footprint 4MiB --rotation 1s --journal '" +
                                    journal + "'");
  ASSERT_EQ(ran.code, 0) << ran.console;
  EXPECT_TRUE(contains(ran.console, "verdict: clean")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "0 wrong result(s), 0 check(s) not performed"));
  EXPECT_TRUE(contains(ran.console, " 0 refused,")) << "no command of the schedule was refused";
  EXPECT_FALSE(contains(ran.console, "refused switch"));
  EXPECT_FALSE(contains(ran.console, "refused rebind"));

  const Outcome replayed = loadforge(scratch, "journal '" + journal + "'");
  ASSERT_EQ(replayed.code, 0) << replayed.console;
  EXPECT_TRUE(contains(replayed.console, "ended cleanly")) << replayed.console;
  EXPECT_TRUE(contains(replayed.console, "phase \"r1 ")) << "a rotation after the first";
  EXPECT_TRUE(contains(replayed.console, "phase \"stop\""));
  EXPECT_TRUE(contains(replayed.console, "phase \"done\""));
}

TEST(M5Binary, ACyclingRunHoldsTheLoadAtEachLevelForItsDwell) {
  const Scratch scratch;
  const std::string journal = (scratch.path() / "cycle.lfj").string();
  const Outcome ran = loadforge(scratch,
                                "stress --test cycling --cycle load --duration 7s "
                                "--workers 2 --footprint 4MiB --dwell 1s --journal '" +
                                    journal + "'");
  ASSERT_EQ(ran.code, 0) << ran.console;
  EXPECT_TRUE(contains(ran.console, "verdict: clean")) << ran.console;
  const Outcome replayed = loadforge(scratch, "journal '" + journal + "'");
  EXPECT_TRUE(contains(replayed.console, "ended cleanly")) << replayed.console;
  EXPECT_TRUE(contains(replayed.console, "idle"))
      << "the load went down to nothing: " << replayed.console;
}

TEST(M5Binary, ACyclingRunOfCoresVariesHowManyWorkersWork) {
  const Scratch scratch;
  const Outcome ran = loadforge(scratch,
                                "stress --test cycling --cycle cores --duration 6s "
                                "--workers 4 --footprint 4MiB --dwell 1s");
  ASSERT_EQ(ran.code, 0) << ran.console;
  EXPECT_TRUE(contains(ran.console, "verdict: clean")) << ran.console;
  EXPECT_TRUE(contains(ran.console, "control: ")) << ran.console;
}

TEST(M5Binary, AJournalAnEarlierRunLeftAbnormallyIsKeptAndReportedNotOverwritten) {
  const Scratch scratch;
  const std::string journal = (scratch.path() / "run.lfj").string();
  // A journal cut short, as a hard kill leaves it: the first run's file, with
  // its tail gone.
  ASSERT_EQ(loadforge(scratch,
                      "stress --test mixed --duration 3s --workers 2 --footprint 4MiB "
                      "--rotation 1s --journal '" +
                          journal + "'")
                .code,
            0);
  const std::string whole = slurp(journal);
  ASSERT_GT(whole.size(), 40U);
  {
    std::ofstream out(journal, std::ios::binary | std::ios::trunc);
    out.write(whole.data(), static_cast<std::streamsize>(whole.size() - 7));
  }
  const Outcome again = loadforge(scratch,
                                  "stress --test mixed --duration 3s --workers 2 "
                                  "--footprint 4MiB --rotation 1s --journal '" +
                                      journal + "'");
  ASSERT_EQ(again.code, 0) << again.console;
  EXPECT_TRUE(contains(again.console, "abnormally")) << again.console;
  EXPECT_TRUE(fs::exists(journal + ".1")) << "the evidence is kept";
  EXPECT_EQ(slurp(journal + ".1").size(), whole.size() - 7);
  const Outcome now = loadforge(scratch, "journal '" + journal + "'");
  EXPECT_TRUE(contains(now.console, "ended cleanly")) << "and the new run has its own";
}

TEST(M5Binary, AnImpossibleScheduleIsRefusedBeforeAnyWorkerIsStarted) {
  const Scratch scratch;
  const Outcome ran = loadforge(scratch,
                                "stress --test mixed --duration 30h --workers 2 "
                                "--footprint 4MiB --rotation 1ms");
  EXPECT_NE(ran.code, 0) << ran.console;
  EXPECT_TRUE(contains(ran.console, "more than 100000 steps")) << ran.console;
  EXPECT_FALSE(contains(ran.console, "spawned")) << ran.console;
}

}  // namespace
}  // namespace loadforge::m5
