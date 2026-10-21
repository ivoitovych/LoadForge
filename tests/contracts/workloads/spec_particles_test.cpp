// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the particles specification carries.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "workloads/particles/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

constexpr double kU = particles::kUnitRoundoff;

TEST(ParticlesSpecDefinition, TheNameIsAValidWorkloadNameAndTheSizesAreTheUnitsLayout) {
  EXPECT_EQ(particles::kName, "particles");
  EXPECT_TRUE(is_valid_name(particles::kName));
  EXPECT_EQ(particles::kVersion, 1U);
  EXPECT_EQ(particles::kParticles, 4096U);
  EXPECT_EQ(particles::kBoxEdge, 16.0);
  EXPECT_EQ(particles::kCutoffSquared, 4.0);
  EXPECT_EQ(particles::kInputBytes, 3U * 4096U * 8U);
  EXPECT_EQ(particles::kOutputBytes, 98304U);
  EXPECT_EQ(particles::kInputBytes, particles::kOutputBytes);
}

TEST(ParticlesSpecDefinition, CoordinatesAreExactMultiplesOfTwoToMinus49InTheHalfOpenBox) {
  using particles::position_value;
  EXPECT_EQ(position_value(0), 0.0) << "the lower end is reached";
  EXPECT_EQ(position_value(std::uint64_t{1} << 63U), 8.0);
  EXPECT_EQ(position_value(~std::uint64_t{0}), 16.0 - std::ldexp(1.0, -49));
  EXPECT_LT(position_value(~std::uint64_t{0}), particles::kBoxEdge) << "the upper end is not";
  EXPECT_EQ(position_value(std::uint64_t{1} << 11U), std::ldexp(1.0, -49));
  EXPECT_EQ(position_value(0x7FFU), 0.0) << "the low 11 bits are discarded";
}

TEST(ParticlesSpecDefinition, CellsPerCutoffAreOneTwoAndFourAndTheDefaultIsOne) {
  for (const std::uint32_t cells : {1U, 2U, 4U}) {
    EXPECT_TRUE(particles::is_cells_per_cutoff(cells)) << cells;
  }
  for (const std::uint32_t cells : {0U, 3U, 8U, 16U}) {
    EXPECT_FALSE(particles::is_cells_per_cutoff(cells)) << cells;
  }
  EXPECT_TRUE(particles::is_cells_per_cutoff(particles::kDefaultCellsPerCutoff));
  EXPECT_EQ(particles::kDefaultCellsPerCutoff, 2U);
}

TEST(ParticlesSpecDefinition, TheSampleIsSixteenParticlesOfThreeComponents) {
  EXPECT_EQ(particles::kSampledParticles, 16U);
  EXPECT_EQ(particles::kEntriesChecked, 48U);
  EXPECT_LE(particles::kSampledParticles, particles::kParticles);
}

TEST(ParticlesSpecDefinition, GammaIsKuOverOneMinusKu) {
  EXPECT_EQ(particles::kUnitRoundoff, std::ldexp(1.0, -53));
  EXPECT_EQ(particles::gamma(0), 0.0);
  const double ku = 7.0 * kU;
  EXPECT_EQ(particles::gamma(7), ku / (1.0 - ku));
  EXPECT_GT(particles::gamma(7), 7.0 * kU);
}

TEST(ParticlesSpecDefinition, TheFloorTheSlackAndTheCandidateLimitArePinned) {
  EXPECT_EQ(particles::kMinTolerance, std::numeric_limits<double>::min());
  EXPECT_EQ(particles::kMinTolerance, std::ldexp(1.0, -1022));
  EXPECT_EQ(particles::kEvaluationSlack, std::ldexp(1.0, -36));
  EXPECT_EQ(particles::kCandidateLimit, 4.0 + std::ldexp(1.0, -38)) << "4 (1 + 2^-40), exact";
}

TEST(ParticlesSpecDefinition, TheTermErrorIsAboutThirtyFourUnitRoundoffs) {
  const double e = particles::term_error();
  EXPECT_NEAR(e, 3.77475828e-15, 1e-22);
  EXPECT_GT(e, 33.0 * kU);
  EXPECT_LT(e, 35.0 * kU);
  const double d_max = 2.0 * (1.0 + particles::gamma(6));
  const double w_max = (1.0 + particles::gamma(7)) * (1.0 + particles::gamma(7)) * (1.0 + kU);
  const double eps_w =
      (2.0 * particles::gamma(7) + kU) * (1.0 + particles::gamma(7)) * (1.0 + particles::gamma(7));
  EXPECT_EQ(e, d_max * (kU * (1.0 + w_max) + eps_w)) << "derivation 1(f)";
}

TEST(ParticlesSpecDefinition, TheForceToleranceIsTwoTimesTermsAndSummationError) {
  const double e = particles::term_error();
  EXPECT_NEAR(particles::force_tolerance(1, 0.0), 2.0 * e, 1e-17) << "one term, nothing summed";
  EXPECT_NEAR(particles::force_tolerance(33, 15.0), 3.59046126e-13, 1e-20);
  const double g = particles::gamma(33);
  EXPECT_EQ(particles::force_tolerance(33, 15.0),
            2.0 * (33.0 * e + g * (15.0 / (1.0 - g) + 2.0 * 33.0 * e)));
  EXPECT_GT(particles::force_tolerance(34, 15.0), particles::force_tolerance(33, 15.0));
  EXPECT_GT(particles::force_tolerance(33, 16.0), particles::force_tolerance(33, 15.0));
}

}  // namespace
}  // namespace loadforge::workloads
