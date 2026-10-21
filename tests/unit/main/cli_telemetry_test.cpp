// SPDX-License-Identifier: GPL-3.0-or-later
//
// `loadforge stress` with its telemetry: what is printed before the run, what
// is written after it, and every way either can be refused -- over the fake
// seam, with the machine's files served by path (the real files are the
// integration tier's).

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "main/cli.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/main/cli_support.hpp"

namespace loadforge::cli {
namespace {

/// A sink that keeps what it is given, and refuses on request.
class KeepingSink final : public ResultSink {
 public:
  core::Result<core::Ok, std::string> prepare(const std::string& directory) override {
    prepared.push_back(directory);
    if (refuse_prepare) {
      return std::string{"no such directory"};
    }
    return core::Ok{};
  }
  core::Result<core::Ok, std::string> store(const std::string& path,
                                            const std::string& bytes) override {
    if (refuse_store) {
      return "the disk is full at " + path;
    }
    stored.emplace_back(path, bytes);
    return core::Ok{};
  }
  bool refuse_prepare = false;
  bool refuse_store = false;
  std::vector<std::string> prepared;
  std::vector<std::pair<std::string, std::string>> stored;
};

/// A summarizer that refuses every window.
class RefusingSummarizer final : public statistics::Summarizer {
 public:
  [[nodiscard]] core::Result<statistics::LoadSignature, statistics::StatisticsError> summarize(
      const telemetry::TimelineView& /*view*/,
      const statistics::Window& /*window*/) const override {
    return statistics::StatisticsError{statistics::StatisticsFault::kWindow, "scripted"};
  }
};

/// Two sources that report one series, so the second is a duplicate the
/// timeline refuses at the probe.
class DuplicateSource final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "twin"; }
  std::vector<telemetry::MetricStatus> probe() override {
    return {{telemetry::Metric{"twin.value", telemetry::Unit::kCount, telemetry::Kind::kGauge,
                               telemetry::Scope::kMachine, 0, std::nullopt, "test"},
             std::nullopt}};
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    out.emplace_back(telemetry::Gauge{1});
  }
};

std::vector<std::unique_ptr<telemetry::Source>> twins(platform::Syscalls& /*syscalls*/,
                                                      platform::FileSystem& /*files*/,
                                                      const MachineRoots& /*roots*/) {
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<DuplicateSource>());
  sources.push_back(std::make_unique<DuplicateSource>());
  return sources;
}

class StressTelemetry : public ::testing::Test {
 protected:
  void SetUp() override {
    syscalls.advance_clocks_per_read(10'000'000);
    syscalls.script_forks({{1001, 0}, {1002, 0}});
    syscalls.script_nonblocking_reaps(
        {{platform::Reaped{1001, 0}, 0}, {platform::Reaped{1002, 0}, 0}});
    // A machine that offers memory and CPU ticks and nothing else, as the
    // kernel's own captures show it.
    syscalls.add_tree(std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" /
                          "procfs" / "meminfo" / "present" / "proc",
                      "/proc");
    syscalls.add_tree(std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" /
                          "procfs" / "stat" / "present" / "proc",
                      "/proc");
    options.sink = &sink;
    options.host = HostFacts{};
    options.formats = {"json", "csv"};
    options.output_dir = "results";
  }

  int run() {
    return run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(), options, out,
                      err);
  }

  testing::FakeSyscalls syscalls;
  KeepingSink sink;
  StressOptions options;
  std::ostringstream out;
  std::ostringstream err;
};

TEST_F(StressTelemetry, WhatTheMachineOffersAndTheRemedyForWhatItDoesNotIsStatedBeforeTheRun) {
  run();
  const std::string text = out.str();
  const std::size_t capability = text.find("telemetry capability: ");
  const std::size_t banner = text.find("loadforge ");
  ASSERT_NE(capability, std::string::npos) << text;
  ASSERT_NE(banner, std::string::npos);
  EXPECT_LT(capability, banner) << "the capabilities come first";
  EXPECT_NE(text.find("  thermal: 0 of 1 present\n    thermal.package (package 0): not offered by "
                      "this kernel\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("      remedy: "), std::string::npos) << "every absence carries its remedy";
  EXPECT_NE(text.find("  memory: 7 of 7 present\n"), std::string::npos) << text;
}

TEST_F(StressTelemetry, TheRunIsSummarisedAfterTheControllersOwnReport) {
  EXPECT_EQ(run(), 3) << "two workers that left no record are not a clean run";
  const std::string text = out.str();
  const std::size_t ended = text.find("run ended after ");
  const std::size_t verdict = text.find("verdict: incomplete\n");
  const std::size_t telemetry = text.find("telemetry: ");
  ASSERT_NE(ended, std::string::npos) << text;
  ASSERT_NE(verdict, std::string::npos) << text;
  EXPECT_LT(ended, verdict);
  EXPECT_LT(verdict, telemetry);
  EXPECT_NE(text.find("point(s) over "), std::string::npos);
  EXPECT_NE(text.find("worker 0 reaped at "), std::string::npos) << text;
  EXPECT_NE(text.find("worker 1 reaped at "), std::string::npos);
}

TEST_F(StressTelemetry, TheResultIsWrittenInEveryFormatAskedForAndSaysWhereItWent) {
  EXPECT_EQ(run(), 3);
  ASSERT_EQ(sink.prepared, std::vector<std::string>{"results"});
  ASSERT_EQ(sink.stored.size(), 2U);
  EXPECT_EQ(sink.stored[0].first, "results/run.json");
  EXPECT_NE(sink.stored[0].second.find("\"schema_version\": 2"), std::string::npos);
  EXPECT_NE(sink.stored[0].second.find("\"memory.available\""), std::string::npos)
      << "the signature is in the result";
  EXPECT_EQ(sink.stored[1].first, "results/run.csv");
  EXPECT_NE(out.str().find("result written: results/run.json\nresult written: results/run.csv\n"),
            std::string::npos)
      << out.str();
  EXPECT_TRUE(err.str().empty()) << err.str();
}

TEST_F(StressTelemetry, WithoutAFormatNothingIsWrittenAndNothingIsPrepared) {
  options.formats.clear();
  EXPECT_EQ(run(), 3);
  EXPECT_TRUE(sink.prepared.empty());
  EXPECT_TRUE(sink.stored.empty());
  EXPECT_EQ(out.str().find("result written"), std::string::npos);
}

TEST_F(StressTelemetry, AnOutputDirectoryThatCannotBeWrittenRefusesTheRunBeforeAnyWorkerStarts) {
  sink.refuse_prepare = true;
  EXPECT_EQ(run(), 1);
  EXPECT_NE(err.str().find("loadforge: cannot write the result: no such directory\n"),
            std::string::npos)
      << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0) << "a run of hours must not find this out at its end";
}

TEST_F(StressTelemetry, AResultThatCannotBeStoredIsItsOwnExitCodeAndNamesTheFile) {
  sink.refuse_store = true;
  EXPECT_EQ(run(), 4);
  EXPECT_NE(err.str().find("loadforge: cannot write the result: the disk is full at "
                           "results/run.json\n"),
            std::string::npos)
      << err.str();
}

TEST_F(StressTelemetry, TelemetryThatCannotBeSetUpRefusesTheRunBeforeAnyWorkerStarts) {
  options.sources = twins;
  EXPECT_EQ(run(), 1);
  EXPECT_NE(err.str().find("loadforge: cannot set up telemetry: duplicate series: a series named "
                           "twin.value already exists\n"),
            std::string::npos)
      << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0);
}

TEST_F(StressTelemetry, ARunThatSamplesNothingStillTakesItsCapabilitiesFromASamplerProbe) {
  options.sources = twins;
  options.sample = false;
  EXPECT_EQ(run(), 1) << "the probe is the sampler's, so a refusal of the timeline is the same one";
  EXPECT_NE(err.str().find("loadforge: cannot set up telemetry: duplicate series"),
            std::string::npos)
      << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0);
}

TEST_F(StressTelemetry, AResultThatCannotBeComputedIsReportedAndNothingIsWritten) {
  const RefusingSummarizer refusing;
  options.summarizer = &refusing;
  EXPECT_EQ(run(), 4);
  EXPECT_NE(err.str().find("loadforge: cannot compute the result: empty or inverted window: "
                           "scripted\n"),
            std::string::npos)
      << err.str();
  EXPECT_TRUE(sink.stored.empty());
}

TEST_F(StressTelemetry, WhatTheRegistersSayReachesTheFingerprint) {
  options.host = HostFacts{fpenv::FpState{}, simd::IsaPath::kAvx2};
  EXPECT_EQ(run(), 3);
  EXPECT_NE(sink.stored[0].second.find("\"isa_path\": \"avx2\""), std::string::npos)
      << sink.stored[0].second;
  EXPECT_EQ(sink.stored[0].second.find("\"fp_environment\": null"), std::string::npos)
      << "and so do the registers";
}

TEST_F(StressTelemetry, TheFingerprintFieldsTheMachineWouldNotGiveAreNamedOnTheConsole) {
  run();
  EXPECT_NE(out.str().find("fingerprint: cpufreq_governor not recorded: "), std::string::npos)
      << out.str();
}

TEST(ExitCodeTest, AFaultIsFiveWhateverTheWorkersDidAndOtherwiseTheWorkersDecide) {
  for (const bool clean : {true, false}) {
    EXPECT_EQ(exit_code_for(results::Verdict::kFindings, clean), 5);
    EXPECT_EQ(exit_code_for(results::Verdict::kCorrectedErrors, clean), 5)
        << "corrected errors are not a clean run (F13)";
  }
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, true), 0);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, true), 0);
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, false), 3);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, false), 3);
}

TEST(HostFactsTest, TheRegistersAndTheSelectedPathOfThisMachineAreRead) {
  const HostFacts facts = read_host_facts();
  ASSERT_TRUE(facts.fp_environment.has_value());
  EXPECT_FALSE(facts.fp_environment->flush_to_zero) << "the process starts in the default state";
  ASSERT_TRUE(facts.isa_path.has_value());
  EXPECT_TRUE(simd::compiled_paths().contains(*facts.isa_path));
}

TEST(StressArgsTest, EveryOptionIsTakenOnceInAnyOrder) {
  const std::vector<std::string_view> args = {
      "--sample-interval", "250ms",    "--output-dir", "out",
      "--output",          "csv,json", "--config",     "a.toml"};
  const auto parsed = parse_stress_args(args);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().config_path, "a.toml");
  EXPECT_EQ(parsed.value().options.output_dir, "out");
  EXPECT_EQ(parsed.value().options.formats, (std::vector<std::string>{"csv", "json"}));
  EXPECT_EQ(parsed.value().options.sample_interval_ns, 250'000'000);
}

TEST(StressArgsTest, TheDefaultsAreNoFilesTheCurrentDirectoryAndASecond) {
  const std::vector<std::string_view> args = {"--config", "a.toml"};
  const auto parsed = parse_stress_args(args);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed.value().options.formats.empty());
  EXPECT_EQ(parsed.value().options.output_dir, ".");
  EXPECT_EQ(parsed.value().options.sample_interval_ns, 1'000'000'000);
  EXPECT_TRUE(parsed.value().options.sample);
}

}  // namespace
}  // namespace loadforge::cli
