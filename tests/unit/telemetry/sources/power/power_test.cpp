// SPDX-License-Identifier: GPL-3.0-or-later
//
// The package-power source under the six states of IMPLEMENTATION M3, through
// both of its providers: powercap's energy_uj, which wraps at
// max_energy_range_uj, and the power PMU through perf_event_open.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Three states cannot be committed and are built here. A node readable by root
// only arrives from a clone readable (git records the execute bit and nothing
// else, and CI is root, which no mode refuses), so denial is the fake's refusal
// of the open over the committed tree's files. A node that disappears mid-run
// and a counter that wraps are events, not states: only a test that changes the
// files between two samples produces them. The present, absent and malformed
// states are committed trees (tests/fixtures/sysfs/powercap), read through the
// real system calls where nothing needs refusing.

#include "telemetry/sources/power/power.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
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
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/sources/counters/counters.hpp"

namespace loadforge::contracts {
namespace {

using loadforge::testing::FakeSyscalls;
using telemetry::Counter;
using telemetry::Kind;
using telemetry::MetricStatus;
using telemetry::perf_remediation;
using telemetry::powercap_remediation;
using telemetry::PowerSource;
using telemetry::Reading;
using telemetry::same_reading;
using telemetry::Scope;
using telemetry::Source;
using telemetry::Unavailable;
using telemetry::Unit;
using topology::Availability;

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);

const std::filesystem::path kFixtures =
    std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" / "sysfs" / "powercap";

constexpr std::uint64_t kRange = 262143328850;  // intel-two-packages' max_energy_range_uj
constexpr std::string_view kZone0 = "/sys/class/powercap/intel-rapl:0";
constexpr std::string_view kZone1 = "/sys/class/powercap/intel-rapl:1";
constexpr std::string_view kPmu = "/sys/bus/event_source/devices/power";

std::string at(std::string_view directory, std::string_view file) {
  return std::string{directory} + "/" + std::string{file};
}

/// A source over a fake kernel, holding the fake it reads. `between` runs
/// before each sample with the round number.
class Rig final : public Source {
 public:
  using Prepare = std::function<void(FakeSyscalls&)>;
  using Between = std::function<void(int, FakeSyscalls&)>;

  explicit Rig(const Prepare& prepare = nullptr, Between between = nullptr)
      : between_(std::move(between)) {
    syscalls.add_file("/sys/elsewhere", "x");  // tree mode, even with nothing else
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
  PowerSource source{syscalls, filesystem};

 private:
  Between between_;
  int round_ = 0;
};

void powercap(FakeSyscalls& syscalls) {
  syscalls.add_tree(kFixtures / "intel-two-packages", "/sys");
}
void pmu_two(FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "perf-two-packages", "/sys"); }
void pmu_one(FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "perf-one-package", "/sys"); }

// --- the contract, once per state ------------------------------------------------

struct PresentSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<Rig>(powercap); }
};
struct PresentPmuSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<Rig>(pmu_two); }
};
struct AbsentSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<Rig>(); }
};
/// The real power PMU of the development VM: psys energy only, no package event.
struct PsysOnlySubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>(
        [](FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "perf-psys-only", "/sys"); });
  }
};
struct DeniedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>([](FakeSyscalls& syscalls) {
      powercap(syscalls);
      syscalls.deny_file(at(kZone0, "energy_uj"), EACCES);
    });
  }
};
struct MalformedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>(
        [](FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "malformed-energy", "/sys"); });
  }
};
struct VanishedSubject {
  static constexpr int kRounds = 4;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>(powercap, [](int round, FakeSyscalls& syscalls) {
      if (round == 1) {
        syscalls.remove_file(at(kZone0, "energy_uj"));
      }
      if (round == 3) {
        syscalls.add_file(at(kZone0, "energy_uj"), "1000\n");
      }
    });
  }
};
struct WrappedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<Rig>(
        [](FakeSyscalls& syscalls) {
          powercap(syscalls);
          syscalls.add_file(at(kZone0, "energy_uj"), "262143328000\n");
        },
        [](int round, FakeSyscalls& syscalls) {
          if (round == 1) {
            syscalls.add_file(at(kZone0, "energy_uj"), "500\n");
          }
        });
  }
};

using States = ::testing::Types<PresentSubject, PresentPmuSubject, AbsentSubject, PsysOnlySubject,
                                DeniedSubject, MalformedSubject, VanishedSubject, WrappedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(PowerStates, SourceContract, States);

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

// --- powercap: present -----------------------------------------------------------

TEST(PowerSourceTest, EachPackageIsACounterOfMicrojoulesThatWrapsAtItsRange) {
  Rig rig{powercap};
  EXPECT_EQ(rig.name(), "power");
  const std::vector<MetricStatus> statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U) << "the sub-zone intel-rapl:0:0 is not a package";
  for (std::uint32_t package = 0; package < 2; ++package) {
    const MetricStatus& status = statuses[package];
    EXPECT_FALSE(status.unavailable.has_value());
    EXPECT_EQ(status.metric.name, "power.package");
    EXPECT_EQ(status.metric.unit, Unit::kJoules);
    EXPECT_EQ(status.metric.kind, Kind::kCounter);
    EXPECT_EQ(status.metric.scope, Scope::kPackage);
    EXPECT_EQ(status.metric.scope_index, package);
    EXPECT_EQ(status.metric.modulus, kRange);
    EXPECT_EQ(status.metric.provider, "powercap:intel-rapl");
  }
  const auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 2U);
  EXPECT_TRUE(same_reading(readings[0], Counter{123456789012}));
  EXPECT_TRUE(same_reading(readings[1], Counter{98765432100}));
  EXPECT_TRUE(rig.syscalls.perf_opens().empty()) << "powercap served both: no counter is held";
}

TEST(PowerSourceTest, AZoneThatIsNotAPackageIsLeftAlone) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.add_file("/sys/class/powercap/intel-rapl:2/name", "psys\n");
    syscalls.add_file("/sys/class/powercap/intel-rapl:2/energy_uj", "1\n");
    syscalls.add_file("/sys/class/powercap/intel-rapl:2/max_energy_range_uj", "100\n");
    syscalls.add_file("/sys/class/powercap/intel-rapl:3/name", "package-1\n");
    syscalls.add_file("/sys/class/powercap/intel-rapl:3/energy_uj", "7\n");
    syscalls.add_file("/sys/class/powercap/intel-rapl:3/max_energy_range_uj", "100\n");
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 3U) << "zone 2 is psys; zone 3 is the third package";
  EXPECT_EQ(statuses[2].metric.scope_index, 2U) << "packages are numbered as they are found";
  EXPECT_TRUE(same_reading(sample_once(rig)[2], Counter{7}));
}

TEST(PowerSourceTest, RealSystemCallsReadTheCommittedTreeToo) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  const std::string root = (kFixtures / "intel-two-packages").string();
  PowerSource source{syscalls, filesystem, root};
  const auto statuses = source.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_TRUE(same_reading(sample_once(source)[1], Counter{98765432100}));
}

// --- absent ----------------------------------------------------------------------

TEST(PowerSourceTest, NoPowerInterfaceAtAllIsOneAbsentMetricWithNothingToTryFixing) {
  Rig rig;
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].metric.name, "power.package");
  ASSERT_TRUE(statuses[0].unavailable.has_value());
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  for (const std::string_view advice : {"privileg", "CAP_", "udev", "chmod", "root"}) {
    EXPECT_FALSE(mentions(statuses[0].unavailable->remediation, advice)) << advice;
  }
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "nothing can be changed"));
  EXPECT_TRUE(sample_once(rig).empty());
}

TEST(PowerSourceTest, ThePowerPmuOfThisVirtualMachineHasNoPackageEventAndIsAbsentNotBroken) {
  // Captured from the development VM (tests/fixtures/sysfs/powercap/README.md).
  Rig rig{[](FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "perf-psys-only", "/sys"); }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 1U);
  ASSERT_TRUE(statuses[0].unavailable.has_value());
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  EXPECT_TRUE(statuses[0].unavailable->path.ends_with("/power/events/energy-pkg"));
  EXPECT_TRUE(rig.syscalls.perf_opens().empty());
}

// --- permission denied: the provider that failed ---------------------------------

TEST(PowerSourceTest, ADeniedEnergyFileIsTheFilePermissionRemedyAndNotCapPerfmon) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.deny_file(at(kZone0, "energy_uj"), EACCES);
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  ASSERT_TRUE(statuses[0].unavailable.has_value());
  const Unavailable& why = *statuses[0].unavailable;
  EXPECT_EQ(why.kind, Availability::kDenied);
  EXPECT_EQ(why.path, at(kZone0, "energy_uj"));
  EXPECT_TRUE(mentions(why.remediation, "CVE-2020-8694"));
  EXPECT_TRUE(mentions(why.remediation, "udev rule"));
  EXPECT_TRUE(mentions(why.remediation, at(kZone0, "energy_uj")));
  EXPECT_FALSE(mentions(why.remediation, "CAP_PERFMON"))
      << "CAP_PERFMON governs perf_event_open, not a 0400 node (PLAN F3)";
  EXPECT_FALSE(mentions(why.remediation, "perf_event_paranoid"));
  EXPECT_FALSE(statuses[1].unavailable.has_value()) << "the other package is its own fact";
  EXPECT_EQ(sample_once(rig).size(), 1U);
}

TEST(PowerSourceTest, ADeniedPackageIsServedByThePowerPmuWhenThatIsGranted) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    pmu_two(syscalls);
    syscalls.deny_file(at(kZone1, "energy_uj"), EACCES);
    syscalls.script_perf_reads(9, 2, {{platform::PerfCount{5ULL * 4294967296ULL, 10, 10}, 0}});
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_EQ(statuses[0].metric.provider, "powercap:intel-rapl");
  EXPECT_EQ(statuses[1].metric.provider, "perf:power");
  EXPECT_FALSE(statuses[1].metric.modulus.has_value()) << "64-bit: it wraps at 2^64";
  EXPECT_FALSE(statuses[1].unavailable.has_value());
  const auto& opens = rig.syscalls.perf_opens();
  ASSERT_EQ(opens.size(), 1U);
  EXPECT_EQ(opens[0].type, 9U);
  EXPECT_EQ(opens[0].config, 2U);
  EXPECT_EQ(opens[0].pid, -1) << "system-wide";
  EXPECT_EQ(opens[0].cpu, 4) << "the second package is the second CPU of the PMU's cpumask";
  EXPECT_FALSE(opens[0].user_only) << "the power PMU refuses the exclude bits (EINVAL)";
  EXPECT_FALSE(opens[0].inherit);
  const auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 2U);
  EXPECT_TRUE(same_reading(readings[0], Counter{123456789012}));
  // 5 * 2^32 counts of 2^-32 J: five joules, in microjoules.
  EXPECT_TRUE(same_reading(readings[1], Counter{5000000}));
}

TEST(PowerSourceTest, BothProvidersDeniedReportsBothRemediesLabelled) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    pmu_two(syscalls);
    syscalls.deny_file(at(kZone0, "energy_uj"), EACCES);
    syscalls.fail_every_perf_open(EPERM);
  }};
  const auto statuses = rig.probe();
  const Unavailable& why = *statuses[0].unavailable;
  EXPECT_EQ(why.kind, Availability::kDenied);
  EXPECT_TRUE(mentions(why.detail, "powercap: "));
  EXPECT_TRUE(mentions(why.detail, "; perf: "));
  EXPECT_TRUE(why.remediation.starts_with("powercap: "));
  EXPECT_TRUE(mentions(why.remediation, "; perf: "));
  EXPECT_TRUE(mentions(why.remediation, "udev rule"));
  EXPECT_TRUE(mentions(why.remediation, "CAP_PERFMON"));
}

TEST(PowerSourceTest, ADeniedProviderAndAnAbsentOneAdviseOnlyWhatCanWork) {
  // Powercap denied, and the PMU cannot even open the event: nothing to advise
  // for it, and no "perf:" noise in the remedy.
  Rig opens_refused{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    pmu_two(syscalls);
    syscalls.deny_file(at(kZone0, "energy_uj"), EACCES);
    syscalls.fail_every_perf_open(ENOENT);
  }};
  auto statuses = opens_refused.probe();
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_TRUE(mentions(statuses[0].unavailable->detail, "; perf: "));
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "udev rule"));
  EXPECT_FALSE(mentions(statuses[0].unavailable->remediation, "CAP_PERFMON"));
  EXPECT_FALSE(mentions(statuses[0].unavailable->remediation, "perf: "));

  // No power PMU on the machine at all.
  Rig no_pmu{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.deny_file(at(kZone0, "energy_uj"), EACCES);
  }};
  statuses = no_pmu.probe();
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_FALSE(mentions(statuses[0].unavailable->remediation, "CAP_PERFMON"));
}

TEST(PowerSourceTest, WithoutPowercapThePmuServesEveryPackageOfItsCpumask) {
  Rig rig{[](FakeSyscalls& syscalls) {
    pmu_two(syscalls);
    syscalls.script_perf_reads(9, 2, {{platform::PerfCount{4294967296ULL, 1, 1}, 0}});
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  for (std::uint32_t package = 0; package < 2; ++package) {
    EXPECT_EQ(statuses[package].metric.scope, Scope::kPackage);
    EXPECT_EQ(statuses[package].metric.scope_index, package);
    EXPECT_EQ(statuses[package].metric.provider, "perf:power");
    EXPECT_FALSE(statuses[package].unavailable.has_value());
  }
  ASSERT_EQ(rig.syscalls.perf_opens().size(), 2U);
  EXPECT_EQ(rig.syscalls.perf_opens()[0].cpu, 0);
  EXPECT_EQ(rig.syscalls.perf_opens()[1].cpu, 4);
  // Reset, then enable, on each.
  EXPECT_EQ(rig.syscalls.perf_controls().size(), 4U);
  const auto readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{1000000})) << "one joule";
}

TEST(PowerSourceTest, WithoutPowercapAPmuDeniedIsDeniedWithPerfsRemedyOnly) {
  Rig rig{[](FakeSyscalls& syscalls) {
    pmu_one(syscalls);
    syscalls.fail_every_perf_open(EACCES);
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 1U);
  const Unavailable& why = *statuses[0].unavailable;
  EXPECT_EQ(why.kind, Availability::kDenied);
  EXPECT_EQ(why.path, "perf event type 9 config 2");
  EXPECT_EQ(why.remediation, perf_remediation(Availability::kDenied, std::nullopt, why.path));
  EXPECT_TRUE(mentions(why.remediation, "CAP_PERFMON"));
  EXPECT_FALSE(mentions(why.remediation, "udev"));
}

TEST(PowerSourceTest, EveryErrnoOfPerfEventOpenIsClassifiedForThePowerPmuToo) {
  for (const auto& [number, kind] :
       {std::pair{EACCES, Availability::kDenied}, std::pair{EPERM, Availability::kDenied},
        std::pair{ENOENT, Availability::kAbsent}, std::pair{ENODEV, Availability::kAbsent},
        std::pair{EOPNOTSUPP, Availability::kAbsent}, std::pair{ENOSYS, Availability::kAbsent},
        std::pair{EINVAL, Availability::kAbsent}, std::pair{E2BIG, Availability::kUnreadable},
        std::pair{EBADF, Availability::kUnreadable}, std::pair{EBUSY, Availability::kUnreadable},
        std::pair{EFAULT, Availability::kUnreadable}, std::pair{EINTR, Availability::kUnreadable},
        std::pair{EMFILE, Availability::kUnreadable}, std::pair{ENOSPC, Availability::kUnreadable},
        std::pair{EOVERFLOW, Availability::kUnreadable},
        std::pair{ESRCH, Availability::kUnreadable}}) {
    Rig rig{[number = number](FakeSyscalls& syscalls) {
      pmu_one(syscalls);
      syscalls.fail_every_perf_open(number);
    }};
    const auto statuses = rig.probe();
    ASSERT_EQ(statuses.size(), 1U) << number;
    EXPECT_EQ(statuses[0].unavailable->kind, kind) << number;
  }
}

TEST(PowerSourceTest, ACounterThatCannotBeResetOrEnabledIsUnreadableAndClosed) {
  for (const auto action : {platform::PerfAction::kReset, platform::PerfAction::kEnable}) {
    Rig rig{[action](FakeSyscalls& syscalls) {
      pmu_one(syscalls);
      syscalls.fail_perf_control(action, EPERM);
    }};
    const auto statuses = rig.probe();
    ASSERT_EQ(statuses.size(), 1U);
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kUnreadable);
    EXPECT_TRUE(mentions(statuses[0].unavailable->detail, "ioctl"));
    EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
  }
}

TEST(PowerSourceTest, APmuReadThatFailsIsClassifiedByWhatTheKernelSaid) {
  Rig rig{[](FakeSyscalls& syscalls) {
    pmu_one(syscalls);
    syscalls.script_perf_reads(
        9, 2, {{platform::PerfCount{}, ENODEV}, {platform::PerfCount{4294967296ULL, 1, 1}, 0}});
  }};
  static_cast<void>(rig.probe());
  auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 1U);
  EXPECT_EQ(as_unavailable(readings[0]).kind, Availability::kVanished);
  EXPECT_EQ(as_unavailable(readings[0]).path, "perf event type 9 config 2");
  EXPECT_TRUE(mentions(as_unavailable(readings[0]).remediation, "went away"));
  readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{1000000})) << "and is read again when it returns";
}

TEST(PowerSourceTest, EveryDescriptorIsClosedWithTheSourceAndAReprobeClosesWhatItReplaces) {
  FakeSyscalls syscalls;
  syscalls.add_tree(kFixtures / "perf-two-packages", "/sys");
  platform::FileSystem filesystem{syscalls};
  {
    PowerSource source{syscalls, filesystem};
    static_cast<void>(source.probe());
    static_cast<void>(source.probe());
    EXPECT_EQ(syscalls.open_count() - syscalls.close_count(), 2)
        << "the first probe's two counters were closed by the second";
    EXPECT_FALSE(syscalls.all_descriptors_closed());
  }
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST(PowerSourceTest, MoreZonesThanTheCpumaskNamesLeaveTheSurplusToPowercapAlone) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    pmu_one(syscalls);
    syscalls.deny_file(at(kZone1, "energy_uj"), EACCES);
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kDenied);
  EXPECT_FALSE(mentions(statuses[1].unavailable->detail, "perf: ")) << "the PMU has no CPU for it";
  EXPECT_TRUE(rig.syscalls.perf_opens().empty());
}

// --- malformed -------------------------------------------------------------------

TEST(PowerSourceTest, AnEnergyThatIsNotANumberIsMalformedAndNothingIsGuessed) {
  Rig rig{
      [](FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "malformed-energy", "/sys"); }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kMalformed);
  EXPECT_EQ(statuses[0].unavailable->detail, "not a decimal count: '12 watts'");
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "decimal count"));
}

TEST(PowerSourceTest, EachWayTheCountersFilesCanBeWrongIsRefused) {
  struct Case {
    std::string range;
    std::string energy;
    std::string detail;
  };
  const std::vector<Case> cases = {
      {"0", "5", "a range of zero cannot hold a counter"},
      {"100", "100", "energy 100 is not below the range 100"},
      {"100", "101", "energy 101 is not below the range 100"},
      {"lots", "5", "not a decimal count: 'lots'"},
      {"100", "-5", "not a decimal count: '-5'"},
  };
  for (const Case& tested : cases) {
    Rig rig{[&](FakeSyscalls& syscalls) {
      syscalls.add_file(at(kZone0, "name"), "package-0\n");
      syscalls.add_file(at(kZone0, "max_energy_range_uj"), tested.range + "\n");
      syscalls.add_file(at(kZone0, "energy_uj"), tested.energy + "\n");
    }};
    const auto statuses = rig.probe();
    ASSERT_EQ(statuses.size(), 1U) << tested.detail;
    ASSERT_TRUE(statuses[0].unavailable.has_value()) << tested.detail;
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kMalformed) << tested.detail;
    EXPECT_EQ(statuses[0].unavailable->detail, tested.detail) << tested.detail;
  }
}

TEST(PowerSourceTest, AReadingAtOrAboveTheRangeIsRefusedWhereverItAppears) {
  Rig rig{powercap};
  static_cast<void>(rig.probe());
  rig.syscalls.add_file(at(kZone0, "energy_uj"), "262143328850\n");
  rig.syscalls.add_file(at(kZone1, "energy_uj"), "plenty\n");
  const auto readings = sample_once(rig);
  EXPECT_EQ(as_unavailable(readings[0]).kind, Availability::kMalformed);
  EXPECT_EQ(as_unavailable(readings[0]).detail,
            "energy 262143328850 is not below the range 262143328850");
  EXPECT_EQ(as_unavailable(readings[1]).kind, Availability::kMalformed);
  rig.syscalls.add_file(at(kZone0, "energy_uj"), "262143328849\n");
  EXPECT_TRUE(same_reading(sample_once(rig)[0], Counter{262143328849}))
      << "one below the range is the largest a reading can be";
}

TEST(PowerSourceTest, EachWayThePmuDescriptionCanBeWrongIsRefusedWithTheFileThatWasWrong) {
  struct Case {
    std::string file;
    std::string content;
    std::string detail;
  };
  const std::vector<Case> cases = {
      {"type", "nine", "not a decimal count: 'nine'"},
      {"type", "4294967305", "not a PMU type number: 4294967305"},
      {"events/energy-pkg", "config=0x2", "not an event=0x<hex> description: 'config=0x2'"},
      {"events/energy-pkg", "event=0x", "not an event=0x<hex> description: 'event=0x'"},
      {"events/energy-pkg", "event=0x2,umask=1",
       "not an event=0x<hex> description: 'event=0x2,umask=1'"},
      {"events/energy-pkg", "2", "not an event=0x<hex> description: '2'"},
      {"events/energy-pkg.scale", "a lot", "not a plausible joules-per-count scale: 'a lot'"},
      {"events/energy-pkg.scale", "2.3e-10 J",
       "not a plausible joules-per-count scale: '2.3e-10 J'"},
      {"events/energy-pkg.scale", "0", "not a plausible joules-per-count scale: '0'"},
      {"events/energy-pkg.scale", "-1e-10", "not a plausible joules-per-count scale: '-1e-10'"},
      {"events/energy-pkg.scale", "1", "not a plausible joules-per-count scale: '1'"},
      {"events/energy-pkg.scale", "nan", "not a plausible joules-per-count scale: 'nan'"},
      {"events/energy-pkg.scale", "inf", "not a plausible joules-per-count scale: 'inf'"},
      {"events/energy-pkg.scale", "5e-7", ""},  // the largest factor, 0.5, is accepted
      {"cpumask", "x", ""},
  };
  for (const Case& tested : cases) {
    Rig rig{[&](FakeSyscalls& syscalls) {
      pmu_one(syscalls);
      syscalls.add_file(at(kPmu, tested.file), tested.content + "\n");
    }};
    const auto statuses = rig.probe();
    ASSERT_EQ(statuses.size(), 1U) << tested.file << " " << tested.content;
    if (tested.detail.empty() && tested.file != "cpumask") {
      EXPECT_FALSE(statuses[0].unavailable.has_value()) << tested.content;
      continue;
    }
    ASSERT_TRUE(statuses[0].unavailable.has_value()) << tested.file << " " << tested.content;
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kMalformed) << tested.content;
    EXPECT_TRUE(statuses[0].unavailable->path.ends_with(tested.file)) << tested.content;
    if (!tested.detail.empty()) {
      EXPECT_EQ(statuses[0].unavailable->detail, tested.detail);
    }
  }
}

TEST(PowerSourceTest, AMalformedScaleFixtureIsRefused) {
  Rig rig{[](FakeSyscalls& syscalls) { syscalls.add_tree(kFixtures / "malformed-scale", "/sys"); }};
  const auto statuses = rig.probe();
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kMalformed);
  EXPECT_TRUE(rig.syscalls.perf_opens().empty())
      << "no counter is opened on a scale it cannot trust";
}

TEST(PowerSourceTest, APmuDescriptionFileThatCannotBeReadIsReportedWithItsKind) {
  for (const std::string file :
       {"type", "events/energy-pkg", "events/energy-pkg.scale", "cpumask"}) {
    Rig removed{[&](FakeSyscalls& syscalls) {
      pmu_one(syscalls);
      syscalls.remove_file(at(kPmu, file));
    }};
    auto statuses = removed.probe();
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent) << file;
    EXPECT_TRUE(statuses[0].unavailable->path.ends_with(file)) << file;

    Rig denied{[&](FakeSyscalls& syscalls) {
      pmu_one(syscalls);
      syscalls.deny_file(at(kPmu, file), EACCES);
    }};
    statuses = denied.probe();
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied) << file;
  }
}

TEST(PowerSourceTest, APmuThatIsThereButUnreadableIsAFailureOfThePerfProviderNextToPowercaps) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.add_tree(kFixtures / "malformed-scale", "/sys");
    syscalls.deny_file(at(kZone0, "energy_uj"), EACCES);
  }};
  const auto statuses = rig.probe();
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_TRUE(mentions(statuses[0].unavailable->detail, "; perf: "));
  EXPECT_TRUE(mentions(statuses[0].unavailable->detail, "joules-per-count"));
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "udev rule"));
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "powercap: "));
}

TEST(PowerSourceTest, ACpumaskThatNamesNoCpuIsAbsent) {
  Rig rig{[](FakeSyscalls& syscalls) {
    pmu_one(syscalls);
    syscalls.add_file(at(kPmu, "cpumask"), "\n");
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 1U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(statuses[0].unavailable->detail, "the power PMU names no CPU to read it from");
}

// --- the other failures of a read ------------------------------------------------

TEST(PowerSourceTest, AnUnreadableNameOrRangeIsReportedWithItsKind) {
  Rig denied_name{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.deny_file(at(kZone0, "name"), EACCES);
  }};
  auto statuses = denied_name.probe();
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied);
  EXPECT_EQ(statuses[0].unavailable->path, at(kZone0, "name"));
  EXPECT_EQ(statuses.size(), 2U) << "the next zone is still found";

  Rig broken_range{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.fail_file_read(at(kZone1, "max_energy_range_uj"), EIO);
  }};
  statuses = broken_range.probe();
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kUnreadable);
  EXPECT_TRUE(mentions(statuses[1].unavailable->remediation, "dmesg"));

  Rig no_range{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.remove_file(at(kZone1, "max_energy_range_uj"));
  }};
  statuses = no_range.probe();
  EXPECT_EQ(statuses[1].unavailable->kind, Availability::kAbsent);
}

TEST(PowerSourceTest, TwoAbsentProvidersAreOneAbsentStatusWithBothDetailsAndOneStatement) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    pmu_two(syscalls);
    syscalls.remove_file(at(kZone0, "max_energy_range_uj"));
    syscalls.fail_every_perf_open(ENOENT);
  }};
  const auto statuses = rig.probe();
  const Unavailable& why = *statuses[0].unavailable;
  EXPECT_EQ(why.kind, Availability::kAbsent);
  EXPECT_TRUE(mentions(why.detail, "powercap: "));
  EXPECT_TRUE(mentions(why.detail, "; perf: "));
  EXPECT_EQ(why.remediation, powercap_remediation(Availability::kAbsent, why.path));
}

TEST(PowerSourceTest, AnUnreadablePowercapAndADeniedPmuAreDeniedWithBothRemedies) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    pmu_two(syscalls);
    syscalls.fail_file_read(at(kZone0, "energy_uj"), EIO);
    syscalls.fail_every_perf_open(EACCES);
  }};
  const auto statuses = rig.probe();
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kDenied)
      << "the failure that can be acted on outranks the one that cannot";
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "dmesg"));
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "CAP_PERFMON"));
}

struct PowercapFault {
  void (*mutate)(FakeSyscalls&);
  Availability kind;
};

void junk_energy(FakeSyscalls& s) { s.add_file(at(kZone0, "energy_uj"), "junk\n"); }
void broken_energy(FakeSyscalls& s) { s.fail_file_read(at(kZone0, "energy_uj"), EIO); }
void missing_energy(FakeSyscalls& s) { s.remove_file(at(kZone0, "energy_uj")); }

const PowercapFault kFaults[] = {{junk_energy, Availability::kMalformed},
                                 {broken_energy, Availability::kUnreadable},
                                 {missing_energy, Availability::kAbsent}};

TEST(PowerSourceTest, EachKindOfFailureOutranksTheNextInTheOrderTheyCanBeActedOn) {
  // Two failures at once, one by powercap and one by the PMU; the kind reported
  // is the more actionable.
  const std::vector<std::pair<int, Availability>> pmu_errors = {{EACCES, Availability::kDenied},
                                                                {EBUSY, Availability::kUnreadable},
                                                                {ENOENT, Availability::kAbsent}};
  for (const auto& [error, pmu_kind] : pmu_errors) {
    for (const PowercapFault& fault : kFaults) {
      Rig rig{[&, error = error](FakeSyscalls& syscalls) {
        powercap(syscalls);
        pmu_two(syscalls);
        fault.mutate(syscalls);
        syscalls.fail_every_perf_open(error);
      }};
      const Availability reported = rig.probe()[0].unavailable->kind;
      const auto rank = [](Availability kind) {
        switch (kind) {
          case Availability::kDenied:
            return 3;
          case Availability::kMalformed:
            return 2;
          case Availability::kUnreadable:
            return 1;
          default:
            return 0;
        }
      };
      const Availability expected = rank(pmu_kind) > rank(fault.kind) ? pmu_kind : fault.kind;
      EXPECT_EQ(reported, expected)
          << "pmu " << static_cast<int>(pmu_kind) << " powercap " << static_cast<int>(fault.kind);
    }
  }
}

// --- disappears mid-run ----------------------------------------------------------

TEST(PowerSourceTest, AnEnergyFileThatGoesAwayIsVanishedAndIsReadAgainWhenItReturns) {
  Rig rig{powercap};
  static_cast<void>(rig.probe());
  rig.syscalls.remove_file(at(kZone0, "energy_uj"));
  auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 2U);
  EXPECT_EQ(as_unavailable(readings[0]).kind, Availability::kVanished);
  EXPECT_TRUE(mentions(as_unavailable(readings[0]).remediation, "went away during the run"));
  EXPECT_TRUE(same_reading(readings[1], Counter{98765432100}));
  rig.syscalls.add_file(at(kZone0, "energy_uj"), "42\n");
  readings = sample_once(rig);
  EXPECT_TRUE(same_reading(readings[0], Counter{42}));
}

TEST(PowerSourceTest, AReadRefusedByTheDriverAfterAnOpenIsDeniedAtSampleToo) {
  Rig rig{powercap};
  static_cast<void>(rig.probe());
  rig.syscalls.fail_file_read(at(kZone0, "energy_uj"), EACCES);
  const auto readings = sample_once(rig);
  EXPECT_EQ(as_unavailable(readings[0]).kind, Availability::kDenied);
  EXPECT_TRUE(mentions(as_unavailable(readings[0]).remediation, "udev rule"));
}

// --- the counter wraps -----------------------------------------------------------

TEST(PowerSourceTest, AnEnergyCounterThatWrapsAtItsRangeHasADeltaAcrossTheWrap) {
  Rig rig{[](FakeSyscalls& syscalls) {
    powercap(syscalls);
    syscalls.add_file(at(kZone0, "energy_uj"), "262143328000\n");
  }};
  const auto statuses = rig.probe();
  const auto before = std::get<Counter>(sample_once(rig)[0]);
  rig.syscalls.add_file(at(kZone0, "energy_uj"), "500\n");
  const auto after = std::get<Counter>(sample_once(rig)[0]);
  // 850 to the top of the range, and 500 past it.
  EXPECT_EQ(telemetry::counter_delta(before.raw, after.raw, statuses[0].metric.modulus), 1350U);
}

// --- the remedies ----------------------------------------------------------------

TEST(PowercapRemediationTest, EveryStateHasItsOwnAdviceAndAStateThatIsNotOneGetsTheGenericOne) {
  const std::string path = "/sys/class/powercap/intel-rapl:0/energy_uj";
  std::vector<std::string> advice;
  for (const Availability kind :
       {Availability::kAbsent, Availability::kVanished, Availability::kDenied,
        Availability::kMalformed, Availability::kUnreadable}) {
    advice.push_back(powercap_remediation(kind, path));
  }
  for (std::size_t i = 0; i < advice.size(); ++i) {
    for (std::size_t j = i + 1; j < advice.size(); ++j) {
      EXPECT_NE(advice[i], advice[j]);
    }
  }
  EXPECT_FALSE(mentions(advice[0], path));
  EXPECT_TRUE(mentions(advice[2], path));
  EXPECT_TRUE(mentions(advice[3], path));
  EXPECT_TRUE(mentions(advice[4], path));
  EXPECT_EQ(powercap_remediation(Availability::kPresent, path), advice[4]);
  EXPECT_EQ(powercap_remediation(static_cast<Availability>(99), path), advice[4]);
}

// --- against the real machine ----------------------------------------------------

TEST(PowerSourceRealMachineTest, TheRealKernelAgreesWithWhatTheSourceReports) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  PowerSource source{syscalls, filesystem};
  const auto statuses = source.probe();
  ASSERT_FALSE(statuses.empty());
  const bool has_powercap = std::filesystem::exists("/sys/class/powercap/intel-rapl:0/name");
  if (!has_powercap && !std::filesystem::exists("/sys/bus/event_source/devices/power/type")) {
    ASSERT_EQ(statuses.size(), 1U);
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
  }
  for (const MetricStatus& status : statuses) {
    if (status.unavailable.has_value()) {
      EXPECT_FALSE(status.unavailable->remediation.empty());
    }
  }
  std::vector<Reading> readings;
  source.sample(readings);
  std::size_t present = 0;
  for (const MetricStatus& status : statuses) {
    present += status.unavailable.has_value() ? 0U : 1U;
  }
  EXPECT_EQ(readings.size(), present);
}

}  // namespace
}  // namespace loadforge::contracts
