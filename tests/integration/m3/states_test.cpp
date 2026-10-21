// SPDX-License-Identifier: GPL-3.0-or-later
//
// The six states of every source (IMPLEMENTATION M3: present, absent,
// permission-denied, malformed, disappears mid-run, counter wraps), followed
// through the whole stack and not only through the source: what the sampler
// puts on the timeline, what the statistics make of it, what the result says
// and what both writers print (M3 exit criteria 2, 3 and 4).

#include <gtest/gtest.h>
#include <linux/perf_event.h>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "stack.hpp"

namespace loadforge::m3 {
namespace {

using topology::Availability;

constexpr std::uint64_t kRaplRange = 262'143'328'850ULL;

struct Row {
  std::string source;  ///< The source's family.
  std::string metric;  ///< The metric whose fate is followed.
  std::uint32_t scope_index = 0;
  bool counter = false;
  /// Files: the one whose state is changed, what it holds when well, and when
  /// malformed. Empty for the performance counters, which are not files.
  std::string path;
  std::string good;
  std::string malformed;
  /// A counter that wraps: two readings, the second smaller, and the advance
  /// between them in the counter's own unit.
  std::string before_wrap;
  std::string after_wrap;
  std::uint64_t wrap_advance = 0;
  std::function<void(Machine&)> deny;
  std::function<void(Machine&)> refuse_everything;
  std::function<void(Machine&)> vanish;
  std::function<void(Machine&)> restore;
};

Row make_row(std::string source, std::string metric, std::uint32_t scope_index, bool counter,
             std::string path, std::string good, std::string malformed,
             std::string before_wrap = {}, std::string after_wrap = {},
             std::uint64_t wrap_advance = 0) {
  Row row;
  row.source = std::move(source);
  row.metric = std::move(metric);
  row.scope_index = scope_index;
  row.counter = counter;
  row.path = std::move(path);
  row.good = std::move(good);
  row.malformed = std::move(malformed);
  row.before_wrap = std::move(before_wrap);
  row.after_wrap = std::move(after_wrap);
  row.wrap_advance = wrap_advance;
  return row;
}

std::string stat_with_busy(std::uint64_t user) {
  return "cpu  " + std::to_string(user) + " 0 0 1000 0 0 0 0 0 0\ncpu0 1 0 0 1 0 0 0 0 0 0\n";
}

std::vector<Row> rows() {
  std::vector<Row> all;
  const auto file_row = [&](Row row) {
    row.deny = [path = row.path](Machine& m) { m.syscalls().deny_file(path, EACCES); };
    row.vanish = [path = row.path](Machine& m) { m.remove(path); };
    row.restore = [path = row.path, good = row.good](Machine& m) { m.set(path, good); };
    all.push_back(std::move(row));
  };
  file_row(make_row("thermal", "thermal.package", 0, false, "/sys/class/hwmon/hwmon0/temp1_input",
                    "45000", "hot"));
  file_row(make_row("fan", "fan.speed", 257, false, "/sys/class/hwmon/hwmon1/fan1_input", "1200",
                    "fast"));
  file_row(make_row("frequency", "frequency.cpu", 0, false,
                    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "2400000", "turbo"));
  file_row(make_row("power", "power.package", 0, true, "/sys/class/powercap/intel-rapl:0/energy_uj",
                    "1000000", "joules", std::to_string(kRaplRange - 100), "50", 150));
  file_row(make_row("edac", "edac.corrected", 0, true, "/sys/devices/system/edac/mc/mc0/ce_count",
                    "7", "many", "4294967294", "1", 3));
  file_row(make_row("frequency", "frequency.busy_ticks", 0, true, "/proc/stat", stat_with_busy(500),
                    "cpu busy", stat_with_busy(~std::uint64_t{0} - 9), stat_with_busy(5), 15));
  file_row(make_row("memory", "memory.available", 0, false, "/proc/meminfo",
                    "MemTotal: 8000 kB\nMemAvailable: 4000 kB\n", "MemAvailable: lots kB\n"));
  {
    Row row;
    row.source = "counters";
    row.metric = "counters.cycles";
    row.counter = true;
    row.deny = [](Machine& m) {
      m.syscalls().fail_perf_open(kPerfHardware, PERF_COUNT_HW_CPU_CYCLES, EACCES);
    };
    row.refuse_everything = [](Machine& m) { m.syscalls().fail_every_perf_open(ENOENT); };
    row.vanish = [](Machine& m) {
      m.syscalls().script_perf_reads(kPerfHardware, PERF_COUNT_HW_CPU_CYCLES,
                                     {{platform::PerfCount{}, ENODEV}});
    };
    row.restore = [](Machine& m) {
      m.syscalls().script_perf_reads(kPerfHardware, PERF_COUNT_HW_CPU_CYCLES,
                                     {{platform::PerfCount{50, 1, 1}, 0}});
    };
    all.push_back(std::move(row));
  }
  return all;
}

std::string label(const ::testing::TestParamInfo<Row>& info) {
  std::string name = info.param.metric;
  for (char& c : name) {
    if (c == '.') {
      c = '_';
    }
  }
  return name;
}

const telemetry::MetricStatus* status_of(const telemetry::CapabilityReport& report,
                                         const Row& row) {
  for (const telemetry::SourceStatus& source : report.sources) {
    for (const telemetry::MetricStatus& status : source.metrics) {
      if (status.metric.name == row.metric && status.metric.scope_index == row.scope_index) {
        return &status;
      }
    }
  }
  return nullptr;
}

const telemetry::SourceStatus* source_of(const telemetry::CapabilityReport& report,
                                         const std::string& name) {
  for (const telemetry::SourceStatus& source : report.sources) {
    if (source.source == name) {
      return &source;
    }
  }
  return nullptr;
}

class SourceStates : public ::testing::TestWithParam<Row> {};

TEST_P(SourceStates, Present) {
  const Row& row = GetParam();
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  const auto* status = status_of(stack.capabilities(), row);
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->unavailable.has_value());
  stack.rounds(3);
  const auto signature = stack.signature();
  const auto* summary = Stack::find(signature, row.metric);
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(std::holds_alternative<telemetry::Unavailable>(summary->value));
  EXPECT_EQ(summary->unavailable_samples, 0U);
}

TEST_P(SourceStates, AbsentIsAbsentWithARemedyAndNeverAZero) {
  const Row& row = GetParam();
  Stack stack;
  if (row.refuse_everything) {
    stack.machine.install_everything();
    row.refuse_everything(stack.machine);
  } else {
    stack.machine.install_everything_but({row.source});
  }
  stack.start();
  const auto* source = source_of(stack.capabilities(), row.source);
  ASSERT_NE(source, nullptr) << "a source with nothing to read still reports (F3)";
  ASSERT_FALSE(source->metrics.empty());
  for (const telemetry::MetricStatus& status : source->metrics) {
    ASSERT_TRUE(status.unavailable.has_value()) << status.metric.name;
    EXPECT_EQ(status.unavailable->kind, Availability::kAbsent) << status.metric.name;
    EXPECT_FALSE(status.unavailable->remediation.empty()) << status.metric.name;
  }
  stack.rounds(2);
  const auto signature = stack.signature();
  for (const statistics::MetricSummary& summary : signature.metrics) {
    EXPECT_NE(summary.metric.name.rfind(row.source + ".", 0), 0U)
        << summary.metric.name << " has a summary though it was absent at the probe";
  }
  const results::RunResult result = stack.result();
  const std::string json = Stack::json(result);
  for (const telemetry::MetricStatus& status : source->metrics) {
    EXPECT_NE(json.find(status.unavailable->remediation), std::string::npos)
        << "the JSON keeps the remedy of " << status.metric.name;
    EXPECT_NE(Stack::csv(result).find(status.unavailable->remediation), std::string::npos);
  }
}

TEST_P(SourceStates, DeniedIsDeniedWithItsRemedyAndTheRestOfTheSourceIsRead) {
  const Row& row = GetParam();
  Stack stack;
  stack.machine.install_everything();
  row.deny(stack.machine);
  stack.start();
  const auto* status = status_of(stack.capabilities(), row);
  ASSERT_NE(status, nullptr);
  ASSERT_TRUE(status->unavailable.has_value());
  EXPECT_EQ(status->unavailable->kind, Availability::kDenied);
  EXPECT_FALSE(status->unavailable->remediation.empty());
  stack.rounds(2);
  const auto signature = stack.signature();
  for (const statistics::MetricSummary& summary : signature.metrics) {
    EXPECT_FALSE(summary.metric.name == row.metric && summary.metric.scope_index == row.scope_index)
        << "a summary for the metric that could not be read";
  }
}

TEST_P(SourceStates, DisappearingMidRunIsRecordedAsVanishedAndNotAsZero) {
  const Row& row = GetParam();
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  stack.round();
  row.vanish(stack.machine);
  stack.rounds(2);
  row.restore(stack.machine);
  stack.rounds(2);

  telemetry::SeriesId series = 0;
  bool found = false;
  for (std::size_t at = 0; at < stack.timeline().series_count(); ++at) {
    const telemetry::Metric& metric = stack.timeline().metric(static_cast<telemetry::SeriesId>(at));
    if (metric.name == row.metric && metric.scope_index == row.scope_index) {
      series = static_cast<telemetry::SeriesId>(at);
      found = true;
    }
  }
  ASSERT_TRUE(found);
  const auto points = stack.timeline().points(series);
  ASSERT_EQ(points.size(), 5U);
  for (const std::size_t gone : {std::size_t{1}, std::size_t{2}}) {
    const auto* unavailable = std::get_if<telemetry::Unavailable>(&points[gone].reading);
    ASSERT_NE(unavailable, nullptr) << "round " << gone;
    EXPECT_EQ(unavailable->kind, Availability::kVanished) << "round " << gone;
    EXPECT_FALSE(unavailable->remediation.empty());
  }
  EXPECT_FALSE(std::holds_alternative<telemetry::Unavailable>(points[3].reading))
      << "a metric that comes back is read again";
  EXPECT_FALSE(std::holds_alternative<telemetry::Unavailable>(points[4].reading));

  const auto signature = stack.signature();
  const statistics::MetricSummary* summary = nullptr;
  for (const statistics::MetricSummary& candidate : signature.metrics) {
    if (candidate.metric.name == row.metric && candidate.metric.scope_index == row.scope_index) {
      summary = &candidate;
    }
  }
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->unavailable_samples, 2U)
      << "a sensor that dropped out says so beside its summary";
  if (const auto* counter = std::get_if<statistics::CounterSummary>(&summary->value)) {
    EXPECT_EQ(counter->samples, 3U);
  } else {
    EXPECT_EQ(std::get<statistics::GaugeSummary>(summary->value).samples, 3U);
  }
}

// The metrics read from a file, which a performance counter is not: only a file
// has text to be malformed.
class FileStates : public SourceStates {};

TEST_P(FileStates, MalformedIsMalformedAndNamesItsFile) {
  const Row& row = GetParam();
  Stack stack;
  stack.machine.install_everything();
  stack.machine.set(row.path, row.malformed);
  stack.start();
  const auto* status = status_of(stack.capabilities(), row);
  ASSERT_NE(status, nullptr);
  ASSERT_TRUE(status->unavailable.has_value());
  EXPECT_EQ(status->unavailable->kind, Availability::kMalformed);
  EXPECT_FALSE(status->unavailable->remediation.empty());
  EXPECT_FALSE(status->unavailable->detail.empty());
}

class CounterWraps : public SourceStates {};

TEST_P(CounterWraps, AWrapBetweenTwoReadingsIsTheAdvanceAndNotAStepBack) {
  const Row& row = GetParam();
  Stack stack;
  stack.machine.install_everything();
  stack.machine.set(row.path, row.before_wrap);
  stack.start();
  stack.round();
  stack.machine.set(row.path, row.after_wrap);
  stack.round();
  const auto signature = stack.signature();
  const auto* summary = Stack::find(signature, row.metric);
  ASSERT_NE(summary, nullptr);
  const auto* counter = std::get_if<statistics::CounterSummary>(&summary->value);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(counter->delta, row.wrap_advance);
  ASSERT_TRUE(counter->rate_per_second.has_value());
  EXPECT_DOUBLE_EQ(*counter->rate_per_second, static_cast<double>(row.wrap_advance));
}

// The performance counters are 64 bits wide and declare no modulus, so a wrap is the
// unsigned difference. Not a file, so it is not a row.
TEST(PerfCounterWraps, AWrapAtTwoToTheSixtyFourIsTheAdvanceAndNotAStepBack) {
  Stack stack;
  stack.machine.install_everything();
  stack.machine.syscalls().script_perf_reads(
      kPerfHardware, PERF_COUNT_HW_CPU_CYCLES,
      {{platform::PerfCount{~std::uint64_t{0} - 9, 1000, 1000}, 0},
       {platform::PerfCount{5, 1000, 1000}, 0}});
  stack.start();
  stack.rounds(2);
  const auto signature = stack.signature();
  const auto* cycles =
      std::get_if<statistics::CounterSummary>(&Stack::find(signature, "counters.cycles")->value);
  ASSERT_NE(cycles, nullptr);
  EXPECT_EQ(cycles->delta, 15U);
}

std::vector<Row> wrapping_rows() {
  std::vector<Row> out;
  for (Row& row : rows()) {
    if (!row.before_wrap.empty()) {
      out.push_back(std::move(row));
    }
  }
  return out;
}

INSTANTIATE_TEST_SUITE_P(Metrics, SourceStates, ::testing::ValuesIn(rows()), label);
std::vector<Row> file_rows() {
  std::vector<Row> out;
  for (Row& row : rows()) {
    if (!row.path.empty()) {
      out.push_back(std::move(row));
    }
  }
  return out;
}

INSTANTIATE_TEST_SUITE_P(Files, FileStates, ::testing::ValuesIn(file_rows()), label);
INSTANTIATE_TEST_SUITE_P(Counters, CounterWraps, ::testing::ValuesIn(wrapping_rows()), label);

}  // namespace
}  // namespace loadforge::m3
