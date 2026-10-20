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

#include "contracts/telemetry/telemetry_contract.hpp"
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

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);

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

TEST(CountersSourceTest, EachEventIsOpenedForTheControllerAndItsChildrenInUserSpace) {
  Rig rig;
  EXPECT_EQ(rig.name(), "counters");
  const std::vector<MetricStatus> statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 5U);
  for (std::size_t i = 0; i < statuses.size(); ++i) {
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
    EXPECT_TRUE(opens[i].user_only) << "all an unprivileged process may count at paranoid 2";
    EXPECT_TRUE(opens[i].inherit) << "the workers are forked after the open";
  }
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
  ASSERT_EQ(readings.size(), 5U);
  EXPECT_TRUE(same_reading(readings[0], Counter{777}));
  EXPECT_TRUE(same_reading(readings[4], Counter{12})) << "never enabled yet: the count as it is";
}

TEST(CountersSourceTest, NothingIsReadBeforeTheProbeAndAReprobeClosesWhatItReplaces) {
  Rig rig;
  EXPECT_TRUE(sample_once(rig).empty());
  static_cast<void>(rig.probe());
  static_cast<void>(rig.probe());
  EXPECT_EQ(sample_once(rig).size(), 5U);
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

TEST(CountersSourceTest, AnEventThatSharedThePmuIsScaledToTheTimeItWasEnabled) {
  Rig rig{[](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(0, 0, {{platform::PerfCount{500, 2000, 1000}, 0}});
    syscalls.script_perf_reads(0, 1, {{platform::PerfCount{3, 3000, 2000}, 0}});
    syscalls.script_perf_reads(
        0, 3, {{platform::PerfCount{std::numeric_limits<std::uint64_t>::max(), 2, 1}, 0}});
  }};
  static_cast<void>(rig.probe());
  const auto readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{1000}));
  EXPECT_TRUE(same_reading(readings[1], Counter{4})) << "4.5 truncated: an estimate, as perf's";
  EXPECT_TRUE(same_reading(readings[2], Counter{std::numeric_limits<std::uint64_t>::max()}))
      << "the product is formed wide and clamped, never wrapped";
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
    ASSERT_EQ(statuses.size(), 5U) << tested.number;
    for (std::size_t i = 0; i < statuses.size(); ++i) {
      ASSERT_TRUE(statuses[i].unavailable.has_value()) << tested.number;
      const Unavailable& why = *statuses[i].unavailable;
      EXPECT_EQ(why.kind, tested.kind) << tested.number;
      EXPECT_EQ(why.path, subject_of(kAll[i]));
      EXPECT_TRUE(mentions(why.detail, "perf_event_open")) << why.detail;
      EXPECT_TRUE(mentions(
          why.detail, platform::describe(platform::SyscallError{tested.number, "x", ""}).substr(4)))
          << "the kernel's own words for errno " << tested.number << ": " << why.detail;
      EXPECT_EQ(why.remediation, perf_remediation(tested.kind, 2, why.path));
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
    ASSERT_EQ(statuses.size(), 5U);
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
  ASSERT_EQ(statuses.size(), 5U);
  EXPECT_EQ(statuses[2].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[2].metric.name, "counters.cache_misses");
  const auto readings = sample_once(rig);
  EXPECT_EQ(readings.size(), 4U) << "the refused one is not read";
}

// --- ioctl failures --------------------------------------------------------------

TEST(CountersSourceTest, ACounterThatCannotBeResetIsNotEnabledAndIsClosed) {
  FakeSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  syscalls.fail_perf_control(platform::PerfAction::kReset, EPERM);
  CountersSource source{syscalls, filesystem};
  const auto statuses = source.probe();
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

  readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{4})) << "a counter that comes back is read again";
}

TEST(CountersSourceTest, ACounterThatWrapsAt64BitsHasADeltaAcrossTheWrap) {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  Rig rig{[&](FakeSyscalls& syscalls) {
    syscalls.script_perf_reads(
        0, 0, {{platform::PerfCount{kMax - 4, 10, 10}, 0}, {platform::PerfCount{5, 20, 20}, 0}});
  }};
  const auto statuses = rig.probe();
  const auto before = std::get<Counter>(sample_once(rig)[0]);
  const auto after = std::get<Counter>(sample_once(rig)[0]);
  EXPECT_EQ(telemetry::counter_delta(before.raw, after.raw, statuses[0].metric.modulus), 10U);
}

// --- against the real kernel -----------------------------------------------------

TEST(CountersSourceRealMachineTest, TheRealKernelsAnswersAreClassifiedConsistently) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  CountersSource source{syscalls, filesystem};
  const auto statuses = source.probe();
  ASSERT_EQ(statuses.size(), 5U);
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
                std::holds_alternative<Unavailable>(reading));
  }
}

}  // namespace
}  // namespace loadforge::contracts
