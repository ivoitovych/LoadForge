// SPDX-License-Identifier: GPL-3.0-or-later
//
// The adaptive mesh oracle against OracleContract (ADR 0003), and what the
// contract leaves to this oracle (T1): the sampled cells, the items and their
// order, what a failed item says, the tolerance held and breached at its
// edge, the hotspots it will not judge, and the refusals of a plan that is
// not an adaptive mesh unit's. Good units come from the model in
// mesh_support.hpp.

#include "verification/oracle/adaptive_mesh/oracle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "unit/verification/oracle/adaptive_mesh/mesh_support.hpp"
#include "verification/oracle.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::adaptive_mesh {

namespace spec = workloads::adaptive_mesh;
namespace mesh = test::mesh;

workloads::Plan mesh_plan() {
  return workloads::Plan{spec::kInputBytes,     spec::kOutputBytes, 0, 4, 1,
                         3 * spec::kInputBytes, spec::kHotspots};
}

std::unique_ptr<Oracle> make_oracle() {
  return std::move(create(workloads::WorkloadConfig{}, mesh_plan())).value();
}

struct AdaptiveMeshOracleSubject {
  static constexpr const char* kWorkload = "adaptive_mesh";
  static constexpr Contract kContract = Contract::kBounded;
  static std::unique_ptr<Oracle> make() { return make_oracle(); }
  static std::vector<contracts::OracleCase> good() {
    std::vector<contracts::OracleCase> cases;
    cases.push_back(mesh::unit_of(0, mesh::generated(1, 0)));
    cases.push_back(mesh::unit_of(5, mesh::generated(1, 5)));
    cases.push_back(mesh::unit_of(6, mesh::all_at(0, 0)));
    cases.push_back(mesh::unit_of(7, mesh::all_at(511, 511)));
    cases.push_back(mesh::unit_of(8, mesh::crowded(sampled_cell(8, 3))));
    return cases;
  }
  static std::vector<contracts::OracleCase> wrong() {
    std::vector<contracts::OracleCase> cases;
    for (const std::uint64_t index : {0U, 5U}) {
      const contracts::OracleCase good = mesh::unit_of(index, mesh::generated(1, index));
      contracts::OracleCase count = good;
      mesh::put32(count.output, spec::count_offset(77),
                  mesh::get32(count.output, spec::count_offset(77)) + 1);
      cases.push_back(count);
      for (const std::size_t j : {0U, 9U, 31U}) {
        contracts::OracleCase integral = good;
        const std::size_t c = sampled_cell(index, j);
        mesh::put_double(integral.output, spec::integral_offset(c),
                         2.0 * mesh::get_double(integral.output, spec::integral_offset(c)));
        cases.push_back(integral);
      }
    }
    return cases;
  }
};

namespace {

BoundedMismatch mismatch_of(const Verdict& verdict) {
  const auto* bounded = std::get_if<BoundedMismatch>(&verdict);
  EXPECT_NE(bounded, nullptr) << "not a BoundedMismatch";
  return bounded != nullptr ? *bounded : BoundedMismatch{"none", 0.0, 1.0, std::nullopt, 0, 1};
}

NotChecked not_checked_of(const Verdict& verdict) {
  const auto* unchecked = std::get_if<NotChecked>(&verdict);
  EXPECT_NE(unchecked, nullptr) << "not a NotChecked";
  return unchecked != nullptr ? *unchecked : NotChecked{"none", "none", 1};
}

Verdict judge(const Oracle& oracle, std::uint64_t index, const mesh::Bytes& input,
              const mesh::Bytes& output) {
  return oracle.check(UnitEvidence{index, input, output});
}

TEST(AdaptiveMeshOracle, NamesTheAdaptiveMeshWorkloadAndTheBoundedContract) {
  const AdaptiveMeshOracle oracle;
  EXPECT_EQ(oracle.workload(), spec::kName);
  EXPECT_EQ(oracle.workload(), kWorkload);
  EXPECT_EQ(oracle.contract(), Contract::kBounded);
}

TEST(AdaptiveMeshSamples, SampleJIsACellWithAnIndexInItsEightAndTheyAreAFunctionOfTheIndex) {
  std::set<std::vector<std::size_t>> distinct;
  for (std::uint64_t index = 0; index < 20; ++index) {
    std::vector<std::size_t> cells;
    for (std::size_t j = 0; j < spec::kSampledCells; ++j) {
      const std::size_t c = sampled_cell(index, j);
      EXPECT_GE(c, 8 * j);
      EXPECT_LT(c, 8 * j + 8);
      EXPECT_EQ(c, sampled_cell(index, j));
      cells.push_back(c);
    }
    distinct.insert(cells);
  }
  EXPECT_GT(distinct.size(), 15U);
}

TEST(AdaptiveMeshSamples, EveryPositionInAnEightIsReachedBySomeIndex) {
  std::set<std::size_t> positions;
  for (std::uint64_t index = 0; index < 400; ++index) {
    positions.insert(sampled_cell(index, 4) % 8);
  }
  EXPECT_EQ(positions.size(), 8U);
}

TEST(AdaptiveMeshOracle, AgreesAfterComparingEveryCountAndEverySampledIntegral) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const Verdict verdict =
      judge(*make_oracle(), 3, mesh::bytes_of(hotspots), mesh::render(mesh::model_of(hotspots)));
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(blocks_of(verdict), spec::kCoarseCells + spec::kSampledCells);
}

TEST(AdaptiveMeshOracle, AnyOrderOfAddingTheLeavesIsRight) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Model model = mesh::model_of(hotspots);
  mesh::Bytes forward = mesh::render(model);
  mesh::Bytes backward = forward;
  mesh::Bytes pairwise = forward;
  bool differed = false;
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    mesh::put_double(backward, spec::integral_offset(c), mesh::reverse_order_sum(model.leaves[c]));
    mesh::put_double(pairwise, spec::integral_offset(c), mesh::pairwise_order_sum(model.leaves[c]));
    differed = differed || backward != forward || pairwise != forward;
  }
  ASSERT_TRUE(differed) << "the orders should give some different bits, or the test shows nothing";
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), 3, input, backward)));
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), 3, input, pairwise)));
}

TEST(AdaptiveMeshOracle, ALeafCountThatDiffersNamesItsCellAndStopsTheCheck) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  mesh::Bytes output = mesh::render(mesh::model_of(hotspots));
  mesh::put32(output, spec::count_offset(200), 9);
  mesh::put32(output, spec::count_offset(41), mesh::get32(output, spec::count_offset(41)) + 3);
  // An integral that is wrong too, to show the counts come first.
  mesh::put_double(output, spec::integral_offset(sampled_cell(3, 0)), 1.0e9);
  const BoundedMismatch found = mismatch_of(judge(*make_oracle(), 3, input, output));
  EXPECT_EQ(found.check, std::string(spec::kCheckLeafCount) + " 41");
  EXPECT_EQ(found.residual, 3.0);
  EXPECT_EQ(found.tolerance, spec::kCountTolerance);
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(41));
  EXPECT_FALSE(found.condition_number.has_value());
  EXPECT_EQ(found.blocks_compared, 42U);
  EXPECT_TRUE(is_valid(Verdict{found}, Contract::kBounded));
}

TEST(AdaptiveMeshOracle, ALeafCountBelowTheRightOneIsAResidualOfTheDifference) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  mesh::Bytes output = mesh::render(mesh::model_of(hotspots));
  const std::uint32_t right = mesh::get32(output, spec::count_offset(0));
  mesh::put32(output, spec::count_offset(0), 0);
  BoundedMismatch found = mismatch_of(judge(*make_oracle(), 3, input, output));
  EXPECT_EQ(found.residual, static_cast<double>(right));
  EXPECT_EQ(found.blocks_compared, 1U);
  mesh::put32(output, spec::count_offset(0), 0xFFFFFFFFU);
  found = mismatch_of(judge(*make_oracle(), 3, input, output));
  EXPECT_EQ(found.residual, 4294967295.0 - static_cast<double>(right));
}

TEST(AdaptiveMeshOracle, AnIntegralOutsideItsToleranceNamesTheCellAndTheDerivedTolerance) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Model model = mesh::model_of(hotspots);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  mesh::Bytes output = mesh::render(model);
  const std::size_t c = sampled_cell(3, 11);
  const double reference = model.reference[c];
  mesh::put_double(output, spec::integral_offset(c), reference * 1.001);
  // A later sample is wrong as well; the earlier one is the one named.
  mesh::put_double(output, spec::integral_offset(sampled_cell(3, 20)), 0.0);
  const BoundedMismatch found = mismatch_of(judge(*make_oracle(), 3, input, output));
  EXPECT_EQ(found.check, std::string(spec::kCheckIntegral) + " " + std::to_string(c));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(c));
  EXPECT_EQ(found.blocks_compared, spec::kCoarseCells + 12);
  EXPECT_EQ(found.tolerance,
            spec::cell_tolerance(static_cast<std::uint32_t>(model.leaves[c].size()), reference));
  EXPECT_EQ(found.residual, std::fabs(reference * 1.001 - reference));
  EXPECT_FALSE(found.condition_number.has_value());
  EXPECT_TRUE(is_valid(Verdict{found}, Contract::kBounded));
}

TEST(AdaptiveMeshOracle, AnIntegralThatIsNotAFiniteNumberIsOutsideEveryTolerance) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  const double inf = std::numeric_limits<double>::infinity();
  for (const double stored : {std::numeric_limits<double>::quiet_NaN(), inf, -inf, -0.5}) {
    mesh::Bytes output = mesh::render(mesh::model_of(hotspots));
    mesh::put_double(output, spec::integral_offset(sampled_cell(3, 0)), stored);
    const Verdict verdict = judge(*make_oracle(), 3, input, output);
    const BoundedMismatch found = mismatch_of(verdict);
    EXPECT_EQ(found.blocks_compared, spec::kCoarseCells + 1);
    EXPECT_FALSE(found.residual <= found.tolerance);
    EXPECT_TRUE(is_valid(verdict, Contract::kBounded)) << stored;
  }
}

TEST(AdaptiveMeshOracle, AnIntegralOfACellNobodySamplesIsNotLookedAt) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  mesh::Bytes output = mesh::render(mesh::model_of(hotspots));
  std::set<std::size_t> sampled;
  for (std::size_t j = 0; j < spec::kSampledCells; ++j) {
    sampled.insert(sampled_cell(3, j));
  }
  std::size_t unsampled = 0;
  while (sampled.contains(unsampled)) {
    ++unsampled;
  }
  mesh::put_double(output, spec::integral_offset(unsampled), 1.0e9);
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), 3, input, output)));
}

TEST(AdaptiveMeshOracle, ACellWithOneLeafHoldsOneUlpAndRefusesThree) {
  // One hotspot far from the sampled cell: the cell has a single leaf, and
  // its tolerance is about one ulp of its integral (2 u c' / (1 - 2 u)).
  const mesh::Hotspots hotspots = mesh::all_at(0, 0);
  const mesh::Model model = mesh::model_of(hotspots);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  std::size_t j = 0;
  std::size_t c = sampled_cell(3, j);
  while (model.leaves[c].size() != 1) {
    c = sampled_cell(3, ++j);
  }
  const double reference = model.reference[c];
  const double inf = std::numeric_limits<double>::infinity();
  mesh::Bytes output = mesh::render(model);
  mesh::put_double(output, spec::integral_offset(c), std::nextafter(reference, inf));
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), 3, input, output)));
  mesh::put_double(output, spec::integral_offset(c), std::nextafter(reference, -inf));
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), 3, input, output)));
  const double three_up = std::nextafter(std::nextafter(std::nextafter(reference, inf), inf), inf);
  mesh::put_double(output, spec::integral_offset(c), three_up);
  const BoundedMismatch found = mismatch_of(judge(*make_oracle(), 3, input, output));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(c));
  EXPECT_EQ(found.blocks_compared, spec::kCoarseCells + j + 1);
}

TEST(AdaptiveMeshOracle, ACrowdedCellHoldsAt99PercentOfItsToleranceAndBreachesAt101Percent) {
  const std::uint64_t index = 11;
  const std::size_t j = 3;
  const std::size_t c = sampled_cell(index, j);
  const mesh::Hotspots hotspots = mesh::crowded(c);
  const mesh::Model model = mesh::model_of(hotspots);
  ASSERT_EQ(model.leaves[c].size(), spec::kMaxLeavesPerCell);
  const mesh::Bytes input = mesh::bytes_of(hotspots);
  const double reference = model.reference[c];
  const double tolerance = spec::cell_tolerance(spec::kMaxLeavesPerCell, reference);
  // The resolution of a stored double: how many ulps the tolerance spans.
  const double ulp = std::nextafter(reference, std::numeric_limits<double>::infinity()) - reference;
  ASSERT_GT(tolerance / ulp, 1000.0);
  for (const double sign : {1.0, -1.0}) {
    mesh::Bytes holds = mesh::render(model);
    mesh::put_double(holds, spec::integral_offset(c), reference + sign * tolerance * 0.99);
    const double held_residual =
        std::fabs(mesh::get_double(holds, spec::integral_offset(c)) - reference);
    EXPECT_LE(held_residual, tolerance);
    EXPECT_GT(held_residual, tolerance * 0.98);
    EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), index, input, holds)));
    mesh::Bytes breaches = mesh::render(model);
    mesh::put_double(breaches, spec::integral_offset(c), reference + sign * tolerance * 1.01);
    const BoundedMismatch found = mismatch_of(judge(*make_oracle(), index, input, breaches));
    EXPECT_GT(found.residual, tolerance);
    EXPECT_LT(found.residual, tolerance * 1.02);
    EXPECT_EQ(found.tolerance, tolerance);
  }
}

TEST(AdaptiveMeshOracle, AHotspotOutsideTheLatticeIsNotJudged) {
  const mesh::Hotspots hotspots = mesh::generated(2, 3);
  const mesh::Bytes output = mesh::render(mesh::model_of(hotspots));
  struct Place {
    std::size_t hotspot;
    bool y;
    std::uint32_t value;
  };
  const std::vector<Place> places = {
      {0, false, 512},  {0, true, 512},         {255, false, 512},
      {255, true, 513}, {17, false, 1U << 31U}, {17, true, 0xFFFFFFFFU},
  };
  for (const Place& place : places) {
    mesh::Bytes input = mesh::bytes_of(hotspots);
    mesh::put32(input, place.hotspot * spec::kHotspotBytes + (place.y ? 4 : 0), place.value);
    const Verdict verdict = judge(*make_oracle(), 3, input, output);
    const NotChecked unchecked = not_checked_of(verdict);
    EXPECT_EQ(unchecked.blocks_compared, 0U);
    EXPECT_NE(unchecked.why.find("hotspot " + std::to_string(place.hotspot)), std::string::npos);
    EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  }
}

TEST(AdaptiveMeshOracle, TheLastCoordinateOfTheLatticeIsJudged) {
  const mesh::Hotspots hotspots = mesh::all_at(511, 0);
  const contracts::OracleCase unit = mesh::unit_of(3, hotspots);
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), 3, unit.input, unit.output)));
}

TEST(AdaptiveMeshOracle, EvidenceOfAnotherSizeSaysWhatItGotAndWhatAUnitIs) {
  const contracts::OracleCase good = mesh::unit_of(1, mesh::generated(1, 1));
  const NotChecked unchecked =
      not_checked_of(judge(*make_oracle(), 1, good.input, mesh::Bytes(10)));
  EXPECT_EQ(unchecked.blocks_compared, 0U);
  EXPECT_NE(unchecked.why.find("10 of output"), std::string::npos);
  EXPECT_NE(unchecked.why.find(std::to_string(spec::kOutputBytes)), std::string::npos);
}

TEST(AdaptiveMeshCreate, GivesTheOracleForDecompositionZeroOrOne) {
  for (const std::uint32_t configured : {0U, 1U}) {
    workloads::WorkloadConfig config;
    config.decomposition = configured;
    const auto created = create(config, mesh_plan());
    ASSERT_TRUE(created.has_value()) << configured;
    EXPECT_EQ(created.value()->workload(), "adaptive_mesh");
  }
}

TEST(AdaptiveMeshCreate, RefusesWhatIsNotAnAdaptiveMeshUnit) {
  workloads::Plan wrong_input = mesh_plan();
  wrong_input.input_bytes -= 1;
  workloads::Plan wrong_output = mesh_plan();
  wrong_output.output_bytes += 1;
  workloads::Plan decomposed = mesh_plan();
  decomposed.decomposition = 2;
  workloads::WorkloadConfig two;
  two.decomposition = 2;
  const auto refusals = {
      create(workloads::WorkloadConfig{}, wrong_input),
      create(workloads::WorkloadConfig{}, wrong_output),
      create(workloads::WorkloadConfig{}, decomposed),
      create(two, mesh_plan()),
  };
  for (const auto& refused : refusals) {
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(refused.error().detail.empty());
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::adaptive_mesh

namespace loadforge::contracts {
namespace {
using AdaptiveMeshOracleSubjects =
    ::testing::Types<verification::oracle::adaptive_mesh::AdaptiveMeshOracleSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(AdaptiveMeshOracle, OracleContract, AdaptiveMeshOracleSubjects);
}  // namespace

// The registry's suite comes with the header; the registry is the
// integration stream's to provide (oracle.hpp), not this module's.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts
