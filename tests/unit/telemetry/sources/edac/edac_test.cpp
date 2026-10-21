// SPDX-License-Identifier: GPL-3.0-or-later
//
// The EDAC source (PLAN F13) under the six states of IMPLEMENTATION M3.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Two states cannot be committed and are built here. A counter that wraps and
// a controller that disappears mid-run are events, not states: only a test
// that changes the files between two samples produces them. And a file the
// caller may not read arrives from a clone readable, because git records the
// execute bit and nothing else, and CI runs as root, which no mode refuses --
// so denial is the fake's refusal of the open, over the committed tree's files.
// The present, absent and malformed states are committed trees, read through
// the real system calls.

#include "telemetry/sources/edac/edac.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "contracts/telemetry/source_contract.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"

// In the contracts' namespace, as the contract's own reference tests are: the
// instantiation macros name the suite unqualified.
namespace loadforge::contracts {
namespace {

using loadforge::testing::FakeSyscalls;
using telemetry::Counter;
using telemetry::edac_remediation;
using telemetry::EdacSource;
using telemetry::Kind;
using telemetry::MetricStatus;
using telemetry::Reading;
using telemetry::same_reading;
using telemetry::Scope;
using telemetry::Source;
using telemetry::Unavailable;
using telemetry::Unit;
using topology::Availability;

// The contract header registers the timeline suite too; this module has no
// timeline, and a binary that instantiates only one of the two would fail.
const std::filesystem::path kFixtures =
    std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" / "sysfs" / "edac";

constexpr std::string_view kMc0Ce = "/sys/devices/system/edac/mc/mc0/ce_count";
constexpr std::string_view kMc0Ue = "/sys/devices/system/edac/mc/mc0/ue_count";
constexpr std::string_view kMc1Ce = "/sys/devices/system/edac/mc/mc1/ce_count";

/// A source over a fake kernel, holding the fake it reads. `between` runs
/// before each sample with the round number: the test's hand on the machine.
class FakeRig final : public Source {
 public:
  using Prepare = std::function<void(FakeSyscalls&)>;
  using Between = std::function<void(int, FakeSyscalls&)>;

  explicit FakeRig(const Prepare& prepare, Between between = nullptr)
      : between_(std::move(between)) {
    prepare(syscalls);
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
  EdacSource source{filesystem};

 private:
  Between between_;
  int round_ = 0;
};

/// A source over the real system calls and a committed tree.
class RealRig final : public Source {
 public:
  explicit RealRig(const std::string& tree)
      : root_((kFixtures / tree).string()), source(filesystem, root_) {}
  [[nodiscard]] std::string_view name() const noexcept override { return source.name(); }
  [[nodiscard]] std::vector<MetricStatus> probe() override { return source.probe(); }
  void sample(std::vector<Reading>& out) override { source.sample(out); }

  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

 private:
  std::string root_;

 public:
  EdacSource source;
};

void with_two_controllers(FakeSyscalls& syscalls) {
  syscalls.add_tree(kFixtures / "two-controllers", "/sys");
}

// --- the contract, once per state ---------------------------------------------

struct PresentSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<RealRig>("two-controllers"); }
};
struct AbsentSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<RealRig>("absent"); }
};
struct MalformedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() { return std::make_unique<RealRig>("malformed"); }
};
struct DeniedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<FakeRig>([](FakeSyscalls& syscalls) {
      with_two_controllers(syscalls);
      syscalls.deny_file(std::string{kMc0Ce}, EACCES);
    });
  }
};
struct VanishedSubject {
  static constexpr int kRounds = 4;
  static std::unique_ptr<Source> make() {
    return std::make_unique<FakeRig>(with_two_controllers, [](int round, FakeSyscalls& syscalls) {
      if (round == 1) {
        syscalls.remove_file(std::string{kMc0Ce});
      }
      if (round == 3) {
        syscalls.add_file(std::string{kMc0Ce}, "9\n");
      }
    });
  }
};
struct WrappedSubject {
  static constexpr int kRounds = 3;
  static std::unique_ptr<Source> make() {
    return std::make_unique<FakeRig>(
        [](FakeSyscalls& syscalls) {
          with_two_controllers(syscalls);
          syscalls.add_file(std::string{kMc0Ce}, "4294967290\n");
        },
        [](int round, FakeSyscalls& syscalls) {
          if (round == 1) {
            syscalls.add_file(std::string{kMc0Ce}, "5\n");
          }
        });
  }
};

using States = ::testing::Types<PresentSubject, AbsentSubject, MalformedSubject, DeniedSubject,
                                VanishedSubject, WrappedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(EdacStates, SourceContract, States);

// --- helpers --------------------------------------------------------------------

const MetricStatus* find_status(const std::vector<MetricStatus>& statuses, std::string_view name,
                                std::uint32_t index) {
  for (const MetricStatus& status : statuses) {
    if (status.metric.name == name && status.metric.scope_index == index) {
      return &status;
    }
  }
  return nullptr;
}

const Unavailable& as_unavailable(const Reading& reading) { return std::get<Unavailable>(reading); }

std::vector<Reading> sample_once(Source& source) {
  std::vector<Reading> readings;
  source.sample(readings);
  return readings;
}

bool mentions(const std::string& text, std::string_view word) {
  return text.find(word) != std::string::npos;
}

// --- present --------------------------------------------------------------------

TEST(EdacSourceTest, EachControllerReportsItsCorrectedAndUncorrectedCounts) {
  RealRig rig{"two-controllers"};
  EXPECT_EQ(rig.name(), "edac");
  const std::vector<MetricStatus> statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 4U);
  for (std::uint32_t controller = 0; controller < 2; ++controller) {
    for (const std::string_view name : {"edac.corrected", "edac.uncorrected"}) {
      const MetricStatus* status = find_status(statuses, name, controller);
      ASSERT_NE(status, nullptr) << name << " mc" << controller;
      EXPECT_FALSE(status->unavailable.has_value());
      EXPECT_EQ(status->metric.unit, Unit::kCount);
      EXPECT_EQ(status->metric.kind, Kind::kCounter);
      EXPECT_EQ(status->metric.scope, Scope::kMemoryController);
      EXPECT_EQ(status->metric.modulus, std::uint64_t{1} << 32U);
      EXPECT_EQ(status->metric.provider, "edac:sysfs");
    }
  }
  // Probe order is the sample order: mc0 corrected, mc0 uncorrected, mc1 ...
  EXPECT_EQ(statuses[0].metric.name, "edac.corrected");
  EXPECT_EQ(statuses[1].metric.name, "edac.uncorrected");
  EXPECT_EQ(statuses[2].metric.scope_index, 1U);

  const std::vector<Reading> readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 4U);
  EXPECT_TRUE(same_reading(readings[0], Counter{3}));
  EXPECT_TRUE(same_reading(readings[1], Counter{0}));
  EXPECT_TRUE(same_reading(readings[2], Counter{0}));
  EXPECT_TRUE(same_reading(readings[3], Counter{0}));
}

TEST(EdacSourceTest, ThePrefixOfTheNameIsTheSourcesFamilyAndTheVerdictsNamesAreThose) {
  RealRig rig{"two-controllers"};
  for (const MetricStatus& status : rig.probe()) {
    EXPECT_TRUE(status.metric.name == "edac.corrected" || status.metric.name == "edac.uncorrected");
  }
}

TEST(EdacSourceTest, NothingIsReadBeforeTheProbeAndAReprobeDoesNotDoubleTheChannels) {
  FakeRig rig{with_two_controllers};
  EXPECT_TRUE(sample_once(rig).empty());
  const auto first = rig.probe();
  const auto second = rig.probe();
  EXPECT_EQ(first, second);
  EXPECT_EQ(sample_once(rig).size(), 4U);
}

TEST(EdacSourceTest, EveryDescriptorIsClosed) {
  FakeRig rig{with_two_controllers};
  static_cast<void>(rig.probe());
  static_cast<void>(sample_once(rig));
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(EdacSourceTest, ControllersAreFoundByNumberUntilTheFirstWithoutACorrectedCount) {
  FakeRig rig{[](FakeSyscalls& syscalls) {
    for (const char* controller : {"mc0", "mc1", "mc2"}) {
      const std::string base = std::string{"/sys/devices/system/edac/mc/"} + controller;
      syscalls.add_file(base + "/ce_count", "1\n");
      syscalls.add_file(base + "/ue_count", "0\n");
    }
    // mc4 without an mc3: the list has ended at the gap.
    syscalls.add_file("/sys/devices/system/edac/mc/mc4/ce_count", "7\n");
  }};
  EXPECT_EQ(rig.probe().size(), 6U);
}

// --- absent ---------------------------------------------------------------------

TEST(EdacSourceTest, ThisMachineHasNoEdacAndSaysSoOncePerMetricWithAdviceThatCanWork) {
  RealRig rig{"absent"};
  const std::vector<MetricStatus> statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_EQ(statuses[0].metric.name, "edac.corrected");
  EXPECT_EQ(statuses[1].metric.name, "edac.uncorrected");
  for (const MetricStatus& status : statuses) {
    ASSERT_TRUE(status.unavailable.has_value());
    const Unavailable& why = *status.unavailable;
    EXPECT_EQ(why.kind, Availability::kAbsent) << "unsupported, never permission-denied (F3)";
    EXPECT_EQ(status.metric.scope, Scope::kMachine);
    EXPECT_TRUE(why.path.ends_with("/devices/system/edac/mc"));
    EXPECT_EQ(why.detail, "no memory controller is registered with EDAC");
    // A fix that cannot work is worse than saying nothing.
    for (const std::string_view advice :
         {"privileg", "permission", "root", "CAP_", "chmod", "sudo"}) {
      EXPECT_FALSE(mentions(why.remediation, advice)) << why.remediation;
    }
    EXPECT_TRUE(mentions(why.remediation, "nothing can be changed"));
  }
  EXPECT_TRUE(sample_once(rig).empty());
}

TEST(EdacSourceTest, AMachineWithNoSysAtAllIsAlsoJustAbsent) {
  FakeRig rig{[](FakeSyscalls& syscalls) { syscalls.add_file("/elsewhere", "x"); }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kAbsent);
}

TEST(EdacSourceTest, AMissingUncorrectedCountIsReportedAbsentAndTheNextControllerIsStillFound) {
  FakeRig rig{[](FakeSyscalls& syscalls) {
    with_two_controllers(syscalls);
    syscalls.remove_file(std::string{kMc0Ue});
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 4U);
  const MetricStatus* lost = find_status(statuses, "edac.uncorrected", 0);
  ASSERT_NE(lost, nullptr);
  ASSERT_TRUE(lost->unavailable.has_value());
  EXPECT_EQ(lost->unavailable->kind, Availability::kAbsent);
  EXPECT_EQ(lost->metric.scope, Scope::kMemoryController);
  EXPECT_FALSE(find_status(statuses, "edac.uncorrected", 1)->unavailable.has_value());
  EXPECT_EQ(sample_once(rig).size(), 3U) << "only the present metrics are read";
}

// --- permission denied ----------------------------------------------------------

TEST(EdacSourceTest, ADeniedCountIsDeniedWithItsPathAndAnAdviceAboutReadAccess) {
  FakeRig rig{[](FakeSyscalls& syscalls) {
    with_two_controllers(syscalls);
    syscalls.deny_file(std::string{kMc0Ce}, EACCES);
  }};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 4U) << "a denial does not end the list of controllers";
  const MetricStatus* denied = find_status(statuses, "edac.corrected", 0);
  ASSERT_NE(denied, nullptr);
  ASSERT_TRUE(denied->unavailable.has_value());
  EXPECT_EQ(denied->unavailable->kind, Availability::kDenied);
  EXPECT_EQ(denied->unavailable->path, kMc0Ce);
  EXPECT_TRUE(mentions(denied->unavailable->remediation, std::string{kMc0Ce}));
  EXPECT_TRUE(mentions(denied->unavailable->remediation, "read access"));
  EXPECT_FALSE(mentions(denied->unavailable->remediation, "CAP_PERFMON"))
      << "a different provider's remedy";
  EXPECT_FALSE(find_status(statuses, "edac.uncorrected", 0)->unavailable.has_value());
}

TEST(EdacSourceTest, AReadRefusedByTheDriverAfterASuccessfulOpenIsDeniedToo) {
  FakeRig rig{with_two_controllers};
  static_cast<void>(rig.probe());
  rig.syscalls.fail_file_read(std::string{kMc0Ce}, EACCES);
  const auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 4U);
  EXPECT_EQ(as_unavailable(readings[0]).kind, Availability::kDenied);
  EXPECT_TRUE(same_reading(readings[1], Counter{0}));
}

// --- malformed ------------------------------------------------------------------

TEST(EdacSourceTest, ACountThatIsNotANumberIsMalformedAndNothingIsGuessed) {
  RealRig rig{"malformed"};
  const auto statuses = rig.probe();
  ASSERT_EQ(statuses.size(), 2U);
  EXPECT_EQ(statuses[0].unavailable->kind, Availability::kMalformed);
  EXPECT_EQ(statuses[0].unavailable->detail, "not a decimal count: 'three'");
  EXPECT_TRUE(mentions(statuses[0].unavailable->remediation, "decimal count"));
  EXPECT_FALSE(statuses[1].unavailable.has_value()) << "the neighbouring file is its own fact";
  const auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 1U);
  EXPECT_TRUE(same_reading(readings[0], Counter{0}));
}

TEST(EdacSourceTest, EachWayOfNotBeingACountIsRefusedAtProbeAndAtSample) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"3x", "not a decimal count: '3x'"},
      {"-1", "not a decimal count: '-1'"},
      {"", "not a decimal count: ''"},
      {"1.5", "not a decimal count: '1.5'"},
      {"4294967296", "exceeds the 32-bit count the kernel keeps: 4294967296"},
      {"99999999999999999999", "not a decimal count: '99999999999999999999'"},
  };
  for (const auto& [content, detail] : cases) {
    FakeRig at_probe{[&](FakeSyscalls& syscalls) {
      with_two_controllers(syscalls);
      syscalls.add_file(std::string{kMc0Ce}, content + "\n");
    }};
    const auto statuses = at_probe.probe();
    ASSERT_TRUE(statuses[0].unavailable.has_value()) << content;
    EXPECT_EQ(statuses[0].unavailable->kind, Availability::kMalformed) << content;
    EXPECT_EQ(statuses[0].unavailable->detail, detail) << content;

    FakeRig at_sample{with_two_controllers};
    static_cast<void>(at_sample.probe());
    at_sample.syscalls.add_file(std::string{kMc0Ce}, content + "\n");
    const auto readings = sample_once(at_sample);
    EXPECT_EQ(as_unavailable(readings[0]).kind, Availability::kMalformed) << content;
    EXPECT_EQ(as_unavailable(readings[0]).detail, detail) << content;
  }
}

TEST(EdacSourceTest, TheLargestCountThe32BitCounterHoldsIsAccepted) {
  FakeRig rig{[](FakeSyscalls& syscalls) {
    with_two_controllers(syscalls);
    syscalls.add_file(std::string{kMc0Ce}, "  4294967295 \n");
  }};
  const auto statuses = rig.probe();
  EXPECT_FALSE(statuses[0].unavailable.has_value());
  EXPECT_TRUE(same_reading(sample_once(rig)[0], Counter{4294967295U}));
}

// --- disappears mid-run ---------------------------------------------------------

TEST(EdacSourceTest, ACountThatGoesAwayIsVanishedNotAbsentAndComesBackWhenItDoes) {
  FakeRig rig{with_two_controllers};
  static_cast<void>(rig.probe());
  rig.syscalls.remove_file(std::string{kMc0Ce});
  auto readings = sample_once(rig);
  ASSERT_EQ(readings.size(), 4U);
  const Unavailable& gone = as_unavailable(readings[0]);
  EXPECT_EQ(gone.kind, Availability::kVanished);
  EXPECT_EQ(gone.path, kMc0Ce);
  EXPECT_TRUE(mentions(gone.remediation, "went away during the run"));
  EXPECT_TRUE(same_reading(readings[1], Counter{0}));

  rig.syscalls.add_file(std::string{kMc0Ce}, "4\n");
  readings.clear();
  rig.sample(readings);
  EXPECT_TRUE(same_reading(readings[0], Counter{4}));
}

TEST(EdacSourceTest, ADriverThatFailsTheReadIsUnreadableAtProbeAndAtSample) {
  FakeRig at_probe{[](FakeSyscalls& syscalls) {
    with_two_controllers(syscalls);
    syscalls.fail_file_read(std::string{kMc1Ce}, EIO);
  }};
  const auto statuses = at_probe.probe();
  const MetricStatus* broken = find_status(statuses, "edac.corrected", 1);
  ASSERT_TRUE(broken->unavailable.has_value());
  EXPECT_EQ(broken->unavailable->kind, Availability::kUnreadable);
  EXPECT_TRUE(mentions(broken->unavailable->remediation, "dmesg"));

  FakeRig at_sample{with_two_controllers};
  static_cast<void>(at_sample.probe());
  at_sample.syscalls.fail_file_read(std::string{kMc1Ce}, EIO);
  const auto readings = sample_once(at_sample);
  EXPECT_EQ(as_unavailable(readings[2]).kind, Availability::kUnreadable);
}

// --- counter wraps --------------------------------------------------------------

TEST(EdacSourceTest, ACounterThatWrapsAtItsModulusHasADeltaAcrossTheWrap) {
  FakeRig rig{[](FakeSyscalls& syscalls) {
                with_two_controllers(syscalls);
                syscalls.add_file(std::string{kMc0Ce}, "4294967290\n");
              },
              [](int round, FakeSyscalls& syscalls) {
                if (round == 1) {
                  syscalls.add_file(std::string{kMc0Ce}, "5\n");
                }
              }};
  const auto statuses = rig.probe();
  const auto modulus = statuses[0].metric.modulus;
  ASSERT_TRUE(modulus.has_value());
  const auto before = std::get<Counter>(sample_once(rig)[0]);
  const auto after = std::get<Counter>(sample_once(rig)[0]);
  EXPECT_EQ(before.raw, 4294967290U);
  EXPECT_EQ(after.raw, 5U);
  EXPECT_EQ(telemetry::counter_delta(before.raw, after.raw, modulus), 11U);
}

// --- the remedies, one per state ------------------------------------------------

TEST(EdacRemediationTest, EveryStateHasItsOwnAdvice) {
  const std::string path = "/sys/devices/system/edac/mc/mc0/ce_count";
  std::vector<std::string> advice;
  for (const Availability kind :
       {Availability::kAbsent, Availability::kVanished, Availability::kDenied,
        Availability::kMalformed, Availability::kUnreadable}) {
    advice.push_back(edac_remediation(kind, path));
    EXPECT_FALSE(advice.back().empty());
  }
  for (std::size_t i = 0; i < advice.size(); ++i) {
    for (std::size_t j = i + 1; j < advice.size(); ++j) {
      EXPECT_NE(advice[i], advice[j]);
    }
  }
  EXPECT_FALSE(mentions(advice[0], path)) << "an absent controller has no path to fix";
  EXPECT_TRUE(mentions(advice[2], path));
  EXPECT_TRUE(mentions(advice[3], path));
  EXPECT_TRUE(mentions(advice[4], path));
}

TEST(EdacRemediationTest, ASourceNeverReportsAStateThatIsNotOne) {
  // kPresent is not a way of being unavailable; asked anyway, the answer is the
  // generic one, and so is a value outside the enumeration.
  const std::string generic = edac_remediation(Availability::kUnreadable, "/p");
  EXPECT_EQ(edac_remediation(Availability::kPresent, "/p"), generic);
  EXPECT_EQ(edac_remediation(static_cast<Availability>(99), "/p"), generic);
}

// --- against the real machine ---------------------------------------------------

TEST(EdacSourceRealMachineTest, TheRealSysAgreesWithWhatTheKernelOffers) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  EdacSource source{filesystem};
  const auto statuses = source.probe();
  ASSERT_FALSE(statuses.empty());
  if (std::filesystem::exists("/sys/devices/system/edac/mc/mc0/ce_count")) {
    EXPECT_EQ(statuses[0].metric.scope, Scope::kMemoryController);
    EXPECT_NE(statuses[0].unavailable.has_value() ? statuses[0].unavailable->kind
                                                  : Availability::kPresent,
              Availability::kAbsent);
  } else {
    for (const MetricStatus& status : statuses) {
      ASSERT_TRUE(status.unavailable.has_value());
      EXPECT_EQ(status.unavailable->kind, Availability::kAbsent);
    }
  }
}

}  // namespace
}  // namespace loadforge::contracts
