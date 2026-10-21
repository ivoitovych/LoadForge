// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recording a golden vector from a workload, and comparing a recorded vector
// with a fresh one: every verdict, every kind of change named, and the report
// kept apart from the error records -- a golden mismatch is a regression
// report, never a hardware verdict (verification.md §4).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "contracts/equality.hpp"
#include "unit/verification/golden/mock_workload.hpp"
#include "verification/checksum/crc64.hpp"
#include "verification/golden/golden.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

using golden_test::Flavor;
using golden_test::MockWorkload;
using golden_test::SerialExecutor;

static_assert(!std::is_convertible_v<GoldenReport, ErrorRecord> &&
                  !std::is_constructible_v<ErrorRecord, GoldenReport>,
              "a golden report can never become an error record");
static_assert(!std::is_constructible_v<ErrorRecord, GoldenVerdict>);

std::unique_ptr<workloads::Workload> make(const workloads::WorkloadConfig& config,
                                          std::uint32_t version = 1,
                                          Flavor flavor = Flavor::kOriginal) {
  auto created = MockWorkload::create(config, version, flavor);
  EXPECT_TRUE(created.has_value());
  return std::move(created).value();
}

GoldenVector recorded(std::uint32_t version = 1, Flavor flavor = Flavor::kOriginal,
                      std::uint64_t units = 3) {
  const workloads::WorkloadConfig config = golden_test::fixture_config();
  SerialExecutor serial;
  auto vector = record_golden(*make(config, version, flavor), config, units, serial);
  EXPECT_TRUE(vector.has_value());
  return vector.value();
}

TEST(GoldenRecord, DigestsEachUnitsInputAndOutputAndAllOutputsInOrder) {
  const workloads::WorkloadConfig config = golden_test::fixture_config();
  const auto workload = make(config);
  const GoldenVector vector = recorded();
  EXPECT_EQ(vector.key, (GoldenKey{"golden_mock", 1, simd::IsaPath::kScalar, config.seed,
                                   config.footprint_bytes, 0, 8, 3}));
  ASSERT_EQ(vector.units.size(), 3U);
  std::vector<std::byte> all;
  SerialExecutor serial;
  for (std::uint64_t index = 0; index < 3; ++index) {
    std::vector<std::byte> input(MockWorkload::kInputBytes);
    std::vector<std::byte> output(MockWorkload::kOutputBytes);
    workload->generate(index, input);
    workload->compute(index, input, output, {}, serial);
    EXPECT_EQ(vector.units[index], (UnitDigest{crc64(input), crc64(output)})) << index;
    all.insert(all.end(), output.begin(), output.end());
  }
  EXPECT_EQ(vector.digest, crc64(all));
}

TEST(GoldenRecord, NoUnitIsRefused) {
  const workloads::WorkloadConfig config = golden_test::fixture_config();
  SerialExecutor serial;
  const auto vector = record_golden(*make(config), config, 0, serial);
  ASSERT_FALSE(vector.has_value());
  EXPECT_EQ(vector.error(),
            (GoldenError{std::nullopt, "no unit to record: a vector of nothing compares nothing"}));
}

TEST(GoldenCompare, TheSameCodeIsUnchanged) {
  const GoldenReport report = compare_golden(recorded(), recorded());
  EXPECT_EQ(report, (GoldenReport{GoldenVerdict::kUnchanged, {}}));
  EXPECT_EQ(render(report, recorded().key),
            "golden vector for golden_mock version 1 on scalar, seed 0x000000000601de11, 3 "
            "unit(s): unchanged\n");
}

TEST(GoldenCompare, AnIntendedChangeIsDetectedAndDeclared) {
  // The workload's bits changed and its version moved with them, as
  // workload.hpp requires: the vector notices, and says the change was
  // declared -- the vector is stale, not the machine bad.
  const GoldenReport report = compare_golden(recorded(1), recorded(2, Flavor::kChanged));
  EXPECT_EQ(report.verdict, GoldenVerdict::kDeclaredChange);
  ASSERT_GE(report.changes.size(), 5U);
  EXPECT_EQ(report.changes[0], (GoldenChange{"version", "1", "2"}));
  EXPECT_EQ(report.changes[1].what, "unit 0 output");
  EXPECT_EQ(report.changes[2].what, "unit 1 output");
  EXPECT_EQ(report.changes[3].what, "unit 2 output");
  EXPECT_EQ(report.changes[4].what, "digest");
  const std::string text = render(report, recorded().key);
  EXPECT_NE(text.find(": changed, and the workload's version says so: the vector is stale\n"),
            std::string::npos);
  EXPECT_NE(text.find("  version: recorded 1, now 2\n"), std::string::npos);
  EXPECT_NE(text.find("It is not a finding about the hardware."), std::string::npos);
}

TEST(GoldenCompare, AVersionMovedWithoutABitChangedIsStillDeclared) {
  const GoldenReport report = compare_golden(recorded(1), recorded(2));
  EXPECT_EQ(report, (GoldenReport{GoldenVerdict::kDeclaredChange, {{"version", "1", "2"}}}));
}

TEST(GoldenCompare, BitsChangedUnderTheSameVersionAreARegression) {
  const GoldenReport report = compare_golden(recorded(1), recorded(1, Flavor::kChanged));
  EXPECT_EQ(report.verdict, GoldenVerdict::kChanged);
  for (const GoldenChange& change : report.changes) {
    EXPECT_EQ(change.what.find("input"), std::string::npos)
        << "the input did not change, and is not reported as having changed";
  }
  ASSERT_EQ(report.changes.size(), 4U);
  EXPECT_EQ(report.changes[0].what, "unit 0 output");
  EXPECT_EQ(report.changes[3].what, "digest");
  EXPECT_NE(render(report, recorded().key)
                .find(": changed under the same version: a regression report\n  unit 0 output: "
                      "recorded 0x"),
            std::string::npos);
}

TEST(GoldenCompare, AChangedInputAndAChangedDecompositionAreNamed) {
  GoldenVector fresh = recorded();
  fresh.units[1].input ^= 1;
  fresh.key.resolved_decomposition = 16;
  const GoldenReport report = compare_golden(recorded(), fresh);
  EXPECT_EQ(report.verdict, GoldenVerdict::kChanged);
  ASSERT_EQ(report.changes.size(), 2U);
  EXPECT_EQ(report.changes[0], (GoldenChange{"resolved_decomposition", "8", "16"}));
  EXPECT_EQ(report.changes[1].what, "unit 1 input");
}

TEST(GoldenCompare, VectorsOfDifferentRunsAreNotComparedAtAll) {
  const GoldenVector base = recorded();
  const std::vector<std::pair<void (*)(GoldenKey&), GoldenChange>> cases = {
      {[](GoldenKey& k) { k.workload = "other"; }, {"workload", "golden_mock", "other"}},
      {[](GoldenKey& k) { k.isa = simd::IsaPath::kNeon; }, {"isa", "scalar", "neon"}},
      {[](GoldenKey& k) { k.seed = 1; }, {"seed", "0x000000000601de11", "0x0000000000000001"}},
      {[](GoldenKey& k) { k.footprint_bytes = 1; }, {"footprint_bytes", "4160", "1"}},
      {[](GoldenKey& k) { k.decomposition = 2; }, {"decomposition", "0", "2"}},
      {[](GoldenKey& k) { k.units = 4; }, {"units", "3", "4"}},
  };
  for (const auto& [change, named] : cases) {
    GoldenVector fresh = base;
    change(fresh.key);
    fresh.digest ^= 1;  // nothing past the key is looked at
    const GoldenReport report = compare_golden(base, fresh);
    EXPECT_EQ(report, (GoldenReport{GoldenVerdict::kNotComparable, {named}})) << named.what;
    EXPECT_NE(render(report, base.key)
                  .find(": not comparable: the two vectors are of different "
                        "runs\n"),
              std::string::npos);
  }
}

TEST(GoldenCompare, ARecordedVectorOfNoUnitComparesNothingAndSaysSo) {
  GoldenVector empty_list = recorded();
  empty_list.units.clear();
  GoldenVector zero_count = recorded();
  zero_count.key.units = 0;
  for (const GoldenVector& vector : {empty_list, zero_count}) {
    const GoldenReport report = compare_golden(vector, recorded());
    EXPECT_EQ(report, (GoldenReport{GoldenVerdict::kNothingCompared, {}}));
    EXPECT_NE(render(report, vector.key).find("which is not a pass"), std::string::npos);
  }
}

TEST(GoldenCompare, VerdictWordsAndEquality) {
  EXPECT_EQ(describe(GoldenVerdict::kUnchanged), "unchanged");
  EXPECT_EQ(describe(static_cast<GoldenVerdict>(9)), "unrecognised golden verdict");
  contracts::expect_memberwise_equality(
      GoldenChange{"a", "b", "c"},
      {[](GoldenChange& c) { c.what = "x"; }, [](GoldenChange& c) { c.recorded = "x"; },
       [](GoldenChange& c) { c.now = "x"; }});
  contracts::expect_memberwise_equality(
      GoldenReport{GoldenVerdict::kChanged, {{"a", "b", "c"}}},
      {[](GoldenReport& r) { r.verdict = GoldenVerdict::kUnchanged; },
       [](GoldenReport& r) { r.changes.clear(); }});
}

}  // namespace
}  // namespace loadforge::verification
