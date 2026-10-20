// SPDX-License-Identifier: GPL-3.0-or-later
//
// The frequency source: cpufreq's current frequency per CPU, and the CPU
// utilisation counters of /proc/stat.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The six states, through the substituted platform seam (thermal/memfs.hpp).
// The captured tree (tests/fixtures/sysfs/frequency/absent: the online list
// and /proc/stat of this machine, which has no cpufreq directory) gives the
// absent state and the real /proc/stat; the present, denied, malformed and
// vanished cpufreq states are built in the kernel's layout, and the wrapped
// state is derived from the captured /proc/stat by editing its first line, as
// the test names (M3 exit criterion 4).

#include "telemetry/sources/frequency/frequency.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "contracts/telemetry/source_contract.hpp"
#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"
#include "unit/telemetry/sources/thermal/memfs.hpp"
#include "unit/telemetry/sources/thermal/owned.hpp"

namespace loadforge::telemetry {
namespace {

using fixtures::MemFs;
using fixtures::OwnedSource;
using topology::Availability;

constexpr std::uint64_t kTop = UINT64_MAX;  // 2^64 - 1

const std::string kOnline = "/sys/devices/system/cpu/online";
const std::string kStat = "/proc/stat";

std::string freq_path(int cpu) {
  return "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
}

/// /proc/stat as the kernel writes it: the aggregate line, then per-CPU lines.
std::string stat_text(const std::string& aggregate) {
  return aggregate + "\ncpu0 1208 0 395 47890 153 0 103 64 0 0\nintr 12345 0 1\nctxt 99\n";
}

std::string ticks_line(std::uint64_t user, std::uint64_t system, std::uint64_t idle) {
  return "cpu  " + std::to_string(user) + " 0 " + std::to_string(system) + " " +
         std::to_string(idle) + " 462 0 224 244 0 0";
}

std::vector<Reading> sample(Source& source) {
  std::vector<Reading> readings;
  source.sample(readings);
  return readings;
}

std::uint64_t counter_of(const Reading& reading) {
  const auto* counter = std::get_if<Counter>(&reading);
  EXPECT_NE(counter, nullptr);
  return counter == nullptr ? 0 : counter->raw;
}

double gauge_of(const Reading& reading) {
  const auto* gauge = std::get_if<Gauge>(&reading);
  EXPECT_NE(gauge, nullptr);
  return gauge == nullptr ? -1.0 : gauge->value;
}

Availability kind_of(const Reading& reading) {
  const auto* unavailable = std::get_if<Unavailable>(&reading);
  EXPECT_NE(unavailable, nullptr);
  return unavailable == nullptr ? Availability::kPresent : unavailable->kind;
}

class FrequencyTest : public ::testing::Test {
 protected:
  void SetUp() override { fs.load(fixtures::fixture("sysfs/frequency/absent")); }

  void give_cpufreq(const std::string& kilohertz = "3000000") {
    for (int cpu = 0; cpu < 4; ++cpu) {
      fs.set(freq_path(cpu), kilohertz + "\n");
    }
  }

  MemFs fs;
  platform::FileSystem files{fs};
  FrequencySource source{files};
};

// --- the captured machine: four CPUs, no cpufreq, a real /proc/stat

TEST_F(FrequencyTest, TheCapturedMachineHasNoCpufreqButHasTicks) {
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 6U) << "four CPUs, then the two counters";
  for (std::uint32_t cpu = 0; cpu < 4; ++cpu) {
    EXPECT_EQ(statuses[cpu].metric.name, "frequency.cpu");
    EXPECT_EQ(statuses[cpu].metric.scope, Scope::kCpu);
    EXPECT_EQ(statuses[cpu].metric.scope_index, cpu);
    EXPECT_EQ(statuses[cpu].metric.unit, Unit::kHertz);
    ASSERT_TRUE(statuses[cpu].unavailable.has_value());
    EXPECT_EQ(statuses[cpu].unavailable->kind, Availability::kAbsent);
    EXPECT_EQ(statuses[cpu].unavailable->path, freq_path(static_cast<int>(cpu)));
    EXPECT_NE(statuses[cpu].unavailable->remediation.find("virtual machine"), std::string::npos);
  }
  EXPECT_EQ(statuses[4].metric.name, "frequency.busy_ticks");
  EXPECT_EQ(statuses[5].metric.name, "frequency.total_ticks");
  for (std::size_t at = 4; at < 6; ++at) {
    EXPECT_FALSE(statuses[at].unavailable.has_value());
    EXPECT_EQ(statuses[at].metric.kind, Kind::kCounter);
    EXPECT_EQ(statuses[at].metric.unit, Unit::kCount);
    EXPECT_EQ(statuses[at].metric.scope, Scope::kMachine);
    EXPECT_FALSE(statuses[at].metric.modulus.has_value()) << "an unsigned long wraps at 2^64";
    EXPECT_EQ(statuses[at].metric.provider, "procfs:stat");
  }

  // The captured first line is "cpu  4677 0 1418 192173 462 0 224 244 0 0":
  // user 4677 nice 0 system 1418 idle 192173 iowait 462 irq 0 softirq 224 steal 244.
  // busy = user + nice + system + irq + softirq + steal, total = busy + idle + iowait.
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 2U) << "an absent CPU is not sampled";
  EXPECT_EQ(counter_of(readings[0]), 4677U + 1418U + 224U + 244U);
  EXPECT_EQ(counter_of(readings[1]), 4677U + 1418U + 224U + 244U + 192173U + 462U);
  EXPECT_EQ(source.name(), "frequency");
}

TEST_F(FrequencyTest, TheCapturedProcStatAgreesWithTheOneInProcfsFixtures) {
  MemFs other;
  other.load(fixtures::fixture("procfs/stat/present"));
  platform::FileSystem other_files(other);
  FrequencySource over_procfs(other_files);
  const std::vector<MetricStatus> statuses = over_procfs.probe();
  ASSERT_EQ(statuses.size(), 3U)
      << "no online list in that tree: one absent CPU entry, two counters";
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  EXPECT_FALSE(statuses[1].unavailable.has_value());
  EXPECT_EQ(counter_of(sample(over_procfs)[0]), 4677U + 1418U + 224U + 244U);
}

TEST_F(FrequencyTest, ARootPrefixesEveryPath) {
  MemFs other;
  other.set("/mnt/m/sys/devices/system/cpu/online", "0\n");
  other.set("/mnt/m/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1200000\n");
  other.set("/mnt/m/proc/stat", stat_text(ticks_line(1, 2, 3)));
  platform::FileSystem other_files(other);
  FrequencySource rooted(other_files, "/mnt/m");
  const std::vector<MetricStatus> statuses = rooted.probe();
  ASSERT_EQ(statuses.size(), 3U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_DOUBLE_EQ(gauge_of(sample(rooted)[0]), 1.2e9);
}

// --- present

TEST_F(FrequencyTest, EachOnlineCpuWithCpufreqIsAGaugeInHertz) {
  fs.set(kOnline, "0,2-3\n");
  fs.set(freq_path(0), "3000000\n");
  fs.set(freq_path(2), "800000\n");
  fs.set(freq_path(3), "4600123\n");
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 5U);
  EXPECT_EQ(statuses[0].metric.scope_index, 0U);
  EXPECT_EQ(statuses[1].metric.scope_index, 2U);
  EXPECT_EQ(statuses[2].metric.scope_index, 3U);
  EXPECT_EQ(statuses[0].metric.provider, "cpufreq");
  EXPECT_EQ(statuses[0].metric.kind, Kind::kGauge);
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 5U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[0]), 3.0e9);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 8.0e8);
  EXPECT_DOUBLE_EQ(gauge_of(readings[2]), 4600123000.0);
  EXPECT_GT(counter_of(readings[4]), counter_of(readings[3]));
}

TEST_F(FrequencyTest, TheBoundsOfAPlausibleFrequency) {
  fs.set(freq_path(0), "0\n");
  fs.set(freq_path(1), "100000000\n");
  fs.set(freq_path(2), "100000001\n");
  fs.set(freq_path(3), "-1\n");
  const std::vector<MetricStatus> statuses = source.probe();
  EXPECT_FALSE(statuses[0].unavailable.has_value()) << "a CPU parked at zero is a reading";
  EXPECT_FALSE(statuses[1].unavailable.has_value());
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
  EXPECT_EQ(statuses[3].unavailable->kind, Availability::kMalformed);
}

// --- the CPU list

TEST_F(FrequencyTest, AnOnlineListThatCannotBeUsedIsOneStatusAndTheTicksAreStillRead) {
  struct Case {
    std::string name;
    Availability kind;
    void (*apply)(MemFs&);
  };
  const Case cases[] = {
      {"absent", Availability::kAbsent, [](MemFs& f) { f.remove(kOnline); }},
      {"denied", Availability::kDenied, [](MemFs& f) { f.fail_open(kOnline, EACCES); }},
      {"denied by policy", Availability::kDenied, [](MemFs& f) { f.fail_open(kOnline, EPERM); }},
      {"malformed", Availability::kMalformed, [](MemFs& f) { f.set(kOnline, "0-x\n"); }},
      {"empty", Availability::kMalformed, [](MemFs& f) { f.set(kOnline, "\n"); }},
      {"unreadable", Availability::kUnreadable, [](MemFs& f) { f.fail_read(kOnline, EIO); }},
      {"a directory", Availability::kUnreadable, [](MemFs& f) { f.make_directory(kOnline); }},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    MemFs one;
    one.load(fixtures::fixture("sysfs/frequency/absent"));
    c.apply(one);
    platform::FileSystem one_files(one);
    FrequencySource probing(one_files);
    const std::vector<MetricStatus> statuses = probing.probe();
    ASSERT_EQ(statuses.size(), 3U);
    EXPECT_EQ(statuses[0].metric.name, "frequency.cpu");
    ASSERT_TRUE(statuses[0].unavailable.has_value());
    EXPECT_EQ(statuses[0].unavailable->kind, c.kind);
    EXPECT_EQ(statuses[0].unavailable->path, kOnline);
    EXPECT_FALSE(statuses[0].unavailable->remediation.empty());
    EXPECT_NE(statuses[0].unavailable->remediation.find("online CPU list"), std::string::npos)
        << "the remedy is for the list, which is what failed";
    EXPECT_EQ(sample(probing).size(), 2U);
  }
}

// --- cpufreq: the states of one CPU

TEST_F(FrequencyTest, ACpuThatCannotBeReadIsReportedWithItsStateAndARemedyForThatState) {
  fs.set(freq_path(0), "3000000\n");
  fs.fail_open(freq_path(1), EACCES);
  fs.set(freq_path(2), "fast\n");
  fs.fail_read(freq_path(3), EIO);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 6U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kDenied);
  EXPECT_NE(statuses[1].unavailable->remediation.find("scaling_cur_freq"), std::string::npos);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kMalformed);
  EXPECT_NE(statuses[2].unavailable->remediation.find("not a frequency"), std::string::npos);
  EXPECT_EQ(statuses[3].unavailable->kind, Availability::kUnreadable);
  EXPECT_NE(statuses[3].unavailable->remediation.find("dmesg"), std::string::npos);
  EXPECT_EQ(sample(source).size(), 3U) << "the one CPU read, and the two counters";
}

TEST_F(FrequencyTest, ACpuThatGoesMidRunIsVanishedAndTheRestOfTheRoundIsUnaffected) {
  give_cpufreq();
  static_cast<void>(source.probe());
  fs.remove(freq_path(2));
  std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 6U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), 3.0e9);
  EXPECT_EQ(kind_of(readings[2]), Availability::kVanished);
  EXPECT_NE(std::get<Unavailable>(readings[2]).remediation.find("disappeared during the run"),
            std::string::npos);
  EXPECT_DOUBLE_EQ(gauge_of(readings[3]), 3.0e9);
  fs.set(freq_path(2), "2000000\n");
  EXPECT_DOUBLE_EQ(gauge_of(sample(source)[2]), 2.0e9);
}

TEST_F(FrequencyTest, ACpuThatTurnsBadMidRunIsReportedAsWhatItNowIsNeverAbsent) {
  give_cpufreq();
  static_cast<void>(source.probe());
  fs.fail_open(freq_path(0), EACCES);
  fs.set(freq_path(1), "x\n");
  fs.fail_read(freq_path(2), EIO);
  fs.fail_read(freq_path(3), ENODEV);
  const std::vector<Reading> readings = sample(source);
  EXPECT_EQ(kind_of(readings[0]), Availability::kDenied);
  EXPECT_EQ(kind_of(readings[1]), Availability::kMalformed);
  EXPECT_EQ(kind_of(readings[2]), Availability::kUnreadable);
  EXPECT_EQ(kind_of(readings[3]), Availability::kVanished) << "the driver says the device is gone";
}

// --- /proc/stat: the states of the counters

TEST_F(FrequencyTest, AStatThatCannotBeUsedMakesBothCountersUnavailableForThatReason) {
  struct Case {
    std::string name;
    Availability kind;
    void (*apply)(MemFs&);
    const char* remedy_word;
  };
  const Case cases[] = {
      {"absent", Availability::kAbsent, [](MemFs& f) { f.remove(kStat); }, "mount procfs"},
      {"denied", Availability::kDenied, [](MemFs& f) { f.fail_open(kStat, EACCES); }, "hidepid"},
      {"unreadable", Availability::kUnreadable, [](MemFs& f) { f.fail_read(kStat, EIO); }, "dmesg"},
      {"a directory", Availability::kUnreadable, [](MemFs& f) { f.make_directory(kStat); },
       "dmesg"},
      {"empty", Availability::kMalformed, [](MemFs& f) { f.set(kStat, ""); }, "does not guess"},
      {"not the aggregate line", Availability::kMalformed,
       [](MemFs& f) { f.set(kStat, "cpu0 1 2 3 4\n"); }, "does not guess"},
      {"no space after cpu", Availability::kMalformed, [](MemFs& f) { f.set(kStat, "cpu\n"); },
       "does not guess"},
      {"three fields", Availability::kMalformed, [](MemFs& f) { f.set(kStat, "cpu  1 2 3\n"); },
       "does not guess"},
      {"no fields", Availability::kMalformed, [](MemFs& f) { f.set(kStat, "cpu   \n"); },
       "does not guess"},
      {"a word", Availability::kMalformed, [](MemFs& f) { f.set(kStat, "cpu  1 2 x 4\n"); },
       "does not guess"},
      {"a negative", Availability::kMalformed, [](MemFs& f) { f.set(kStat, "cpu  1 2 -3 4\n"); },
       "does not guess"},
      {"a late word", Availability::kMalformed,
       [](MemFs& f) { f.set(kStat, "cpu  1 2 3 4 5 6 7 8 9 10 abc\n"); }, "does not guess"},
      {"past 2^64", Availability::kMalformed,
       [](MemFs& f) { f.set(kStat, "cpu  18446744073709551616 2 3 4\n"); }, "does not guess"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    MemFs one;
    one.load(fixtures::fixture("sysfs/frequency/absent"));
    c.apply(one);
    platform::FileSystem one_files(one);
    FrequencySource probing(one_files);
    const std::vector<MetricStatus> statuses = probing.probe();
    ASSERT_EQ(statuses.size(), 6U);
    for (std::size_t at = 4; at < 6; ++at) {
      ASSERT_TRUE(statuses[at].unavailable.has_value());
      EXPECT_EQ(statuses[at].unavailable->kind, c.kind);
      EXPECT_EQ(statuses[at].unavailable->path, kStat);
      EXPECT_FALSE(statuses[at].unavailable->detail.empty());
      EXPECT_NE(statuses[at].unavailable->remediation.find(c.remedy_word), std::string::npos)
          << statuses[at].unavailable->remediation;
    }
    EXPECT_TRUE(sample(probing).empty()) << "unavailable counters are not sampled";
  }
}

TEST_F(FrequencyTest, AStatWithFewerOrMoreFieldsThanThisKernelWritesIsStillRead) {
  fs.set(kStat, "cpu  10 0 20 30\n");  // the four every kernel writes
  static_cast<void>(source.probe());
  std::vector<Reading> readings = sample(source);
  EXPECT_EQ(counter_of(readings[0]), 30U);
  EXPECT_EQ(counter_of(readings[1]), 60U);

  fs.set(kStat,
         "cpu  10 1 20 30 40 2 3 4 500 600\n");  // guest and guest_nice are inside user, nice
  readings = sample(source);
  EXPECT_EQ(counter_of(readings[0]), 10U + 1U + 20U + 2U + 3U + 4U);
  EXPECT_EQ(counter_of(readings[1]), 10U + 1U + 20U + 2U + 3U + 4U + 30U + 40U);

  fs.set(kStat, "cpu  10 1 20 30 40 2 3 4 500 600 7 8 9\n");  // a later kernel's extra fields
  readings = sample(source);
  EXPECT_EQ(counter_of(readings[0]), 40U);
}

TEST_F(FrequencyTest, AStatThatDisappearsMidRunIsVanishedForBothCountersAndReadAgain) {
  static_cast<void>(source.probe());
  fs.remove(kStat);
  std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 2U);
  EXPECT_EQ(kind_of(readings[0]), Availability::kVanished);
  EXPECT_EQ(kind_of(readings[1]), Availability::kVanished);
  EXPECT_NE(std::get<Unavailable>(readings[0]).remediation.find("unmounted"), std::string::npos);
  fs.set(kStat, stat_text(ticks_line(5, 6, 7)));
  readings = sample(source);
  EXPECT_EQ(counter_of(readings[0]), 5U + 6U + 224U + 244U);
}

TEST_F(FrequencyTest, AStatThatTurnsBadMidRunIsReportedAsWhatItNowIsNeverAbsent) {
  static_cast<void>(source.probe());
  fs.set(kStat, "garbage\n");
  std::vector<Reading> readings = sample(source);
  EXPECT_EQ(kind_of(readings[0]), Availability::kMalformed);
  EXPECT_EQ(kind_of(readings[1]), Availability::kMalformed);
  fs.heal(kStat);
  fs.fail_open(kStat, EACCES);
  readings = sample(source);
  EXPECT_EQ(kind_of(readings[0]), Availability::kDenied);
  fs.heal(kStat);
  fs.set(kStat, stat_text(ticks_line(1, 1, 1)));
  EXPECT_EQ(counter_of(sample(source)[0]), 1U + 1U + 224U + 244U);
}

TEST_F(FrequencyTest, AStatThatWasNeverReadableIsNotSampledAfterwards) {
  fs.remove(kStat);
  static_cast<void>(source.probe());
  fs.set(kStat, stat_text(ticks_line(1, 1, 1)));
  EXPECT_TRUE(sample(source).empty()) << "the counters were not part of the set the probe fixed";
}

TEST_F(FrequencyTest, ProbingAgainStartsFromTheMachineAsItIsNow) {
  give_cpufreq();
  EXPECT_EQ(sample(source).size(), 0U) << "nothing before the probe";
  static_cast<void>(source.probe());
  EXPECT_EQ(sample(source).size(), 6U);
  fs.remove(freq_path(0));
  fs.remove(kStat);
  const std::vector<MetricStatus> again = source.probe();
  EXPECT_EQ(again[0].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(again[4].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(sample(source).size(), 3U);
}

// --- the counter wraps

TEST_F(FrequencyTest, ACounterThatWrapsMidSampleAdvancesByWhatItRanNotByAHugeStep) {
  // Lines of the captured shape with every field but user and idle zero, so that
  // busy is user and total is user + idle, and with `user` 100 ticks short of
  // 2^64 and then 50 ticks past it, idle moving 150 ticks. Both sums wrap between
  // the rounds, as an unsigned long does on a 64-bit kernel.
  fs.set(kStat, stat_text("cpu  " + std::to_string(kTop - 99U) + " 0 0 1000 0 0 0 0 0 0"));
  static_cast<void>(source.probe());
  const std::vector<Reading> before = sample(source);
  fs.set(kStat, stat_text("cpu  50 0 0 1150 0 0 0 0 0 0"));
  const std::vector<Reading> after = sample(source);

  const std::uint64_t busy_before = counter_of(before[0]);
  const std::uint64_t busy_after = counter_of(after[0]);
  EXPECT_LT(busy_after, busy_before) << "the raw value went down: that is the wrap";
  EXPECT_EQ(counter_delta(busy_before, busy_after, std::nullopt), 150U);
  EXPECT_EQ(counter_delta(counter_of(before[1]), counter_of(after[1]), std::nullopt), 300U);

  const std::optional<double> share = utilisation(CpuTicks{busy_before, counter_of(before[1])},
                                                  CpuTicks{busy_after, counter_of(after[1])});
  ASSERT_TRUE(share.has_value());
  EXPECT_DOUBLE_EQ(*share, 0.5);
}

TEST(UtilisationTest, TheShareOfTheIntervalTheCpusWereBusy) {
  EXPECT_DOUBLE_EQ(utilisation({100, 400}, {150, 500}).value(), 0.5);
  EXPECT_DOUBLE_EQ(utilisation({100, 400}, {100, 500}).value(), 0.0) << "all of it idle";
  EXPECT_DOUBLE_EQ(utilisation({100, 400}, {200, 500}).value(), 1.0) << "all of it busy";
}

TEST(UtilisationTest, AWrapIsAdvancedAndAStepBackIsNotAWrap) {
  EXPECT_DOUBLE_EQ(utilisation({kTop - 9U, kTop - 9U}, {10, 30}).value(), 0.5)
      << "busy and total both wrapped: 20 of 40";
  EXPECT_FALSE(utilisation({1000, 5000}, {1000, 4990}).has_value())
      << "the total went back by ten: accounting reconciled, cannot be told";
  EXPECT_FALSE(utilisation({1000, 5000}, {990, 5100}).has_value()) << "busy went back";
}

TEST(UtilisationTest, AnIntervalThatCannotBeToldIsNothingNeverANumber) {
  EXPECT_FALSE(utilisation({10, 100}, {10, 100}).has_value()) << "the clock did not tick";
  EXPECT_FALSE(utilisation({10, 100}, {200, 150}).has_value()) << "more busy than the total";
}

TEST(UtilisationTest, TheLargestAdvanceIsTheOneBelowHalfTheRange) {
  const std::uint64_t half = std::uint64_t{1} << 63U;
  EXPECT_DOUBLE_EQ(utilisation({0, 0}, {half - 1U, half - 1U}).value(), 1.0);
  EXPECT_FALSE(utilisation({0, 0}, {half, half}).has_value())
      << "an advance of 2^63 is a step back";
  EXPECT_FALSE(utilisation({0, 0}, {10, half}).has_value());
}

// --- the source contract, once per state (ADR 0003)

fixtures::Edit with_cpufreq() {
  return [](MemFs& fs) {
    fs.load(fixtures::fixture("sysfs/frequency/absent"));
    for (int cpu = 0; cpu < 4; ++cpu) {
      fs.set(freq_path(cpu), "2400000\n");
    }
  };
}

struct PresentState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FrequencySource>>(with_cpufreq());
  }
};

struct AbsentState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FrequencySource>>(
        [](MemFs& fs) { fs.load(fixtures::fixture("sysfs/frequency/absent")); });
  }
};

struct DeniedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FrequencySource>>([](MemFs& fs) {
      with_cpufreq()(fs);
      fs.fail_open(freq_path(1), EACCES);
      fs.fail_open(kStat, EACCES);
    });
  }
};

struct MalformedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FrequencySource>>([](MemFs& fs) {
      with_cpufreq()(fs);
      fs.set(freq_path(1), "999999999999\n");
      fs.set(kStat, "cpu  1 2\n");
    });
  }
};

struct VanishedState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<FrequencySource>>(with_cpufreq(), [](MemFs& fs) {
      fs.remove(freq_path(1));
      fs.remove(kStat);
    });
  }
};

/// The counters wrap between the second and the third round.
struct WrappedState {
  static constexpr int kRounds = 4;
  static std::unique_ptr<Source> make() {
    auto round = std::make_shared<std::uint64_t>(0);
    return std::make_unique<OwnedSource<FrequencySource>>(
        [](MemFs& fs) {
          with_cpufreq()(fs);
          fs.set(kStat, stat_text(ticks_line(kTop - 99U, 1418, 192171)));
        },
        fixtures::Edit{},
        [round](MemFs& fs) {
          ++*round;
          fs.set(kStat,
                 stat_text(ticks_line((kTop - 99U) + (*round * 80U), 1418, 192171 + *round * 80U)));
        });
  }
};

struct RealMachine {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<fixtures::RealMachineSource<FrequencySource>>();
  }
};

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

namespace {

using FrequencyStates =
    ::testing::Types<telemetry::PresentState, telemetry::AbsentState, telemetry::DeniedState,
                     telemetry::MalformedState, telemetry::VanishedState, telemetry::WrappedState,
                     telemetry::RealMachine>;

INSTANTIATE_TYPED_TEST_SUITE_P(FrequencySource, SourceContract, FrequencyStates);

}  // namespace
}  // namespace loadforge::contracts
