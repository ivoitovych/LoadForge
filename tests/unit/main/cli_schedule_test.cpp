// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command line's two runs made of a schedule (stress --test mixed and
// cycling), the crash journal's options, and `loadforge journal`.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "controller/journal/file_ops.hpp"
#include "controller/journal/file_storage.hpp"
#include "main/cli.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/main/cli_support.hpp"
#include "unit/main/memory_files.hpp"
#include "workloads/mixed/schedule.hpp"

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

/// A link to the shipped binary, which is what a real run's workers must be.
class LinkToTheBinary {
 public:
  LinkToTheBinary()
      : path_(std::filesystem::temp_directory_path() /
              ("loadforge-schedule-link-" + std::to_string(::getpid()))) {
    std::filesystem::remove(path_);
    std::filesystem::create_symlink(LOADFORGE_BINARY, path_);
  }
  ~LinkToTheBinary() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  LinkToTheBinary(const LinkToTheBinary&) = delete;
  LinkToTheBinary& operator=(const LinkToTheBinary&) = delete;
  LinkToTheBinary(LinkToTheBinary&&) = delete;
  LinkToTheBinary& operator=(LinkToTheBinary&&) = delete;
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

/// A directory of the test's own.
class Directory {
 public:
  Directory()
      : path_(std::filesystem::temp_directory_path() /
              ("loadforge-schedule-dir-" + std::to_string(::getpid()))) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_);
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  Directory(Directory&&) = delete;
  Directory& operator=(Directory&&) = delete;
  [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

Outcome run_test(const LinkToTheBinary& link, const std::vector<std::string_view>& args) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = stress_test(args, link.path(), "/sys", StressOptions{}, out, err);
  return {code, out.str(), err.str()};
}

// --- what the options accept ---------------------------------------------------------------

TEST(CliSchedule, EveryOptionOfTheSchedulesIsRefusedByNameWhereItDoesNotBelong) {
  struct Case {
    std::vector<std::string_view> args;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{"compression", "--rotation", "1s"},
       "loadforge: stress --test compression: --rotation is not an option of this run\n"},
      {{"compression", "--dwell", "1s"},
       "loadforge: stress --test compression: --dwell is not an option of this run\n"},
      {{"compression", "--cycle", "load"},
       "loadforge: stress --test compression: --cycle is not an option of this run\n"},
      {{"compression", "--workload", "fft"},
       "loadforge: stress --test compression: --workload is not an option of this run\n"},
      {{"mixed", "--dwell", "1s"},
       "loadforge: stress --test mixed: --dwell is not an option of this run\n"},
      {{"mixed", "--workload", "fft"},
       "loadforge: stress --test mixed: --workload is not an option of this run\n"},
      {{"cycling", "--rotation", "1s"},
       "loadforge: stress --test cycling: --rotation is not an option of this run\n"},
  };
  for (const Case& each : cases) {
    std::vector<std::string_view> args{"stress", "--test"};
    args.insert(args.end(), each.args.begin(), each.args.end());
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2) << each.expected;
    EXPECT_EQ(r.err, each.expected);
    EXPECT_TRUE(r.out.empty());
  }
}

TEST(CliSchedule, EachValueIsRefusedByNameWhenItIsMisused) {
  struct Case {
    std::vector<std::string_view> options;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{"--rotation", "soon"}, "--rotation: "},
      {{"--rotation", "0s"}, "--rotation: must be above zero and no longer than a run can be"},
      {{"--rotation", "9223372036854775807ms"},
       "--rotation: must be above zero and no longer than a run can be"},
      {{"--rotation", "1s", "--rotation", "2s"},
       "--rotation: not an option of stress --test, or given twice"},
      {{"--dwell", "soon"}, "--dwell: "},
      {{"--dwell", "0s"}, "--dwell: must be above zero and no longer than a run can be"},
      {{"--cycle", "sideways"}, "--cycle: 'sideways' is none of load, cores"},
      {{"--journal"}, "--journal: missing its value"},
      {{"--journal", "a", "--journal", "b"},
       "--journal: not an option of stress --test, or given twice"},
  };
  for (const Case& each : cases) {
    std::vector<std::string_view> args{"stress", "--test", "cycling"};
    if (each.options[0] == "--rotation") {
      args[2] = "mixed";
    }
    args.insert(args.end(), each.options.begin(), each.options.end());
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2) << each.expected;
    EXPECT_EQ(r.err.substr(0, 26 + each.expected.size()), "loadforge: stress --test: " + each.expected)
        << r.err;
  }
}

TEST(CliSchedule, TheWorkloadOfACyclingRunIsOneThisBuildHas) {
  const Outcome r = invoke({"stress", "--test", "cycling", "--workload", "nope"});
  EXPECT_EQ(r.code, 2);
  EXPECT_EQ(r.err,
            "loadforge: stress --test: no workload named 'nope'; this build has: adaptive_mesh "
            "compression dense_linear fft graph monte_carlo particles raytrace sort sparse "
            "stencil\n");
}

TEST(CliSchedule, ARunThatCannotBeScheduledIsRefusedBeforeAnyWorker) {
  {
    const Outcome r = invoke({"stress", "--test", "mixed", "--footprint", "50%"});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err,
              "loadforge: stress --test mixed: --footprint must be a size, not a share of memory\n");
  }
  {
    const Outcome r = invoke({"stress", "--test", "mixed", "--footprint", "1KiB"});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err,
              "loadforge: stress --test mixed: no workload of the roster accepts a footprint of "
              "1024 bytes\n");
  }
  {
    const Outcome r = invoke({"stress", "--test", "cycling", "--duration", "0s"});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err,
              "loadforge: stress --test cycling: the duration is not one a schedule can be made "
              "for\n");
  }
}

// --- the runs themselves, with real workers ---------------------------------------------------

TEST(CliSchedule, AMixedRunRotatesTheWorkersAndKeepsVerifyingAndSaysSo) {
  const LinkToTheBinary link;
  const Outcome r = run_test(link, {"mixed", "--duration", "5s", "--rotation", "1s", "--workers",
                                    "2", "--footprint", "1MiB", "--seed", "11"});
  EXPECT_EQ(r.code, 0) << r.out << r.err;
  EXPECT_NE(r.out.find("[1."), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("phase r1 compute-heavy: "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("phase r4 "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("2 of 2 worker(s) completed"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("found 0 wrong"), std::string::npos) << "verification went on: " << r.out;
  EXPECT_NE(r.out.find("control: "), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("refused switch"), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("refused rebind"), std::string::npos) << r.out;
}

TEST(CliSchedule, ACyclingRunRaisesAndLowersTheLoadOfOneWorkload) {
  const LinkToTheBinary link;
  const Outcome r = run_test(link, {"cycling", "--workload", "compression", "--cycle", "load",
                                    "--duration", "5s", "--dwell", "1s", "--workers", "2",
                                    "--footprint", "1MiB"});
  EXPECT_EQ(r.code, 0) << r.out << r.err;
  EXPECT_NE(r.out.find("phase c0 idle: cycling load"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("phase c1 heavy: "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("phase c2 moderate: "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("2 of 2 worker(s) completed"), std::string::npos) << r.out;
}

TEST(CliSchedule, TheCyclingOfCoresPausesSomeWorkersAndNotOthers) {
  const LinkToTheBinary link;
  const Outcome r = run_test(link, {"cycling", "--workload", "compression", "--cycle", "cores",
                                    "--duration", "3s", "--dwell", "1s", "--workers", "2",
                                    "--footprint", "1MiB"});
  EXPECT_EQ(r.code, 0) << r.out << r.err;
  EXPECT_NE(r.out.find("phase c0 1 of 2: cycling cores"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("phase c1 1 of 2: "), std::string::npos) << r.out;
}

// --- the journal ---------------------------------------------------------------------------------

TEST(CliJournal, ARunWritesItAndTheNextOneSaysHowItEnded) {
  const LinkToTheBinary link;
  const Directory directory;
  const std::string path = directory.file("run.lfj");
  const Outcome first = run_test(link, {"compression", "--duration", "1s", "--workers", "1",
                                        "--footprint", "1MiB", "--journal", path});
  EXPECT_EQ(first.code, 0) << first.out << first.err;
  EXPECT_EQ(first.out.find("previous journal"), std::string::npos) << "there was none";
  const Outcome shown = invoke({"journal", path});
  EXPECT_EQ(shown.code, 0) << shown.err;
  EXPECT_NE(shown.out.find("ended cleanly; "), std::string::npos) << shown.out;
  EXPECT_NE(shown.out.find("phase \"run\" workload compression"), std::string::npos) << shown.out;
  EXPECT_NE(shown.out.find("phase \"stop\""), std::string::npos) << shown.out;
  EXPECT_NE(shown.out.find("phase \"done\""), std::string::npos) << shown.out;

  const Outcome second = run_test(link, {"compression", "--duration", "1s", "--workers", "1",
                                         "--footprint", "1MiB", "--journal", path});
  EXPECT_EQ(second.code, 0) << second.out << second.err;
  EXPECT_NE(second.out.find("previous journal " + path + ": ended cleanly; "), std::string::npos)
      << second.out;
  EXPECT_FALSE(std::filesystem::exists(path + ".1")) << "a clean one is not kept";
}

TEST(CliJournal, ARunThatWasKilledLeavesAJournalTheNextRunKeepsAndNames) {
  const LinkToTheBinary link;
  const Directory directory;
  const std::string path = directory.file("run.lfj");
  {
    journal::RealFileOps ops;
    auto storage = journal::FileStorage::open(ops, path);
    ASSERT_TRUE(storage.has_value());
    auto writer = journal::open_writer(*storage.value());
    ASSERT_TRUE(writer.has_value());
    journal::Entry entry;
    entry.elapsed_ns = 3'000'000'000;
    entry.phase = "r2 memory-heavy";
    entry.workload = "sort+graph";
    entry.workers_alive = 4;
    ASSERT_TRUE(writer.value()->append(entry).has_value());
    ASSERT_TRUE(writer.value()->sync().has_value());
  }
  const Outcome r = run_test(link, {"compression", "--duration", "1s", "--workers", "1",
                                    "--footprint", "1MiB", "--journal", path});
  EXPECT_EQ(r.code, 0) << r.out << r.err;
  EXPECT_NE(r.out.find("previous journal " + path +
                       ": ended abnormally between records; 1 entry; the last: t=3.0s phase "
                       "\"r2 memory-heavy\" workload sort+graph workers 4 alive 0 stale; kept as " +
                       path + ".1"),
            std::string::npos)
      << r.out;
  EXPECT_TRUE(std::filesystem::exists(path + ".1"));
}

TEST(CliJournal, TheCommandTakesOneFileAndSaysWhenThereIsNone) {
  EXPECT_EQ(invoke({"journal"}).code, 2);
  EXPECT_EQ(invoke({"journal"}).err, "loadforge: journal takes exactly one FILE\n");
  EXPECT_EQ(invoke({"journal", "a", "b"}).code, 2);
  const Outcome missing = invoke({"journal", "/no/such/journal"});
  EXPECT_EQ(missing.code, 1);
  EXPECT_EQ(missing.err, "loadforge: there is no journal at /no/such/journal\n");
  EXPECT_TRUE(missing.out.empty());
}

TEST(CliJournal, AnOptionOfTheConfigCommandToo) {
  const Outcome r = invoke({"stress", "--config", "a.toml", "--journal"});
  EXPECT_EQ(r.code, 2);
  EXPECT_EQ(r.err, "loadforge: --journal takes a value\n");
  const auto parsed = parse_stress_args(
      std::vector<std::string_view>{"--config", "a.toml", "--journal", "run.lfj"});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().options.journal_path, "run.lfj");
  EXPECT_TRUE(parse_stress_args(std::vector<std::string_view>{"--config", "a.toml"})
                  .value()
                  .options.journal_path.empty());
}

TEST(CliJournal, ShowJournalSaysWhatWentWrongOnStderr) {
  cli_testing::MemoryFiles files;
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(show_journal("run.lfj", files, files, out, err), 1);
  EXPECT_EQ(err.str(), "loadforge: there is no journal at run.lfj\n");
  EXPECT_TRUE(out.str().empty());
}

// --- the run over the fake ----------------------------------------------------------------------

const StressOptions& supervisor_with_a_journal(cli_testing::MemoryFiles& files) {
  static StressOptions options;
  options.sample = false;
  options.capabilities = false;
  options.journal_path = "run.lfj";
  options.journal_ops = &files;
  options.journal_files = &files;
  return options;
}

TEST(CliJournal, AJournalThatCannotBeOpenedRefusesTheRunBeforeAFork) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  cli_testing::MemoryFiles files;
  files.fail_open_from(1, EACCES);
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_with_a_journal(files), out, err),
            1);
  EXPECT_NE(err.str().find("loadforge: cannot open the journal run.lfj: "), std::string::npos)
      << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0);
}

TEST(CliJournal, ARunThatIsReportedClosesItsJournalEvenWhenItCouldNotStartTheWorkers) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.fail_fork(EAGAIN);
  cli_testing::MemoryFiles files;
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_with_a_journal(files), out, err),
            1);
  const auto shown = read_journal(files, files, "run.lfj");
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_EQ(shown.value().ending, journal::Ending::kTerminated)
      << "the controller came back and said it failed; that is an end";
}

TEST(CliJournal, TheChildOfAFailedExecDoesNotCloseTheJournalItSharesWithItsParent) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{0, 0}});  // This process is the worker; its exec will fail.
  syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  cli_testing::MemoryFiles files;
  std::ostringstream out;
  std::ostringstream err;
  (void)run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                   supervisor_with_a_journal(files), out, err);
  const auto shown = read_journal(files, files, "run.lfj");
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_NE(shown.value().ending, journal::Ending::kTerminated)
      << "a terminator written by the child would tell the parent's reader the run ended";
}

TEST(CliJournal, AJournalThatCannotBeWrittenIsSaidAndTheRunGoesOn) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{1001, 0}, {1002, 0}});
  syscalls.script_nonblocking_reaps({testing::FakeSyscalls::WaitStep{platform::Reaped{1001, 0}, 0},
                                     testing::FakeSyscalls::WaitStep{platform::Reaped{1002, 0}, 0}});
  cli_testing::MemoryFiles files;
  files.fail_write_from(1, ENOSPC);
  std::ostringstream out;
  std::ostringstream err;
  // The workers exit at once, with nothing to say; that is the run's business here.
  (void)run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                   supervisor_with_a_journal(files), out, err);
  EXPECT_NE(out.str().find("journal: append: storage failed"), std::string::npos) << out.str();
  EXPECT_NE(err.str().find("the journal could not be closed, and will read as ended abnormally: "),
            std::string::npos)
      << err.str();
}

}  // namespace
}  // namespace loadforge::cli
