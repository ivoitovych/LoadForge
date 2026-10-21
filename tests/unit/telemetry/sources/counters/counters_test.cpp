// SPDX-License-Identifier: GPL-3.0-or-later
//
// The performance-counter source under the six states of IMPLEMENTATION M3,
// with perf_event_paranoid as a state of its own and every errno of
// perf_event_open injected.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// A counter's states are the kernel's answers to perf_event_open and read(2),
// not files, so there is nothing to commit: each test scripts the answers on
// the fake seam (tests/support/fake_syscalls.hpp). The one file the source
// reads, perf_event_paranoid, is built per test in the fake's tree, because
// its value is the state under test. The real kernel's answers on the machine
// running the tests are asserted at the end, where this machine has a PMU or
// does not.

#include "telemetry/sources/counters/counters.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "contracts/telemetry/source_contract.hpp"
#include "platform/fs.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"

namespace loadforge::contracts {
namespace {

using loadforge::testing::FakeSyscalls;
using telemetry::classify_perf_open;
using telemetry::Counter;
using telemetry::CountersSource;
using telemetry::Gauge;
using telemetry::Kind;
using telemetry::MetricStatus;
using telemetry::perf_remediation;
using telemetry::Reading;
using telemetry::same_reading;
using telemetry::Scope;
using telemetry::Source;
using telemetry::Unavailable;
using telemetry::Unit;
using topology::Availability;

constexpr std::string_view kParanoidFile = "/proc/sys/kernel/perf_event_paranoid";

// The events, as (type, config): PERF_TYPE_HARDWARE is 0 and PERF_TYPE_SOFTWARE 1;
// the configs are PERF_COUNT_HW_CPU_CYCLES 0, _INSTRUCTIONS 1, _CACHE_MISSES 3,
// _BRANCH_MISSES 5 and PERF_COUNT_SW_CONTEXT_SWITCHES 3.
struct Event {
  std::string_view name;
  std::uint32_t type;
  std::uint64_t config;
};
constexpr Event kCycles{"counters.cycles", 0, 0};
constexpr Event kInstructions{"counters.instructions", 0, 1};
constexpr Event kCacheMisses{"counters.cache_misses", 0, 3};
constexpr Event kBranchMisses{"counters.branch_misses", 0, 5};
constexpr Event kContextSwitches{"counters.context_switches", 1, 3};
constexpr Event kAll[] = {kCycles, kInstructions, kCacheMisses, kBranchMisses, kContextSwitches};

/// A source over a fake kernel, holding the fake it reads. `between` runs
/// before each sample with the round number.
class Rig final : public Source {
 public:
  using Prepare = std::function<void(FakeSyscalls&)>;
  using Between = std::function<void(int, FakeSyscalls&)>;

  explicit Rig(const Prepare& prepare = nullptr, Between between = nullptr,
               std::optional<std::string> paranoid = "2\n")
      : between_(std::move(between)) {
    if (paranoid.has_value()) {
      syscalls.add_file(std::string{kParanoidFile}, *paranoid);
    } else {
      syscalls.add_file("/proc/elsewhere", "x");
    }
    if (prepare) {
      prepare(syscalls);
    }
  }
  [[nodiscard]] std::string_view name() const noexcept override { return source.name(); }
  [[nodiscard]] std::vector<MetricStatus> probe() override { return source.probe(); }
  void sample(std::vector<Reading>& out) override {
    if (between_) {
      between_(round_, syscalls);
    }
    ++round_;
    source.sample(out);
  }

  FakeSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  CountersSource source{syscalls, filesystem};

 private:
  Between between_;
  int round_ = 0;
};

// --- the contract, once per state ------------------------------------------------

struct PresentSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<Rig>(); }
};
/// What a virtual machine without a PMU answers (captured on this repository's
/// own development VM): ENOENT for every hardware event, a software event
/// granted.
struct AbsentSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>([](FakeSyscalls& syscalls) {
      for (const Event& event : {kCycles, kInstructions, kCacheMisses, kBranchMisses}) {
        syscalls.fail_perf_open(event.type, event.config, ENOENT);
      }
    });
  }
};
struct DeniedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>(
        [](FakeSyscalls& syscalls) { syscalls.fail_every_perf_open(EACCES); }, nullptr, "4\n");
  }
};
/// Malformed: the sysctl holds nonsense, and a counter that was granted never
/// ran.
struct MalformedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>(
        [](FakeSyscalls& syscalls) {
          syscalls.script_perf_reads(0, 1, {{platform::PerfCount{0, 500, 0}, 0}});
        },
        nullptr, "very\n");
  }
};
struct VanishedSubject {
  static constexpr int kRounds = 4;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>([](FakeSyscalls& syscalls) {
      syscalls.script_perf_reads(0, 0,
                                 {{platform::PerfCount{5, 10, 10}, 0},
                                  {platform::PerfCount{}, ENODEV},
                                  {platform::PerfCount{}, ENODEV},
                                  {platform::PerfCount{9, 20, 20}, 0}});
    });
  }
};
struct WrappedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>([](FakeSyscalls& syscalls) {
      constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
      syscalls.script_perf_reads(
          0, 0, {{platform::PerfCount{kMax - 4, 10, 10}, 0}, {platform::PerfCount{5, 20, 20}, 0}});
    });
  }
};

using States = ::testing::Types<PresentSubject, AbsentSubject, DeniedSubject, MalformedSubject,
                                VanishedSubject, WrappedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(CounterStates, SourceContract, States);

// --- helpers ---------------------------------------------------------------------

bool mentions(const std::string& text, std::string_view word) {
  return text.find(word) != std::string::npos;
}

const Unavailable& as_unavailable(const Reading& reading) { return std::get<Unavailable>(reading); }

std::vector<Reading> sample_once(Source& source) {
  std::vector<Reading> readings;
  source.sample(readings);
  return readings;
}

std::string subject_of(const Event& event) {
  return "perf event type " + std::to_string(event.type) + " config " +
         std::to_string(event.config);
}

// --- present ---------------------------------------------------------------------

TEST(CountersSourceTest, EachEventIsOpenedForTheControllerAndItsChildren) {
  Rig rig;
  EXPECT_EQ(rig.name(), "counters");
  const std::vector<MetricStatus> statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 6U) << "five events and the share of time on the PMU";
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(statuses[i].metric.name, kAll[i].name);
    EXPECT_FALSE(statuses[i].unavailable.has_value());
    EXPECT_EQ(statuses[i].metric.unit, Unit::kCount);
    EXPECT_EQ(statuses[i].metric.kind, Kind::kCounter);
    EXPECT_EQ(statuses[i].metric.scope, Scope::kMachine);
    EXPECT_FALSE(statuses[i].metric.modulus.has_value()) << "a 64-bit counter";
    EXPECT_EQ(statuses[i].metric.provider, i < 4 ? "perf:hardware" : "perf:software");
  }
  const auto& opens = rig.syscalls.perf_opens();
  ASSERT_EQ(opens.size(), 5U);
  for (std::size_t i = 0; i < opens.size(); ++i) {
    EXPECT_EQ(opens[i].type, kAll[i].type);
    EXPECT_EQ(opens[i].config, kAll[i].config);
    EXPECT_EQ(opens[i].pid, FakeSyscalls::kFakePid) << "the process, not an arbitrary thread";
    EXPECT_EQ(opens[i].cpu, -1);
    // User space for what the CPU counts (all an unprivileged process may count at
    // paranoid 2); the kernel included for a context switch, which happens in it.
    EXPECT_EQ(opens[i].user_only, kAll[i].type == 0) << kAll[i].name;
    EXPECT_TRUE(opens[i].inherit) << "the workers are forked after the open";
  }
  EXPECT_EQ(statuses[5].metric.name, "counters.pmu_share");
  EXPECT_FALSE(statuses[5].unavailable.has_value());
  EXPECT_EQ(statuses[5].metric.unit, Unit::kRatio);
  EXPECT_EQ(statuses[5].metric.kind, Kind::kGauge);
  EXPECT_EQ(statuses[5].metric.scope, Scope::kMachine);
  EXPECT_EQ(statuses[5].metric.provider, "perf:hardware");
  // Reset, then enable, for each: counts start from zero at their own enabling.
  const auto& controls = rig.syscalls.perf_controls();
  ASSERT_EQ(controls.size(), 10U);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(controls[2 * i].second, platform::PerfAction::kReset);
    EXPECT_EQ(controls[2 * i + 1].second, platform::PerfAction::kEnable);
    EXPECT_EQ(controls[2 * i].first, controls[2 * i + 1].first);
  }
}

TEST(CountersSourceTest, AReadingIsTheCountWhenTheEventRanTheWholeTime) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(0, 0, {{platform::PerfCount{777, 40, 40}, 0}});
    syscalls.script_perf_reads(1, 3, {{platform::PerfCount{12, 0, 0}, 0}});
  }};
  static_cast<void>(rig.probe());
  const auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 6U);
  EXPECT_TRUE(same_reading(readings[0], Counter{777}));
  EXPECT_TRUE(same_reading(readings[4], Counter{12})) << "never enabled yet: the count as it is";
  EXPECT_TRUE(same_reading(readings[5], Gauge{1.0})) << "every hardware event ran the whole time";
}

TEST(CountersSourceTest, NothingIsReadBeforeTheProbeAndAReprobeClosesWhatItReplaces) {
  Rig rig;
  EXPECT_TRUE(sample_once(rig).empty());
  static_cast<void>(rig.probe());
  static_cast<void>(rig.probe());
  EXPECT_EQ(sample_once(rig).size(), 6U);
  EXPECT_EQ(rig.syscalls.perf_opens().size(), 10U);
  EXPECT_EQ(rig.syscalls.open_count() - rig.syscalls.close_count(), 5)
      << "only the second probe's five descriptors are open: the first's were closed";
}

TEST(CountersSourceTest, EveryDescriptorIsClosedWithTheSourceEvenWhenACloseFails) {
  FakeSyscalls syscalls;
  syscalls.add_file(std::string{kParanoidFile}, "2\n");
  platform::FileSystem filesystem{syscalls};
  {
    CountersSource source{syscalls, filesystem};
    static_cast<void>(source.probe());
    syscalls.fail_close_of(syscalls.perf_fd_of(0, 0), EIO);
    EXPECT_FALSE(syscalls.all_descriptors_closed());
  }
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

// --- multiplexing ----------------------------------------------------------------

TEST(CountersSourceTest, AnEventThatSharedThePmuIsReportedAsItsRawCountNeverScaledUp) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(0, 0, {{platform::PerfCount{500, 2000, 1000}, 0}});
    syscalls.script_perf_reads(0, 1, {{platform::PerfCount{3, 3000, 2000}, 0}});
    syscalls.script_perf_reads(
        0, 3, {{platform::PerfCount{std::numeric_limits<std::uint64_t>::max(), 2, 1}, 0}});
  }};
  static_cast<void>(rig.probe());
  const auto readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{500})) << "the kernel's count: it only increases";
  EXPECT_TRUE(same_reading(readings[1], Counter{3}));
  EXPECT_TRUE(same_reading(readings[2], Counter{std::numeric_limits<std::uint64_t>::max()}));
  EXPECT_TRUE(same_reading(readings[5], Gauge{0.5}))
      << "the smallest share of time on the PMU among the hardware events (500 of 1000)";
}

// Disagreement 2 of the M3 integration: a count scaled up for the time the event
// was off the PMU is an estimate; scaled in one round and exact in the next it goes
// down, and a counter that goes down is a reset or a wrap. The raw count never does.
TEST(CountersSourceTest, TheCountDoesNotGoDownWhenTheEventStopsBeingTimeShared) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(0, 0,
                               {{platform::PerfCount{1000, 2000, 1000}, 0},    // shared PMU
                                {platform::PerfCount{1500, 2000, 2000}, 0}});  // exclusive again
  }};
  static_cast<void>(rig.probe());
  const auto first = sample_once(rig);
  const auto second = sample_once(rig);
  EXPECT_TRUE(same_reading(first[0], Counter{1000}));
  EXPECT_TRUE(same_reading(second[0], Counter{1500}));
  EXPECT_TRUE(same_reading(first[5], Gauge{0.5}));
  EXPECT_TRUE(same_reading(second[5], Gauge{1.0}));
}

TEST(CountersSourceTest, TheShareIsAtMostOneAndIsOneBeforeAnythingIsEnabled) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(0, 0, {{platform::PerfCount{5, 10, 12}, 0}});  // running > enabled
    syscalls.script_perf_reads(0, 1, {{platform::PerfCount{0, 0, 0}, 0}});    // not yet enabled
  }};
  static_cast<void>(rig.probe());
  EXPECT_TRUE(same_reading(sample_once(rig)[5], Gauge{1.0}));
}

TEST(CountersSourceTest, TheShareIsUnreadableWhenNoHardwareEventCanBeRead) {
  Rig rig{[](FakeSyscalls& syscalls) {
    for (const Event& event : {kCycles, kInstructions, kCacheMisses, kBranchMisses}) {
      syscalls.script_perf_reads(event.type, event.config, {{platform::PerfCount{}, ENODEV}});
    }
  }};
  static_cast<void>(rig.probe());
  const auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 6U);
  const Unavailable& why = as_unavailable(readings[5]);
  EXPECT_EQ(why.kind, Availability::kUnreadable);
  EXPECT_FALSE(why.detail.empty());
  EXPECT_FALSE(why.remediation.empty());
  EXPECT_TRUE(same_reading(readings[4], Counter{1000})) << "the software event is not affected";
}

TEST(CountersSourceTest, TheShareIsAbsentForTheReasonTheFirstHardwareEventIsWhenNoneIsHeld) {
  Rig rig{[](FakeSyscalls& syscalls) { syscalls.fail_every_perf_open(EACCES); }, nullptr, "3\n"};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 6U);
  ASSERT_TRUE(statuses[5].unavailable.has_value());
  EXPECT_EQ(*statuses[5].unavailable, *statuses[0].unavailable);
  EXPECT_EQ(statuses[5].metric.name, "counters.pmu_share");
  EXPECT_TRUE(sample_once(rig).empty()) << "nothing held, nothing read";

  // Only the software event held: the share is absent, the software count present.
  Rig software{[](FakeSyscalls& syscalls) {
    for (const Event& event : {kCycles, kInstructions, kCacheMisses, kBranchMisses}) {
      syscalls.fail_perf_open(event.type, event.config, ENOENT);
    }
  }};
  const auto statuses_software = software.probe();
  EXPECT_EQ(statuses_software[5].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(sample_once(software).size(), 1U);
}

// Disagreement 4 of the M3 integration. With the kernel excluded a context switch
// is never counted (the scheduler counts it in the kernel): on Linux 6.18 the count
// stays 0 across five 20 ms sleeps, and without the exclusion it is 1, 2, 3, 4, 5.
// perf_event_open(2) says an unprivileged process may leave the kernel in only
// when perf_event_paranoid is 1 or below (EACCES otherwise; also seen on 6.18 as
// uid 65534: exclude_kernel=0 at paranoid 2 is EACCES, at paranoid 1 it counts).
TEST(CountersSourceTest, AnEventThatHappensInTheKernelIsNotOpenedWithTheKernelExcluded) {
  Rig rig;
  static_cast<void>(rig.probe());
  bool software = false;
  for (const platform::PerfEventSpec& spec : rig.syscalls.perf_opens()) {
    if (spec.type == 1) {
      software = true;
      EXPECT_FALSE(spec.user_only) << "config " << spec.config;
    } else {
      EXPECT_TRUE(spec.user_only) << "the CPU's events stay user space only";
    }
  }
  EXPECT_TRUE(software);
}

TEST(CountersSourceTest, TheContextSwitchCounterNeedsTheKernelAndSaysSoWhenDenied) {
  // Unprivileged at paranoid 2: the hardware events are granted (user space) and the
  // software event is refused (kernel included).
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.fail_perf_open(kContextSwitches.type, kContextSwitches.config, EACCES);
  }};
  const auto statuses = rig.probe();
  ASSERT_TRUE(statuses[4].unavailable.has_value());
  EXPECT_EQ(statuses[4].unavailable->kind, Availability::kDenied);
  const std::string& remedy = statuses[4].unavailable->remediation;
  EXPECT_TRUE(mentions(remedy, "CAP_PERFMON"));
  EXPECT_TRUE(mentions(remedy, "perf_event_paranoid to 1")) << remedy;
  EXPECT_TRUE(mentions(remedy, "(it is 2)")) << remedy;
  EXPECT_TRUE(mentions(remedy, "in the kernel"));
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_EQ(sample_once(rig).size(), 5U) << "four counts, and the share; none for the denied one";

  const std::string unknown = perf_remediation(Availability::kDenied, std::nullopt, "x", true);
  EXPECT_TRUE(mentions(unknown, "perf_event_paranoid to 1"));
  EXPECT_FALSE(mentions(unknown, "(it is"));
  EXPECT_TRUE(mentions(perf_remediation(Availability::kAbsent, 2, "x", true), "nothing can be"));
}

TEST(CountersSourceTest, AReadOfTheKernelSideEventThatIsDeniedCarriesTheSameAdvice) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(kContextSwitches.type, kContextSwitches.config,
                               {{platform::PerfCount{}, EACCES}});
  }};
  static_cast<void>(rig.probe());
  const auto readings = sample_once(rig);
  EXPECT_EQ(as_unavailable(readings[4]).kind, Availability::kDenied);
  EXPECT_TRUE(mentions(as_unavailable(readings[4]).remediation, "perf_event_paranoid to 1"));
}

TEST(CountersSourceTest, AnEventThatNeverRanIsUnreadableNotZero) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(0, 0, {{platform::PerfCount{0, 123456, 0}, 0}});
  }};
  static_cast<void>(rig.probe());
  const auto readings = sample_once(rig);
  const Unavailable& starved = as_unavailable(readings[0]);
  EXPECT_EQ(starved.kind, Availability::kUnreadable);
  EXPECT_EQ(starved.path, subject_of(kCycles));
  EXPECT_EQ(starved.detail, "enabled for 123456 ns and never scheduled onto a counter");
  EXPECT_TRUE(mentions(starved.remediation, "nmi_watchdog"));
  EXPECT_TRUE(same_reading(readings[1], Counter{1000}));
}

// --- every errno of perf_event_open ----------------------------------------------

struct ErrnoCase {
  int number;
  Availability kind;
};

// The sixteen errnos the man page documents, and what each means for a counter.
const ErrnoCase kErrnos[] = {
    {EACCES, Availability::kDenied},        {EPERM, Availability::kDenied},
    {ENOENT, Availability::kAbsent},        {ENODEV, Availability::kAbsent},
    {EOPNOTSUPP, Availability::kAbsent},    {ENOSYS, Availability::kAbsent},
    {EINVAL, Availability::kAbsent},        {E2BIG, Availability::kUnreadable},
    {EBADF, Availability::kUnreadable},     {EBUSY, Availability::kUnreadable},
    {EFAULT, Availability::kUnreadable},    {EINTR, Availability::kUnreadable},
    {EMFILE, Availability::kUnreadable},    {ENOSPC, Availability::kUnreadable},
    {EOVERFLOW, Availability::kUnreadable}, {ESRCH, Availability::kUnreadable},
};

TEST(CountersSourceTest, EveryErrnoOfPerfEventOpenIsClassifiedAndInjected) {
  for (const ErrnoCase& tested : kErrnos) {
    EXPECT_EQ(classify_perf_open(tested.number), tested.kind) << tested.number;
    Rig rig{[&](FakeSyscalls& syscalls) { syscalls.fail_every_perf_open(tested.number); }};
    const auto statuses = rig.probe();
    ASSERT_EQ(statuses.size(), 6U) << tested.number;
    ASSERT_EQ(*statuses[5].unavailable, *statuses[0].unavailable)
        << "the share: the first's reason";
    for (std::size_t i = 0; i < 5; ++i) {
      ASSERT_TRUE(statuses[i].unavailable.has_value()) << tested.number;
      const Unavailable& why = *statuses[i].unavailable;
      EXPECT_EQ(why.kind, tested.kind) << tested.number;
      EXPECT_EQ(why.path, subject_of(kAll[i]));
      EXPECT_TRUE(mentions(why.detail, "perf_event_open")) << why.detail;
      EXPECT_TRUE(mentions(
          why.detail, platform::describe(platform::SyscallError{tested.number, "x", ""}).substr(4)))
          << "the kernel's own words for errno " << tested.number << ": " << why.detail;
      EXPECT_EQ(why.remediation, perf_remediation(tested.kind, 2, why.path, kAll[i].type == 1));
    }
    EXPECT_TRUE(sample_once(rig).empty()) << "an event not opened is not read";
    EXPECT_TRUE(rig.syscalls.perf_controls().empty());
  }
}

TEST(CountersSourceTest, AnErrnoThePageDoesNotDocumentIsAFailureNotAbsence) {
  EXPECT_EQ(classify_perf_open(EEXIST), Availability::kUnreadable);
  EXPECT_EQ(classify_perf_open(0), Availability::kUnreadable);
}

// --- perf_event_paranoid, the state of its own ----------------------------------

TEST(CountersSourceTest, TheParanoiaLevelIsNamedInTheDetailOfADenial) {
  for (const char* level : {"-1", "0", "1", "2", "3", "4"}) {
    Rig rig{[](FakeSyscalls& syscalls) { syscalls.fail_every_perf_open(EACCES); }, nullptr,
            std::string{level} + "\n"};
    const auto statuses = rig.probe();
    const Unavailable& why = *statuses[0].unavailable;
    EXPECT_EQ(why.kind, Availability::kDenied);
    EXPECT_TRUE(mentions(why.detail, std::string{"(kernel.perf_event_paranoid is "} + level + ")"))
        << why.detail;
    EXPECT_TRUE(mentions(why.remediation, std::string{"is "} + level) ||
                mentions(why.remediation, std::string{"it is "} + level))
        << why.remediation;
  }
}

TEST(CountersSourceTest, AboveTwoTheAdviceIsToLowerItAtTwoOrBelowItIsNotTheSysctl) {
  const std::string high = perf_remediation(Availability::kDenied, 3, "x");
  EXPECT_TRUE(mentions(high, "lower kernel.perf_event_paranoid"));
  EXPECT_TRUE(mentions(high, "above 2 every unprivileged use is refused"));
  EXPECT_TRUE(mentions(high, "CAP_PERFMON"));

  const std::string low = perf_remediation(Availability::kDenied, 2, "x");
  EXPECT_TRUE(mentions(low, "seccomp"));
  EXPECT_TRUE(mentions(low, "already allows"));
  EXPECT_FALSE(mentions(low, "lower kernel.perf_event_paranoid"))
      << "lowering it would not help, so it is not advised";

  const std::string unknown = perf_remediation(Availability::kDenied, std::nullopt, "x");
  EXPECT_TRUE(mentions(unknown, "CAP_PERFMON"));
  EXPECT_TRUE(mentions(unknown, "lower kernel.perf_event_paranoid"));
  EXPECT_FALSE(mentions(unknown, "(it is"));
}

TEST(CountersSourceTest, AParanoiaFileThatCannotBeReadLeavesTheLevelUnknownNotTheCountersUnusable) {
  for (const std::optional<std::string>& content :
       {std::optional<std::string>{"high\n"}, std::optional<std::string>{}}) {
    Rig rig{[](FakeSyscalls& syscalls) { syscalls.fail_every_perf_open(EPERM); }, nullptr, content};
    const auto statuses = rig.probe();
    ASSERT_EQ(statuses.size(), 6U);
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
    EXPECT_FALSE(mentions(statuses[0].unavailable->detail, "paranoid"));
  }
  Rig granted{nullptr, nullptr, std::nullopt};
  EXPECT_FALSE(granted.probe()[0].unavailable.has_value());
}

TEST(CountersSourceTest, TheRemedyIsThePerfProvidersAndNeverAFilePermissionFix) {
  const std::string denied = perf_remediation(Availability::kDenied, std::nullopt, "x");
  for (const std::string_view wrong :
       {"udev", "group ownership", "read access", "chmod", "root-only"}) {
    EXPECT_FALSE(mentions(denied, wrong)) << wrong;
  }
  const std::string absent =
      perf_remediation(Availability::kAbsent, 2, "perf event type 0 config 0");
  EXPECT_TRUE(mentions(absent, "perf event type 0 config 0"));
  EXPECT_TRUE(mentions(absent, "nothing can be changed"));
  for (const std::string_view wrong : {"CAP_PERFMON", "paranoid", "privileg"}) {
    EXPECT_FALSE(mentions(absent, wrong)) << "a fix that cannot work: " << wrong;
  }
}

TEST(CountersSourceTest, EveryStateHasItsOwnAdviceAndAStateThatIsNotOneGetsTheGenericOne) {
  std::vector<std::string> advice;
  for (const Availability kind : {Availability::kAbsent, Availability::kVanished,
                                  Availability::kDenied, Availability::kUnreadable}) {
    advice.push_back(perf_remediation(kind, 2, "subject"));
  }
  for (std::size_t i = 0; i < advice.size(); ++i) {
    for (std::size_t j = i + 1; j < advice.size(); ++j) {
      EXPECT_NE(advice[i], advice[j]);
    }
  }
  const std::string generic = perf_remediation(Availability::kUnreadable, 2, "subject");
  EXPECT_EQ(perf_remediation(Availability::kMalformed, 2, "subject"), generic);
  EXPECT_EQ(perf_remediation(Availability::kPresent, 2, "subject"), generic);
  EXPECT_EQ(perf_remediation(static_cast<Availability>(99), 2, "subject"), generic);
  EXPECT_TRUE(mentions(generic, "subject"));
}

// --- one event refused, the others granted --------------------------------------

TEST(CountersSourceTest, OneRefusedEventDoesNotTakeTheOthersWithIt) {
  Rig rig{[](FakeSyscalls& syscalls) { syscalls.fail_perf_open(0, 3, ENOENT); }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 6U);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[2].metric.name, "counters.cache_misses");
  const auto readings = sample_once(rig);
  EXPECT_EQ(readings.size(), 5U) << "the refused one is not read; the share is";
}

// --- ioctl failures --------------------------------------------------------------

TEST(CountersSourceTest, ACounterThatCannotBeResetIsNotEnabledAndIsClosed) {
  FakeSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  syscalls.fail_perf_control(platform::PerfAction::kReset, EPERM);
  CountersSource source{syscalls, filesystem};
  const auto statuses = source.probe();
  ASSERT_EQ(statuses.size(), 6U);
  for (const MetricStatus& status : statuses) {
    ASSERT_TRUE(status.unavailable.has_value());
    EXPECT_EQ(status.unavailable->kind, Availability::kUnreadable);
    EXPECT_TRUE(mentions(status.unavailable->detail, "ioctl"));
  }
  EXPECT_EQ(syscalls.perf_controls().size(), 5U) << "a reset that fails ends that counter's start";
  for (const auto& control : syscalls.perf_controls()) {
    EXPECT_EQ(control.second, platform::PerfAction::kReset);
  }
  EXPECT_TRUE(syscalls.all_descriptors_closed()) << "nothing leaked";
  EXPECT_TRUE(sample_once(source).empty());
}

TEST(CountersSourceTest, ACounterThatCannotBeEnabledIsClosedToo) {
  FakeSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  syscalls.fail_perf_control(platform::PerfAction::kEnable, EBADF);
  CountersSource source{syscalls, filesystem};
  const auto statuses = source.probe();
  ASSERT_EQ(statuses.size(), 6U);
  for (const MetricStatus& status : statuses) {
    EXPECT_EQ(status.unavailable->kind, Availability::kUnreadable);
  }
  EXPECT_EQ(syscalls.perf_controls().size(), 10U);
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

// --- reads that fail -------------------------------------------------------------

TEST(CountersSourceTest, AReadThatFailsIsClassifiedByWhatTheKernelSaidAndRecovers) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(
        0, 0, {{platform::PerfCount{}, ENODEV}, {platform::PerfCount{4, 8, 8}, 0}});
    syscalls.script_perf_reads(0, 1, {{platform::PerfCount{}, EACCES}});
    syscalls.script_perf_reads(0, 3, {{platform::PerfCount{}, EIO}});
  }};
  static_cast<void>(rig.probe());
  auto readings = sample_once(rig);
  const Unavailable& vanished = as_unavailable(readings[0]);
  EXPECT_EQ(vanished.kind, Availability::kVanished);
  EXPECT_EQ(vanished.path, subject_of(kCycles));
  EXPECT_TRUE(mentions(vanished.remediation, "went away"));
  EXPECT_EQ(as_unavailable(readings[1]).kind, Availability::kDenied);
  EXPECT_TRUE(mentions(as_unavailable(readings[1]).remediation, "CAP_PERFMON"));
  EXPECT_EQ(as_unavailable(readings[2]).kind, Availability::kUnreadable);
  EXPECT_TRUE(same_reading(readings[3], Counter{1000}));
  EXPECT_TRUE(same_reading(readings[5], Gauge{1.0})) << "the share of the events that were read";

  readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{4})) << "a counter that comes back is read again";
}

TEST(CountersSourceTest, A64BitCountHasNoModulusSoAStepDownIsAResetNotAWrap) {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  Rig rig{[&](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(
        0, 0, {{platform::PerfCount{kMax - 4, 10, 10}, 0}, {platform::PerfCount{5, 20, 20}, 0}});
  }};
  const auto statuses = rig.probe();
  EXPECT_FALSE(statuses[0].metric.modulus.has_value());
  const auto before = std::get<Counter>(sample_once(rig)[0]);
  const auto after = std::get<Counter>(sample_once(rig)[0]);
  EXPECT_FALSE(
      telemetry::counter_advance(before.raw, after.raw, statuses[0].metric.modulus).has_value());
}

// --- against the real kernel -----------------------------------------------------

TEST(CountersSourceRealMachineTest, TheRealKernelsAnswersAreClassifiedConsistently) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  CountersSource source{syscalls, filesystem};
  const auto statuses = source.probe();
  ASSERT_EQ(statuses.size(), 6U);
  std::size_t present = 0;
  for (const MetricStatus& status : statuses) {
    if (!status.unavailable.has_value()) {
      ++present;
      continue;
    }
    // On a machine without a PMU the hardware events are unsupported; where perf
    // is gated they are denied. Nothing else is a fact about a healthy kernel.
    EXPECT_TRUE(status.unavailable->kind == Availability::kAbsent ||
                status.unavailable->kind == Availability::kDenied)
        << status.metric.name << ": " << status.unavailable->detail;
    EXPECT_FALSE(status.unavailable->remediation.empty());
  }
  std::vector<Reading> readings;
  source.sample(readings);
  EXPECT_EQ(readings.size(), present);
  for (const Reading& reading : readings) {
    EXPECT_TRUE(std::holds_alternative<Counter>(reading) ||
                std::holds_alternative<Gauge>(reading) ||
                std::holds_alternative<Unavailable>(reading));
  }
}

}  // namespace
}  // namespace loadforge::contracts
