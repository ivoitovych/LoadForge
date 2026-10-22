// SPDX-License-Identifier: GPL-3.0-or-later
//
// The roster a registry gives a schedule, and ADR 0003's cross-execution: the
// registry contract's suite run against this module's own reading of the
// registry (stub_registry.hpp).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "contracts/workloads/registry_contract.hpp"
#include "unit/workloads/mixed/stub_registry.hpp"
#include "workloads/mixed/mixed.hpp"

namespace loadforge::contracts {
namespace {

using mixed_testing::StubEntry;
using mixed_testing::StubRegistry;

struct StubSubject {
  static std::unique_ptr<workloads::Registry> make() {
    return std::make_unique<StubRegistry>(std::vector<StubEntry>{
        {"fft", 100'000, true}, {"compression", 1'000'000, true}, {"sort", 2'500'000, true}});
  }
  static workloads::WorkloadConfig config(const std::string& /*name*/) {
    return workloads::WorkloadConfig{1, std::uint64_t{8} << 20U, 0, simd::IsaPath::kScalar};
  }
};
INSTANTIATE_TYPED_TEST_SUITE_P(MixedModel, RegistryContract, ::testing::Types<StubSubject>);

}  // namespace
}  // namespace loadforge::contracts

namespace loadforge::mixed {
namespace {

using mixed_testing::StubRegistry;

TEST(Roster, EveryWorkloadWithTheLeastFootprintItIsMadeAtInDoublingsFromSixtyFourKiB) {
  const StubRegistry registry({{"fft", 100'000, true},
                               {"compression", 1'000'000, true},
                               {"sort", 2'500'000, true},
                               {"tiny", 1, true}});
  const std::vector<RosterEntry> roster = roster_of(registry, std::uint64_t{16} << 20U);
  ASSERT_EQ(roster.size(), 4U);
  // In the registry's order, which is sorted.
  EXPECT_EQ(roster[0], (RosterEntry{"compression", std::uint64_t{1} << 20U}));
  EXPECT_EQ(roster[1], (RosterEntry{"fft", std::uint64_t{128} << 10U}));
  EXPECT_EQ(roster[2], (RosterEntry{"sort", std::uint64_t{4} << 20U}));
  EXPECT_EQ(roster[3], (RosterEntry{"tiny", std::uint64_t{64} << 10U}));
}

TEST(Roster, AWorkloadThatCannotBeMadeAtOrBelowTheCeilingIsLeftOut) {
  const StubRegistry registry(
      {{"big", 9'000'000, true}, {"never", 1, false}, {"fits", 70'000, true}});
  const std::vector<RosterEntry> roster = roster_of(registry, std::uint64_t{4} << 20U);
  ASSERT_EQ(roster.size(), 1U);
  EXPECT_EQ(roster[0].name, "fits");
  // The ceiling itself is tried: exactly one doubling that fits it.
  const std::vector<RosterEntry> exact = roster_of(registry, std::uint64_t{8} << 20U);
  EXPECT_EQ(exact.size(), 1U) << "9 MB does not fit in 8 MiB";
  EXPECT_EQ(roster_of(registry, std::uint64_t{16} << 20U).size(), 2U);
}

TEST(Roster, ACeilingBelowTheSmallestTryIsAnEmptyRoster) {
  const StubRegistry registry({{"fits", 1, true}});
  EXPECT_TRUE(roster_of(registry, (std::uint64_t{64} << 10U) - 1).empty());
  EXPECT_EQ(roster_of(registry, std::uint64_t{64} << 10U).size(), 1U);
}

TEST(Roster, EntriesCompareByBothFields) {
  EXPECT_EQ((RosterEntry{"a", 1}), (RosterEntry{"a", 1}));
  EXPECT_NE((RosterEntry{"a", 1}), (RosterEntry{"b", 1}));
  EXPECT_NE((RosterEntry{"a", 1}), (RosterEntry{"a", 2}));
}

TEST(Roster, EachWorkloadIsMadeOnceForEveryDoublingUntilItFits) {
  const StubRegistry registry({{"only", 300'000, true}});
  (void)roster_of(registry, std::uint64_t{1} << 30U);
  EXPECT_EQ(registry.creations, 4) << "64 KiB, 128 KiB and 256 KiB refused, 512 KiB made";
}

}  // namespace
}  // namespace loadforge::mixed
