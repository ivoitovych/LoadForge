// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the particles oracle: the forces scipy's neighbour search and numpy
// computed for a generated unit are agreed with, a corrupted output is not, and
// the derived tolerance holds at the boundary the specification names --
// breached at (1 + epsilon) times T' and held at (1 - epsilon) times it, for a
// particle with one candidate and for one with thirty-three.

#include "verification/oracle/particles/particles_oracle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "oracle/verification/oracle/particles/particles_evidence.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::verification::oracle::particles {
namespace {

namespace spec = workloads::particles;
using evidence::Point;
using evidence::Rig;
using evidence::Unit;

constexpr double kEpsilon = 1.0 / 1048576.0;  // 2^-20, the specification's

std::unique_ptr<Oracle> make_oracle(std::uint32_t cells = spec::kDefaultCellsPerCutoff) {
  auto created = create(workloads::WorkloadConfig{},
                        workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, cells, 0, 0});
  return std::move(created).value();
}

contracts::OracleCase as_case(const Unit& unit) {
  return contracts::OracleCase{unit.index, unit.input, unit.output};
}

Verdict check_unit(const Oracle& oracle, const Unit& unit) {
  return oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
}

std::vector<contracts::OracleCase> real_tool_cases() {
  return {as_case(evidence::real_tool_unit())};
}

// An exponent flip in a sampled entry (a sample is all the oracle examines, so
// the corruptions are made where the unit's index samples), the sign of every
// entry, and a NaN.
std::vector<contracts::OracleCase> corrupted_cases() {
  const contracts::OracleCase base = as_case(evidence::real_tool_unit());
  const auto sampled = sampled_particles(base.index);
  std::vector<contracts::OracleCase> cases;
  for (const std::size_t which : {std::size_t{0}, std::size_t{7}, std::size_t{15}}) {
    contracts::OracleCase unit = base;
    unit.output[(3 * std::size_t{sampled[which]} + which % 3) * 8 + 7] ^= std::byte{0x40};
    cases.push_back(unit);
  }
  contracts::OracleCase negated = base;
  for (std::size_t element = 0; element < 3 * spec::kParticles; ++element) {
    negated.output[element * 8 + 7] ^= std::byte{0x80};
  }
  cases.push_back(negated);
  contracts::OracleCase nan = base;
  const std::vector<std::byte> nan_bytes = evidence::bytes_of(std::vector<double>(1, std::nan("")));
  std::copy(nan_bytes.begin(), nan_bytes.end(),
            nan.output.begin() + static_cast<std::ptrdiff_t>(std::size_t{24} * sampled[4]));
  cases.push_back(nan);
  return cases;
}

// One neighbour just inside the cutoff: its term is about 2e-8 and the force
// on the sampled particle is that term alone, so the offsets made below are
// resolved far finer than epsilon times the tolerance.
Rig one_candidate_rig(std::uint64_t index) {
  Rig rig(index);
  for (std::size_t k = 0; k < rig.sampled.size(); ++k) {
    rig.place_near(k, Point{1.9999, 0.0, 0.0});
  }
  return rig;
}

// Thirty-two candidates in opposite pairs and one at the sampled particle's own
// position: a force that cancels to within the rounding of the sum.
Rig thirty_three_candidate_rig(std::uint64_t index) {
  Rig rig(index);
  const std::vector<Point> offsets = evidence::symmetric_offsets();
  for (std::size_t k = 0; k < rig.sampled.size(); ++k) {
    for (const Point& offset : offsets) {
      rig.place_near(k, offset);
    }
  }
  return rig;
}

double expected_limit(std::size_t candidates, double abs_sum) {
  return spec::force_tolerance(candidates, abs_sum) * (1.0 + spec::kEvaluationSlack);
}

// The boundary of one entry: the sampled particle `which` of the rig, component
// `component`. F~ is the oracle's own force, which is known to be tiny, so the
// offset is the residual to far better than epsilon.
void expect_boundary(const Rig& rig, std::size_t which, std::size_t component,
                     std::size_t candidates, double min_abs_sum) {
  const auto oracle = make_oracle();
  const std::uint32_t particle = rig.sampled[which];
  const Force f = direct_force(rig.positions, particle);
  ASSERT_EQ(f.candidates, candidates);
  ASSERT_GE(f.abs_sum[component], min_abs_sum);
  const double limit = expected_limit(candidates, f.abs_sum[component]);
  const std::size_t element = 3 * std::size_t{particle} + component;
  std::vector<double> forces = rig.agreeing_forces();
  const double exact = forces[element];
  ASSERT_LT(std::fabs(exact), 1e-7)
      << "the rig's force is small, so its ulp is far below epsilon T";

  forces[element] = exact + limit * (1.0 + kEpsilon);
  const Verdict breached = check_unit(*oracle, rig.unit_with(forces));
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(breached))
      << "particle " << particle << " component " << component;
  const auto& mismatch = std::get<BoundedMismatch>(breached);
  EXPECT_EQ(mismatch.check, "force-component");
  EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>(element));
  EXPECT_EQ(mismatch.blocks_compared, 3 * which + component + 1);
  EXPECT_EQ(mismatch.tolerance, limit);
  EXPECT_NEAR(mismatch.residual, limit * (1.0 + kEpsilon), limit * 0.1 * kEpsilon);
  EXPECT_TRUE(is_valid(breached, Contract::kBounded));

  forces[element] = exact - limit * (1.0 - kEpsilon);
  const Verdict held = check_unit(*oracle, rig.unit_with(forces));
  ASSERT_TRUE(std::holds_alternative<Agreement>(held))
      << "particle " << particle << " component " << component;
  EXPECT_EQ(std::get<Agreement>(held).blocks_compared, spec::kEntriesChecked);
}

TEST(ParticlesOracleTolerance, OneCandidateBreachedJustAboveItsToleranceAndHeldJustBelow) {
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{13}}) {
    const Rig rig = one_candidate_rig(index);
    for (const std::size_t which : {std::size_t{0}, std::size_t{9}, spec::kSampledParticles - 1}) {
      for (std::size_t component = 0; component < 3; ++component) {
        expect_boundary(rig, which, component, 1, component == 0 ? 1e-9 : 0.0);
      }
    }
  }
}

TEST(ParticlesOracleTolerance,
     ThirtyThreeCandidatesBreachedJustAboveTheirToleranceAndHeldJustBelow) {
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{13}}) {
    const Rig rig = thirty_three_candidate_rig(index);
    for (const std::size_t which : {std::size_t{0}, std::size_t{5}, spec::kSampledParticles - 1}) {
      for (std::size_t component = 0; component < 3; ++component) {
        expect_boundary(rig, which, component, 33, 1.0);
      }
    }
  }
}

TEST(ParticlesOracleTolerance, TheThirtyThreeCandidateToleranceIsOfTheSizeTheSpecificationQuotes) {
  // "At M_p = 33 and a typical S~ of about 15 it is about 4e-13" (spec.hpp).
  const double limit = expected_limit(33, 15.0);
  EXPECT_GT(limit, 2e-13);
  EXPECT_LT(limit, 8e-13);
  EXPECT_EQ(tolerance_for(33, 15.0), limit);
}

TEST(ParticlesOracleRealTool, TheNeighbourSearchForcesAreAgreedWithAtEveryCellCount) {
  for (const std::uint32_t cells : spec::kCellsPerCutoff) {
    const auto oracle = make_oracle(cells);
    const Verdict verdict = check_unit(*oracle, evidence::real_tool_unit());
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "cells " << cells;
    EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kEntriesChecked);
  }
}

TEST(ParticlesOracleRealTool, TheFixtureForcesSumToZeroAsNewtonsThirdLawSays) {
  const Unit unit = evidence::real_tool_unit();
  const std::vector<double> forces = evidence::doubles_of(unit.output);
  std::array<long double, 3> total = {0.0L, 0.0L, 0.0L};
  double largest = 0.0;
  for (std::size_t p = 0; p < spec::kParticles; ++p) {
    for (std::size_t c = 0; c < 3; ++c) {
      total.at(c) += forces[3 * p + c];
      largest = std::max(largest, std::fabs(forces[3 * p + c]));
    }
  }
  for (const long double component : total) {
    EXPECT_LT(std::fabs(static_cast<double>(component)), 1e-10);
  }
  EXPECT_GT(largest, 0.1) << "the unit has real forces in it";
}

}  // namespace
}  // namespace loadforge::verification::oracle::particles

// The suites are instantiated where they are declared: the macros name them
// unqualified.
namespace loadforge::contracts {
namespace {

namespace under_test = verification::oracle::particles;

struct ParticlesSubject {
  static constexpr const char* kWorkload = "particles";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return under_test::make_oracle(); }
  static std::vector<OracleCase> good() { return under_test::real_tool_cases(); }
  static std::vector<OracleCase> wrong() { return under_test::corrupted_cases(); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Particles, OracleContract, ::testing::Types<ParticlesSubject>);

}  // namespace
}  // namespace loadforge::contracts
