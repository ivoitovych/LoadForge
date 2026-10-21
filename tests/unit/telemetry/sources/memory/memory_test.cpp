// SPDX-License-Identifier: GPL-3.0-or-later
//
// The memory source: system memory from /proc/meminfo.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The six states, through the substituted platform seam (thermal/memfs.hpp),
// over the /proc/meminfo captured from this machine
// (tests/fixtures/procfs/meminfo/present, taken with cp). The absent,
// malformed and vanished states are that capture edited at run time by the test
// that needs them, and each says how; the denied and unreadable states are the
// seam refusing the open or the read. Memory is gauges, so there is no wrapped
// state.

#include "telemetry/sources/memory/memory.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
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

const std::string kMeminfo = "/proc/meminfo";

// The seven figures of the captured file, in kB, as the kernel printed them.
constexpr std::uint64_t kTotalKb = 16479424;
constexpr std::uint64_t kFreeKb = 15843844;
constexpr std::uint64_t kAvailableKb = 15977872;
constexpr std::uint64_t kBuffersKb = 3876;
constexpr std::uint64_t kCachedKb = 345588;
constexpr std::uint64_t kSwapTotalKb = 0;
constexpr std::uint64_t kSwapFreeKb = 0;

std::vector<Reading> sample(Source& source) {
  std::vector<Reading> readings;
  source.sample(readings);
  return readings;
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

/// The captured file with every line starting with `key` removed.
std::string without(std::string text, const std::string& key) {
  const std::size_t at = text.find("\n" + key + ":");
  if (at != std::string::npos) {
    text.erase(at + 1, text.find('\n', at + 1) - at);
  }
  return text;
}

/// The captured file with the line for `key` replaced by `line`.
std::string with_line(std::string text, const std::string& key, const std::string& line) {
  const std::size_t at = text.find("\n" + key + ":");
  const std::size_t end = text.find('\n', at + 1);
  text.replace(at + 1, end - at - 1, line);
  return text;
}

class MemoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs.load(fixtures::fixture("procfs/meminfo/present"));
    captured = fs.content_of(kMeminfo);
  }

  MemFs fs;
  platform::FileSystem files{fs};
  MemorySource source{files};
  std::string captured;
};

TEST_F(MemoryTest, TheCapturedFileGivesSevenGaugesInBytes) {
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 7U);
  const char* const names[] = {"memory.total",    "memory.free",   "memory.available",
                               "memory.buffers",  "memory.cached", "memory.swap_total",
                               "memory.swap_free"};
  for (std::size_t at = 0; at < statuses.size(); ++at) {
    EXPECT_EQ(statuses[at].metric.name, names[at]);
    EXPECT_EQ(statuses[at].metric.unit, Unit::kBytes);
    EXPECT_EQ(statuses[at].metric.kind, Kind::kGauge);
    EXPECT_EQ(statuses[at].metric.scope, Scope::kMachine);
    EXPECT_EQ(statuses[at].metric.scope_index, 0U);
    EXPECT_EQ(statuses[at].metric.provider, "procfs:meminfo");
    EXPECT_FALSE(statuses[at].unavailable.has_value()) << names[at];
  }
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 7U);
  const std::uint64_t kilobytes[] = {kTotalKb,  kFreeKb,      kAvailableKb, kBuffersKb,
                                     kCachedKb, kSwapTotalKb, kSwapFreeKb};
  for (std::size_t at = 0; at < readings.size(); ++at) {
    EXPECT_DOUBLE_EQ(gauge_of(readings[at]), static_cast<double>(kilobytes[at] * 1024U))
        << names[at];
  }
  EXPECT_EQ(source.name(), "memory");
}

TEST_F(MemoryTest, NoSwapIsAReadingOfZeroBytesNotAnAbsence) {
  static_cast<void>(source.probe());
  const std::vector<Reading> readings = sample(source);
  EXPECT_DOUBLE_EQ(gauge_of(readings[5]), 0.0);
  EXPECT_DOUBLE_EQ(gauge_of(readings[6]), 0.0);
}

TEST_F(MemoryTest, ARoundIsOneReadOfTheFileSoItsFiguresAreFromOneInstant) {
  static_cast<void>(source.probe());
  const int opened = fs.opens_of(kMeminfo);
  static_cast<void>(sample(source));
  EXPECT_EQ(fs.opens_of(kMeminfo), opened + 1);
  static_cast<void>(sample(source));
  EXPECT_EQ(fs.opens_of(kMeminfo), opened + 2);
  EXPECT_TRUE(fs.all_closed());
}

TEST_F(MemoryTest, ARootPrefixesTheFile) {
  fs.set("/mnt/m/proc/meminfo", captured);
  MemorySource rooted(files, "/mnt/m");
  EXPECT_EQ(rooted.probe().size(), 7U);
  EXPECT_FALSE(rooted.probe()[0].unavailable.has_value());
  EXPECT_EQ(MemorySource(files, "/mnt/none").probe()[0].unavailable->path,
            "/mnt/none/proc/meminfo");
}

TEST_F(MemoryTest, NothingIsSampledBeforeTheProbeAndSamplingAppends) {
  EXPECT_TRUE(sample(source).empty());
  static_cast<void>(source.probe());
  std::vector<Reading> readings = {Gauge{-1.0}};
  source.sample(readings);
  EXPECT_EQ(readings.size(), 8U);
}

// --- absent: a line the kernel does not write

TEST_F(MemoryTest, AKernelWithoutAFieldReportsThatOneAbsentAndTheRestPresent) {
  fs.set(kMeminfo, without(captured, "MemAvailable"));
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 7U);
  ASSERT_TRUE(statuses[2].unavailable.has_value());
  EXPECT_EQ(statuses[2].metric.name, "memory.available");
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[2].unavailable->path, kMeminfo);
  EXPECT_NE(statuses[2].unavailable->detail.find("MemAvailable"), std::string::npos);
  EXPECT_NE(statuses[2].unavailable->remediation.find("3.14"), std::string::npos);
  EXPECT_NE(statuses[2].unavailable->remediation.find("does not substitute an estimate"),
            std::string::npos);
  for (const std::size_t at : {0U, 1U, 3U, 4U, 5U, 6U}) {
    EXPECT_FALSE(statuses[at].unavailable.has_value());
  }
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 6U);
  EXPECT_DOUBLE_EQ(gauge_of(readings[2]), static_cast<double>(kBuffersKb * 1024U))
      << "the figures that remain are read in probe order";
}

TEST_F(MemoryTest, AKernelWithoutSwapSupportLacksBothSwapLines) {
  fs.set(kMeminfo, without(without(captured, "SwapTotal"), "SwapFree"));
  const std::vector<MetricStatus> statuses = source.probe();
  EXPECT_EQ(statuses[5].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[6].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(sample(source).size(), 5U);
}

TEST_F(MemoryTest, NoProcfsIsEverythingAbsentAndNothingIsSampled) {
  fs.remove(kMeminfo);
  const std::vector<MetricStatus> statuses = source.probe();
  ASSERT_EQ(statuses.size(), 7U);
  for (const MetricStatus& status : statuses) {
    ASSERT_TRUE(status.unavailable.has_value());
    EXPECT_EQ(status.unavailable->kind, Availability::kAbsent);
    EXPECT_NE(status.unavailable->remediation.find("procfs may not be mounted"), std::string::npos);
  }
  EXPECT_TRUE(sample(source).empty());
}

TEST_F(MemoryTest, AnEmptyFileIsEverythingAbsentBecauseNoLineIsThere) {
  fs.set(kMeminfo, "");
  const std::vector<MetricStatus> statuses = source.probe();
  for (const MetricStatus& status : statuses) {
    EXPECT_EQ(status.unavailable->kind, Availability::kAbsent);
  }
}

// --- the file cannot be read

TEST_F(MemoryTest, AFileThatCannotBeReadIsEveryMetricInThatStateWithItsRemedy) {
  struct Case {
    std::string name;
    Availability kind;
    void (*apply)(MemFs&);
    const char* remedy_word;
  };
  const Case cases[] = {
      {"denied", Availability::kDenied, [](MemFs& f) { f.fail_open(kMeminfo, EACCES); },
       "security-module"},
      {"denied by policy", Availability::kDenied, [](MemFs& f) { f.fail_open(kMeminfo, EPERM); },
       "security-module"},
      {"unreadable", Availability::kUnreadable, [](MemFs& f) { f.fail_read(kMeminfo, EIO); },
       "dmesg"},
      {"a signal storm", Availability::kUnreadable, [](MemFs& f) { f.fail_read(kMeminfo, EINTR); },
       "dmesg"},
      {"a directory", Availability::kUnreadable, [](MemFs& f) { f.make_directory(kMeminfo); },
       "dmesg"},
      {"a path through a file", Availability::kUnreadable,
       [](MemFs& f) { f.fail_open(kMeminfo, ENOTDIR); }, "dmesg"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    MemFs one;
    one.load(fixtures::fixture("procfs/meminfo/present"));
    c.apply(one);
    platform::FileSystem one_files(one);
    MemorySource probing(one_files);
    const std::vector<MetricStatus> statuses = probing.probe();
    ASSERT_EQ(statuses.size(), 7U);
    for (const MetricStatus& status : statuses) {
      ASSERT_TRUE(status.unavailable.has_value());
      EXPECT_EQ(status.unavailable->kind, c.kind);
      EXPECT_NE(status.unavailable->remediation.find(c.remedy_word), std::string::npos)
          << status.unavailable->remediation;
    }
    EXPECT_TRUE(sample(probing).empty());
    EXPECT_TRUE(one.all_closed());
  }
}

// --- malformed: a line that is not "<Key>: <number> kB"

TEST_F(MemoryTest, EveryMalformedLineIsRefusedForItsMetricAloneWithTheReason) {
  struct Case {
    std::string line;
    const char* reason;
  };
  const Case cases[] = {
      {"MemFree:        15843844", "is not in kB"},
      {"MemFree:        15843844 MB", "is not in kB"},
      {"MemFree:        15843844 kB extra", "is not in kB"},
      {"MemFree: kB", "is not in kB"},
      {"MemFree:", "is not in kB"},
      {"MemFree: -5 kB", "does not hold a number"},
      {"MemFree: 1 2 kB", "does not hold a number"},
      {"MemFree:\t5 kB", "does not hold a number"},
      {"MemFree: 18014398509481984 kB", "does not hold a number of kB a byte count can carry"},
      {"MemFree: 99999999999999999999999 kB",
       "does not hold a number of kB a byte count can carry"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.line);
    MemFs one;
    one.set(kMeminfo, with_line(captured, "MemFree", c.line));
    platform::FileSystem one_files(one);
    MemorySource probing(one_files);
    const std::vector<MetricStatus> statuses = probing.probe();
    ASSERT_EQ(statuses.size(), 7U);
    ASSERT_TRUE(statuses[1].unavailable.has_value());
    EXPECT_EQ(statuses[1].unavailable->kind, Availability::kMalformed);
    EXPECT_NE(statuses[1].unavailable->detail.find(c.reason), std::string::npos)
        << statuses[1].unavailable->detail;
    EXPECT_NE(statuses[1].unavailable->remediation.find("MemFree"), std::string::npos);
    EXPECT_FALSE(statuses[0].unavailable.has_value()) << "its neighbours are unaffected";
    EXPECT_FALSE(statuses[2].unavailable.has_value());
    EXPECT_EQ(sample(probing).size(), 6U);
  }
}

TEST_F(MemoryTest, AKeyThatAppearsTwiceIsMalformedNotTheFirstOfThem) {
  fs.set(kMeminfo, captured + "MemFree:        1 kB\n");
  const std::vector<MetricStatus> statuses = source.probe();
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kMalformed);
  EXPECT_NE(statuses[1].unavailable->detail.find("appears twice"), std::string::npos);
}

TEST_F(MemoryTest, TheLargestByteCountIsAcceptedAndOneKilobyteMoreIsNot) {
  const std::uint64_t largest_kb = UINT64_MAX / 1024U;
  fs.set(kMeminfo,
         with_line(captured, "MemFree", "MemFree: " + std::to_string(largest_kb) + " kB"));
  EXPECT_FALSE(source.probe()[1].unavailable.has_value());
  fs.set(kMeminfo,
         with_line(captured, "MemFree", "MemFree: " + std::to_string(largest_kb + 1U) + " kB"));
  EXPECT_EQ(source.probe()[1].unavailable->kind, Availability::kMalformed);
}

TEST_F(MemoryTest, AKeyThatOnlyStartsLikeAnotherIsNotThatKey) {
  fs.set(kMeminfo, with_line(captured, "MemTotal", "MemTotalExtra: 5 kB"));
  EXPECT_EQ(source.probe()[0].unavailable->kind, Availability::kAbsent);
}

// --- in the run

TEST_F(MemoryTest, AFileThatDisappearsMidRunIsVanishedForEveryMetricAndReadAgainWhenItReturns) {
  static_cast<void>(source.probe());
  fs.remove(kMeminfo);
  std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 7U) << "one per metric present at the probe";
  for (const Reading& reading : readings) {
    EXPECT_EQ(kind_of(reading), Availability::kVanished);
  }
  EXPECT_NE(std::get<Unavailable>(readings[0]).remediation.find("disappeared"), std::string::npos);
  fs.set(kMeminfo, captured);
  EXPECT_DOUBLE_EQ(gauge_of(sample(source)[0]), static_cast<double>(kTotalKb * 1024U));
}

TEST_F(MemoryTest, ALineThatDisappearsMidRunIsVanishedForThatMetricAlone) {
  static_cast<void>(source.probe());
  fs.set(kMeminfo, without(captured, "Buffers"));
  const std::vector<Reading> readings = sample(source);
  ASSERT_EQ(readings.size(), 7U);
  EXPECT_EQ(kind_of(readings[3]), Availability::kVanished);
  EXPECT_NE(std::get<Unavailable>(readings[3]).detail.find("Buffers"), std::string::npos);
  EXPECT_DOUBLE_EQ(gauge_of(readings[4]), static_cast<double>(kCachedKb * 1024U));
}

TEST_F(MemoryTest, AFileThatTurnsBadMidRunIsReportedAsWhatItNowIsNeverAbsent) {
  static_cast<void>(source.probe());
  fs.fail_open(kMeminfo, EACCES);
  EXPECT_EQ(kind_of(sample(source)[0]), Availability::kDenied) << "a permission does not vanish";
  fs.heal(kMeminfo);
  fs.fail_read(kMeminfo, EIO);
  EXPECT_EQ(kind_of(sample(source)[0]), Availability::kUnreadable);
  fs.heal(kMeminfo);
  fs.set(kMeminfo, with_line(captured, "MemTotal", "MemTotal: lots kB"));
  const std::vector<Reading> readings = sample(source);
  EXPECT_EQ(kind_of(readings[0]), Availability::kMalformed);
  EXPECT_DOUBLE_EQ(gauge_of(readings[1]), static_cast<double>(kFreeKb * 1024U));
}

TEST_F(MemoryTest, AMetricAbsentAtTheProbeIsNotSampledEvenIfItAppearsLater) {
  fs.set(kMeminfo, without(captured, "MemAvailable"));
  static_cast<void>(source.probe());
  fs.set(kMeminfo, captured);
  EXPECT_EQ(sample(source).size(), 6U) << "the set was fixed at the probe";
  EXPECT_EQ(source.probe().size(), 7U);
  EXPECT_EQ(sample(source).size(), 7U) << "and a probe again fixes it again";
}

// --- the source contract, once per state (ADR 0003)

fixtures::Edit with_captured_meminfo() {
  return [](MemFs& fs) { fs.load(fixtures::fixture("procfs/meminfo/present")); };
}

struct PresentState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<MemorySource>>(with_captured_meminfo());
  }
};

struct AbsentState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<MemorySource>>([](MemFs& fs) {
      with_captured_meminfo()(fs);
      fs.set(kMeminfo, without(fs.content_of(kMeminfo), "MemAvailable"));
    });
  }
};

struct NoProcfsState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<MemorySource>>([](MemFs&) {});
  }
};

struct DeniedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<MemorySource>>([](MemFs& fs) {
      with_captured_meminfo()(fs);
      fs.fail_open(kMeminfo, EACCES);
    });
  }
};

struct MalformedState {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<MemorySource>>([](MemFs& fs) {
      with_captured_meminfo()(fs);
      fs.set(kMeminfo, with_line(fs.content_of(kMeminfo), "MemFree", "MemFree: -1 kB"));
    });
  }
};

struct VanishedState {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<OwnedSource<MemorySource>>(with_captured_meminfo(),
                                                       [](MemFs& fs) { fs.remove(kMeminfo); });
  }
};

struct RealMachine {
  static constexpr int kRounds = 2;
  static std::unique_ptr<Source> make() {
    return std::make_unique<fixtures::RealMachineSource<MemorySource>>();
  }
};

}  // namespace
}  // namespace loadforge::telemetry

namespace loadforge::contracts {

namespace {

using MemoryStates =
    ::testing::Types<telemetry::PresentState, telemetry::AbsentState, telemetry::NoProcfsState,
                     telemetry::DeniedState, telemetry::MalformedState, telemetry::VanishedState,
                     telemetry::RealMachine>;

INSTANTIATE_TYPED_TEST_SUITE_P(MemorySource, SourceContract, MemoryStates);

}  // namespace
}  // namespace loadforge::contracts
