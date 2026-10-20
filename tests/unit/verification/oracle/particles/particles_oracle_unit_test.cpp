// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the particles oracle: its sampling, its direct sum against an
// independent evaluation, the cutoff and the candidate count at their edges,
// its tolerance function and the plans it refuses, each on its own.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include "oracle/verification/oracle/particles/particles_evidence.hpp"
#include "verification/oracle/particles/particles_oracle.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::verification::oracle::particles {
namespace {

namespace spec = workloads::particles;
using evidence::Point;
using evidence::Rig;

TEST(ParticlesLoadDouble, ReadsLittleEndianWhateverTheHostIs) {
  const std::vector<std::byte> bytes = {
      std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0xF0}, std::byte{0x3F}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0}};
  EXPECT_EQ(load_double(bytes, 0), 1.0);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(load_double(bytes, 1)), 1U);
}

TEST(ParticlesSampled, AreSortedDistinctPinnedAndAFunctionOfTheIndexAlone) {
  std::set<std::vector<std::uint32_t>> distinct_sets;
  for (std::uint64_t index = 0; index < 3000; ++index) {
    const auto sample = sampled_particles(index);
    EXPECT_TRUE(std::is_sorted(sample.begin(), sample.end())) << "index " << index;
    EXPECT_EQ(std::adjacent_find(sample.begin(), sample.end()), sample.end()) << "index " << index;
    EXPECT_EQ(sample.front(), 0U);
    EXPECT_EQ(sample.back(), spec::kParticles - 1);
    EXPECT_EQ(sample, sampled_particles(index)) << "a re-run checks the same particles";
    distinct_sets.insert(std::vector<std::uint32_t>(sample.begin(), sample.end()));
  }
  // The sample is set by one number in [0, 4094), so 3000 indices share by the
  // birthday bound (about 2100 distinct); a sample fixed by anything less would
  // show far fewer.
  EXPECT_GT(distinct_sets.size(), 1800U) << "units are spread over the particles";
  EXPECT_EQ(sampled_particles(~std::uint64_t{0}).size(), spec::kSampledParticles);
}

// The same force by an evaluation of the specification's model in long double,
// term by term, with no shared code: within the tolerance the specification
// derives for the double evaluation.
TEST(ParticlesDirectForce, AgreesWithAnIndependentEvaluationOnTheFixtureUnit) {
  const std::vector<double> positions =
      evidence::generated_positions(evidence::kSeed, evidence::kFixtureIndex);
  for (const std::uint32_t p : sampled_particles(evidence::kFixtureIndex)) {
    const Force f = direct_force(positions, p);
    std::array<long double, 3> truth = {0.0L, 0.0L, 0.0L};
    std::size_t candidates = 0;
    for (std::size_t j = 0; j < spec::kParticles; ++j) {
      if (j == p) {
        continue;
      }
      std::array<long double, 3> d{};
      long double q = 0.0L;
      for (std::size_t c = 0; c < 3; ++c) {
        d.at(c) = static_cast<long double>(positions[3 * std::size_t{p} + c]) -
                  static_cast<long double>(positions[3 * j + c]);
        q += d.at(c) * d.at(c);
      }
      if (q < 4.0L * (1.0L + 0x1p-40L)) {
        ++candidates;
      }
      const long double t = 1.0L - q / 4.0L;
      if (t > 0.0L) {
        for (std::size_t c = 0; c < 3; ++c) {
          truth.at(c) += t * t * d.at(c);
        }
      }
    }
    EXPECT_EQ(f.candidates, candidates) << "particle " << p;
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_NEAR(f.force.at(c), static_cast<double>(truth.at(c)),
                  tolerance_for(f.candidates, f.abs_sum.at(c)) / 4.0)
          << "particle " << p << " component " << c;
    }
  }
}

TEST(ParticlesDirectForce, ThereIsAForceOnlyWithinTheCutoffAndItIsZeroAtAndBeyondIt) {
  Rig rig(0);
  const std::size_t p = rig.sampled[3];
  const auto force_with_neighbour_at = [&](double distance) {
    Rig local(0);
    local.place_near(3, Point{distance, 0.0, 0.0});
    return direct_force(local.positions, p);
  };
  // Just inside: a small force of the neighbour's sign, and a candidate.
  const Force inside = force_with_neighbour_at(1.9999);
  EXPECT_EQ(inside.candidates, 1U);
  EXPECT_LT(inside.force[0], 0.0) << "repulsion: the neighbour is on the +x side";
  EXPECT_GT(inside.abs_sum[0], 0.0);
  EXPECT_EQ(inside.force[1], 0.0);
  EXPECT_EQ(inside.force[2], 0.0);
  // At the cutoff exactly: q = 4, t = 0: no force, but a candidate.
  const Force at = force_with_neighbour_at(2.0);
  EXPECT_EQ(at.candidates, 1U);
  EXPECT_EQ(at.force[0], 0.0);
  EXPECT_EQ(at.abs_sum[0], 0.0);
  // Between the cutoff and the candidate limit: no force, still a candidate.
  const Force between = force_with_neighbour_at(2.0000000000005);
  EXPECT_EQ(between.candidates, 1U);
  EXPECT_EQ(between.force[0], 0.0);
  // Beyond the candidate limit: neither.
  const Force beyond = force_with_neighbour_at(2.000000000001);
  EXPECT_EQ(beyond.candidates, 0U);
  EXPECT_EQ(beyond.force[0], 0.0);
  // On top of the particle itself: g(0) = 0, a candidate that exerts nothing.
  Rig coincident(0);
  coincident.place_near(3, Point{0.0, 0.0, 0.0});
  const Force on_top = direct_force(coincident.positions, p);
  EXPECT_EQ(on_top.candidates, 1U);
  EXPECT_EQ(on_top.force[0], 0.0);
  EXPECT_EQ(on_top.force[1], 0.0);
  EXPECT_EQ(on_top.force[2], 0.0);
  // Nobody near: nothing at all.
  const Force alone = direct_force(rig.positions, p);
  EXPECT_EQ(alone.candidates, 0U);
  EXPECT_EQ(alone.force[0], 0.0);
}

TEST(ParticlesDirectForce, ThreeComponentsAreIndependentAndNewtonsThirdLawHoldsForAPair) {
  Rig rig(0);
  rig.place_near(0, Point{0.5, -0.75, 1.0});
  const std::size_t other = rig.free_particles[0];
  const Force on_sampled = direct_force(rig.positions, rig.sampled[0]);
  const Force on_other = direct_force(rig.positions, other);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_NE(on_sampled.force[c], 0.0) << "component " << c;
    EXPECT_EQ(on_other.force[c], -on_sampled.force[c]) << "component " << c;
  }
  EXPECT_LT(on_sampled.force[0], 0.0);
  EXPECT_GT(on_sampled.force[1], 0.0);
  EXPECT_LT(on_sampled.force[2], 0.0);
}

TEST(ParticlesDirectForce, TheSymmetricClusterCancelsAndCountsThirtyThreeCandidates) {
  Rig rig(0);
  for (const Point& offset : evidence::symmetric_offsets()) {
    rig.place_near(4, offset);
  }
  const Force f = direct_force(rig.positions, rig.sampled[4]);
  EXPECT_EQ(f.candidates, 33U);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_LT(std::fabs(f.force[c]), 1e-15) << "component " << c;
    EXPECT_GT(f.abs_sum[c], 1.0) << "component " << c;
  }
}

TEST(ParticlesToleranceFor, IsTheSpecificationsFunctionWithTheSlackAndTheFloor) {
  EXPECT_EQ(tolerance_for(0, 0.0), spec::kMinTolerance);
  EXPECT_EQ(tolerance_for(0, 123.0), spec::kMinTolerance) << "no candidates, nothing to sum";
  for (const std::size_t candidates : {std::size_t{1}, std::size_t{33}, std::size_t{4095}}) {
    for (const double sum : {0.0, 1.0, 15.0}) {
      EXPECT_EQ(tolerance_for(candidates, sum),
                spec::force_tolerance(candidates, sum) * (1.0 + spec::kEvaluationSlack));
    }
  }
  EXPECT_GT(tolerance_for(1, 0.0), 7e-15);
  EXPECT_LT(tolerance_for(1, 0.0), 8e-15);
  EXPECT_GT(tolerance_for(33, 15.0), tolerance_for(33, 1.0));
  EXPECT_GT(tolerance_for(33, 1.0), tolerance_for(1, 1.0));
}

TEST(ParticlesCreate, GivesTheOracleForEveryCellCount) {
  for (const std::uint32_t cells : spec::kCellsPerCutoff) {
    const workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 5, cells, 0, 0};
    auto created = create(workloads::WorkloadConfig{}, plan);
    ASSERT_TRUE(created.has_value());
    const auto oracle = std::move(created).value();
    EXPECT_EQ(oracle->workload(), "particles");
    EXPECT_EQ(oracle->contract(), Contract::kBounded);
  }
}

TEST(ParticlesCreate, RefusesAPlanThatIsNotAUnitOfTheSpecification) {
  const auto refused = [](const workloads::Plan& plan) {
    auto created = create(workloads::WorkloadConfig{}, plan);
    EXPECT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(created.error().detail.empty());
  };
  const workloads::Plan good{spec::kInputBytes, spec::kOutputBytes, 0, 1, 2, 0, 0};
  workloads::Plan plan = good;
  plan.input_bytes = spec::kInputBytes - 8;
  refused(plan);
  plan = good;
  plan.input_bytes = spec::kInputBytes + 8;
  refused(plan);
  plan = good;
  plan.output_bytes = spec::kOutputBytes - 8;
  refused(plan);
  plan = good;
  plan.output_bytes = 0;
  refused(plan);
  for (const std::uint32_t cells : {0U, 3U, 5U, 8U}) {
    plan = good;
    plan.decomposition = cells;
    refused(plan);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::particles
