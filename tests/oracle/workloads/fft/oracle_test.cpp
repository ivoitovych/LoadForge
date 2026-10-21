// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the workload against a direct O(n^2) DFT in long double, at every bin of
// whole units; against Parseval; against its own inverse (a round trip, which
// the specification leaves to this stream); and the two radices against each
// other. Every bound is the specification's, derived and not chosen.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "oracle/workloads/fft/fft_support.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

using testing::Bytes;

struct Case {
  std::uint32_t radix;
  std::uint64_t seed;
  std::uint64_t unit;
};

std::unique_ptr<Workload> made(std::uint32_t radix, std::uint64_t seed) {
  auto created =
      create(WorkloadConfig{seed, 8 * (kInputBytes + kOutputBytes), radix, simd::IsaPath::kScalar});
  EXPECT_TRUE(created.has_value());
  return std::move(created).value();
}

TEST(FftOracle, EveryBinOfWholeUnitsIsWithinTheDerivedBoundOfTheDirectDft) {
  testing::Threads serial(1);
  for (const Case c : {Case{2, 1, 0}, Case{4, 1, 0}, Case{2, 99, 17}, Case{4, 99, 17},
                       Case{4, 0, ~std::uint64_t{0}}}) {
    const auto workload = made(c.radix, c.seed);
    Bytes input(kInputBytes);
    workload->generate(c.unit, input);
    const std::vector<double> x = testing::to_doubles(input);
    const std::vector<double> y =
        testing::to_doubles(testing::compute_unit(*workload, c.unit, input, serial));
    const auto want = testing::direct_dft(x);
    const double tolerance = 64.0 * testing::norm2(x) * bin_tolerance_factor(c.radix);
    double worst = 0.0;
    for (std::size_t k = 0; k < kPoints; ++k) {
      worst = std::max({worst, static_cast<double>(std::fabs(y[2 * k] - want[k].real())),
                        static_cast<double>(std::fabs(y[2 * k + 1] - want[k].imag()))});
    }
    EXPECT_LE(worst, tolerance) << "radix " << c.radix << " seed " << c.seed;
    EXPECT_GT(worst, 0.0) << "an exactly right answer would mean the test compares a copy";
    EXPECT_LT(worst, tolerance / 20.0)
        << "the bound is a worst case; real error sits far below it (radix " << c.radix << ")";
  }
}

TEST(FftOracle, TheSpecificationsOwnCheckAgreesOnEveryUnitOfBothRadices) {
  testing::Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    const auto workload = made(radix, 2024);
    for (std::uint64_t unit = 0; unit < 12; ++unit) {
      Bytes input(kInputBytes);
      workload->generate(unit, input);
      const testing::Verdict verdict = testing::check_unit(
          radix, unit, input, testing::compute_unit(*workload, unit, input, serial));
      EXPECT_EQ(verdict.kind, testing::Kind::kAgreement)
          << "radix " << radix << " unit " << unit << " " << verdict.check << " residual "
          << verdict.residual << " tolerance " << verdict.tolerance;
      EXPECT_EQ(verdict.blocks_compared, kChecks);
    }
  }
}

TEST(FftOracle, ParsevalHoldsFarInsideItsBoundOverManySeeds) {
  testing::Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    double worst_ratio = 0.0;
    for (std::uint64_t seed = 0; seed < 40; ++seed) {
      const auto workload = made(radix, seed);
      Bytes input(kInputBytes);
      workload->generate(seed, input);
      const double a = testing::norm2(testing::to_doubles(input));
      const double s = testing::norm2(
          testing::to_doubles(testing::compute_unit(*workload, seed, input, serial)));
      const double tolerance = 64.0 * a * parseval_tolerance_factor(radix);
      worst_ratio = std::max(worst_ratio, std::fabs(s - 64.0 * a) / tolerance);
    }
    EXPECT_LT(worst_ratio, 0.01) << "radix " << radix;
  }
}

TEST(FftOracle, TheInverseRecoversTheInputToTheDerivedRoundTripBound) {
  // The inverse by the conjugation identity: conj(F(conj(X))) / n = x. It uses
  // the forward workload and nothing else, so it needs no second algorithm.
  testing::Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    const auto workload = made(radix, 5);
    Bytes input(kInputBytes);
    workload->generate(3, input);
    const std::vector<double> x = testing::to_doubles(input);
    std::vector<double> conjugated =
        testing::to_doubles(testing::compute_unit(*workload, 3, input, serial));
    for (std::size_t j = 0; j < kPoints; ++j) {
      conjugated[2 * j + 1] = -conjugated[2 * j + 1];
    }
    const std::vector<double> back = testing::to_doubles(
        testing::compute_unit(*workload, 3, testing::to_bytes(conjugated), serial));
    double error = 0.0;
    for (std::size_t j = 0; j < kPoints; ++j) {
      const double re = back[2 * j] / static_cast<double>(kPoints);
      const double im = -back[2 * j + 1] / static_cast<double>(kPoints);
      error += (re - x[2 * j]) * (re - x[2 * j]) + (im - x[2 * j + 1]) * (im - x[2 * j + 1]);
    }
    // Two transforms, each within rho of exact in the 2-norm, the second of a
    // vector whose norm is already (1 + rho) times its exact value.
    const double rho = forward_error_factor(radix);
    EXPECT_LE(std::sqrt(error), (2.0 * rho + rho * rho) * testing::norm2(x)) << "radix " << radix;
    EXPECT_GT(error, 0.0);
  }
}

TEST(FftOracle, TheTwoRadicesAgreeWithinTwiceTheBinBoundAndNotBitForBit) {
  testing::Threads serial(1);
  const auto two = made(2, 8);
  const auto four = made(4, 8);
  Bytes input(kInputBytes);
  two->generate(4, input);
  const Bytes a = testing::compute_unit(*two, 4, input, serial);
  const Bytes b = testing::compute_unit(*four, 4, input, serial);
  EXPECT_NE(a, b) << "different decompositions round differently: that is the declared class";
  const std::vector<double> ya = testing::to_doubles(a);
  const std::vector<double> yb = testing::to_doubles(b);
  const double bound = 64.0 * testing::norm2(testing::to_doubles(input)) *
                       (bin_tolerance_factor(2) + bin_tolerance_factor(4));
  for (std::size_t e = 0; e < ya.size(); ++e) {
    ASSERT_LE(std::fabs(ya[e] - yb[e]), bound) << e;
  }
}

}  // namespace
}  // namespace loadforge::workloads::fft
