// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the FFT specification carries.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "workloads/fft/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

constexpr double kU = fft::kUnitRoundoff;

TEST(FftSpecDefinition, TheNameIsAValidWorkloadNameAndTheSizesAreTheUnitsLayout) {
  EXPECT_EQ(fft::kName, "fft");
  EXPECT_TRUE(is_valid_name(fft::kName));
  EXPECT_EQ(fft::kVersion, 1U);
  EXPECT_EQ(fft::kPoints, std::size_t{1} << 12U);
  EXPECT_EQ(fft::kInputBytes, 2U * 4096U * 8U);
  EXPECT_EQ(fft::kOutputBytes, 65536U);
  EXPECT_EQ(fft::kInputBytes, fft::kOutputBytes);
}

TEST(FftSpecDefinition, ComponentsAreExactMultiplesOfTwoToMinus52InTheHalfOpenInterval) {
  using fft::entry_value;
  EXPECT_EQ(entry_value(0), -1.0) << "the lower end is reached";
  EXPECT_EQ(entry_value(std::uint64_t{1} << 63U), 0.0);
  EXPECT_EQ(entry_value(~std::uint64_t{0}), 1.0 - 1.0 / 4503599627370496.0)
      << "the upper end, +1, is not";
  EXPECT_LT(entry_value(~std::uint64_t{0}), 1.0);
  EXPECT_EQ(entry_value(std::uint64_t{1} << 11U), -1.0 + 1.0 / 4503599627370496.0);
  EXPECT_EQ(entry_value(0x7FFU), -1.0) << "the low 11 bits are discarded";
}

TEST(FftSpecDefinition, TheRadicesAreTwoAndFourAndTheDefaultIsOne) {
  for (const std::uint32_t radix : {2U, 4U}) {
    EXPECT_TRUE(fft::is_radix(radix)) << radix;
  }
  for (const std::uint32_t radix : {0U, 1U, 3U, 8U, 16U, 4096U}) {
    EXPECT_FALSE(fft::is_radix(radix)) << radix;
  }
  EXPECT_TRUE(fft::is_radix(fft::kDefaultRadix));
  EXPECT_EQ(fft::kDefaultRadix, 4U);
}

TEST(FftSpecDefinition, StageCountIsTheExponentOfThePointCountInTheRadix) {
  EXPECT_EQ(fft::stage_count(2), 12U);
  EXPECT_EQ(fft::stage_count(4), 6U);
  EXPECT_EQ(fft::stage_count(16), 3U);
  EXPECT_EQ(fft::stage_count(64), 2U);
  EXPECT_EQ(fft::stage_count(4096), 1U);
  for (const std::uint32_t radix : fft::kRadices) {
    std::size_t reach = 1;
    for (std::uint32_t stage = 0; stage < fft::stage_count(radix); ++stage) {
      reach *= radix;
    }
    EXPECT_EQ(reach, fft::kPoints) << "every radix divides the point count into whole stages";
  }
}

TEST(FftSpecDefinition, GammaIsKuOverOneMinusKu) {
  EXPECT_EQ(fft::kUnitRoundoff, std::ldexp(1.0, -53));
  EXPECT_EQ(fft::gamma(0), 0.0);
  const double ku = 8192.0 * kU;
  EXPECT_EQ(fft::gamma(8192), ku / (1.0 - ku));
  EXPECT_GT(fft::gamma(8192), 8192.0 * kU);
  EXPECT_LT(fft::gamma(8193), std::ldexp(1.0, -39))
      << "derivation 4: gamma_{2n+1} < 2^-39, so three of them and a few u fit 2^-37";
}

TEST(FftSpecDefinition, TheSlackIsTwoToMinus36AndSqrtTwoIsBounded) {
  EXPECT_EQ(fft::kEvaluationSlack, std::ldexp(1.0, -36));
  EXPECT_GE(fft::kSqrtTwoBound * fft::kSqrtTwoBound, 2.0);
  EXPECT_LT(fft::kSqrtTwoBound, 1.5000001);
}

TEST(FftSpecDefinition, SqrtHalfIsTheDoubleNearestAndItsBitsArePinned) {
  EXPECT_EQ(std::bit_cast<std::uint64_t>(fft::kSqrtHalf), fft::kSqrtHalfBits);
  EXPECT_EQ(fft::kSqrtHalfBits, 0x3FE6A09E667F3BCDULL);
  EXPECT_EQ(fft::kSqrtHalf, std::sqrt(0.5)) << "sqrt is correctly rounded: the nearest double";
  EXPECT_NEAR(fft::kSqrtHalf * fft::kSqrtHalf, 0.5, 2.0 * kU);
}

TEST(FftSpecDefinition, TheStageErrorIsUPlusRGammaTwoRTimesOnePlusU) {
  for (const std::uint32_t radix : fft::kRadices) {
    EXPECT_EQ(fft::stage_error(radix),
              kU + static_cast<double>(radix) * fft::gamma(std::size_t{2} * radix) * (1.0 + kU))
        << radix;
  }
  EXPECT_NEAR(fft::stage_error(2), 9.99200722e-16, 1e-22);
  EXPECT_NEAR(fft::stage_error(4), 3.66373598e-15, 1e-21);
  EXPECT_GT(fft::stage_error(4), fft::stage_error(2));
}

TEST(FftSpecDefinition, TheForwardErrorFactorIsTEtaOverOneMinusTEta) {
  for (const std::uint32_t radix : fft::kRadices) {
    const double t_eta = static_cast<double>(fft::stage_count(radix)) * fft::stage_error(radix);
    EXPECT_EQ(fft::forward_error_factor(radix), t_eta / (1.0 - t_eta)) << radix;
    EXPECT_GT(fft::forward_error_factor(radix), t_eta);
    EXPECT_LT(fft::forward_error_factor(radix), t_eta * 1.000001);
  }
  EXPECT_NEAR(fft::forward_error_factor(2), 1.19904087e-14, 1e-21);
  EXPECT_NEAR(fft::forward_error_factor(4), 2.19824159e-14, 1e-21);
}

TEST(FftSpecDefinition, TheBinFactorAddsTheOraclesOwnDftErrorToTheTransformsRho) {
  for (const std::uint32_t radix : fft::kRadices) {
    const double own = 1.5 * fft::gamma(8192) * (1.0 + kU);
    EXPECT_EQ(fft::bin_tolerance_factor(radix), fft::forward_error_factor(radix) + kU + own)
        << radix;
  }
  EXPECT_NEAR(fft::bin_tolerance_factor(2), 1.37634348e-12, 1e-19);
  EXPECT_NEAR(fft::bin_tolerance_factor(4), 1.38633549e-12, 1e-19);
}

TEST(FftSpecDefinition, TheParsevalFactorIsRhoPlusGammaOfTwoNPlusOneTimesTwoPlusRho) {
  for (const std::uint32_t radix : fft::kRadices) {
    const double rho = fft::forward_error_factor(radix);
    EXPECT_EQ(fft::parseval_tolerance_factor(radix), rho + fft::gamma(8193) * (2.0 + rho)) << radix;
  }
  EXPECT_NEAR(fft::parseval_tolerance_factor(2), 1.83120186e-12, 1e-19);
  EXPECT_NEAR(fft::parseval_tolerance_factor(4), 1.84119386e-12, 1e-19);
}

TEST(FftSpecDefinition, TheChecksAreTheSampledBinsThenParseval) {
  EXPECT_EQ(fft::kSampledBins, 16U);
  EXPECT_EQ(fft::kChecks, fft::kSampledBins + 1U);
  EXPECT_LE(fft::kSampledBins, fft::kPoints);
}

}  // namespace
}  // namespace loadforge::workloads
