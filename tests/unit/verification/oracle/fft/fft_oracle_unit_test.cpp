// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the FFT oracle: its twiddle table, its sampling, its direct DFT, its
// tolerance functions and the plans it refuses, each on its own.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <set>
#include <vector>

#include "oracle/verification/oracle/fft/fft_evidence.hpp"
#include "verification/oracle/fft/fft_oracle.hpp"
#include "verification/oracle/fft/twiddle_table.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::verification::oracle::fft {
namespace {

namespace spec = workloads::fft;

constexpr std::size_t kQuarter = spec::kPoints / 4;

std::uint64_t bits_of(double value) { return std::bit_cast<std::uint64_t>(value); }

TEST(FftTwiddleTable, PinsTheEntriesWhereALibmTableGoesWrong) {
  const Complex first = twiddle(0);
  EXPECT_EQ(bits_of(first.re), bits_of(1.0));
  EXPECT_EQ(bits_of(first.im), bits_of(0.0)) << "+0, not -0";

  const Complex quarter = twiddle(kQuarter);
  EXPECT_EQ(bits_of(quarter.re), bits_of(0.0)) << "cos(pi/2) is exactly 0 here, not 6.1e-17";
  EXPECT_EQ(bits_of(quarter.im), bits_of(-1.0));

  const Complex eighth = twiddle(spec::kPoints / 8);
  EXPECT_EQ(bits_of(eighth.re), spec::kSqrtHalfBits);
  EXPECT_EQ(bits_of(eighth.im), bits_of(-spec::kSqrtHalf));

  const Complex half = twiddle(spec::kPoints / 2);
  EXPECT_EQ(bits_of(half.re), bits_of(-1.0));
  EXPECT_EQ(bits_of(half.im), bits_of(0.0));

  const Complex three_quarters = twiddle(3 * kQuarter);
  EXPECT_EQ(bits_of(three_quarters.re), bits_of(0.0));
  EXPECT_EQ(bits_of(three_quarters.im), bits_of(1.0));
}

TEST(FftTwiddleTable, HoldsTheExactImagesTheSpecificationNames) {
  for (std::size_t m = 0; m < spec::kPoints / 2; ++m) {
    const Complex w = twiddle(m);
    const Complex opposite = twiddle(m + spec::kPoints / 2);
    EXPECT_EQ(opposite.re, -w.re) << "W[m + n/2] = -W[m], m = " << m;
    EXPECT_EQ(opposite.im, -w.im) << "m = " << m;
  }
  for (std::size_t m = 0; m < spec::kPoints - kQuarter; ++m) {
    const Complex w = twiddle(m);
    const Complex turned = twiddle(m + kQuarter);
    EXPECT_EQ(turned.re, w.im) << "W[m + n/4] = -i W[m], m = " << m;
    EXPECT_EQ(turned.im, -w.re) << "m = " << m;
  }
  for (std::size_t m = 0; m <= kQuarter; ++m) {
    const Complex w = twiddle(m);
    const Complex reflected = twiddle(kQuarter - m);
    EXPECT_EQ(reflected.re, -w.im) << "W[n/4 - m] = (-Im W[m], -Re W[m]), m = " << m;
    EXPECT_EQ(reflected.im, -w.re) << "m = " << m;
  }
}

TEST(FftTwiddleTable, NoPartIsANegativeZero) {
  for (const std::uint64_t word : kTwiddleBits) {
    EXPECT_NE(word, bits_of(-0.0));
  }
}

// The table is the double nearest the real number. The C library's long double
// sine and cosine are an independent evaluation of that number; used on an
// angle in the first octant, where neither a sine nor a cosine is near a zero
// they could misplace, they are good to about 2^-11 of a double's spacing. A
// table entry is accepted when it is within half a spacing of the true value
// with that slack, so a wrong rounding that matters is caught and one whose
// true value lies within a hair of a midpoint is not claimed to be.
struct LongDoublePair {
  long double cosine;
  long double sine;
};

// cos and sin of 2 pi m / n by reduction to [0, pi/4], which is exact integer
// work and keeps the library's argument small.
LongDoublePair cosine_and_sine(std::size_t m) {
  constexpr long double kTau = 2.0L * std::numbers::pi_v<long double>;
  const std::size_t quadrant = m / kQuarter;
  const std::size_t r = m % kQuarter;
  const bool reflected = r > kQuarter / 2;
  const std::size_t base = reflected ? kQuarter - r : r;
  const long double angle = kTau * static_cast<long double>(base) / spec::kPoints;
  long double c = base == 0 ? 1.0L : std::cos(angle);
  long double s = base == 0 ? 0.0L : std::sin(angle);
  if (reflected) {
    std::swap(c, s);
  }
  switch (quadrant) {
    case 0:
      return {c, s};
    case 1:
      return {-s, c};
    case 2:
      return {-c, -s};
    default:
      return {s, -c};
  }
}

TEST(FftTwiddleTable, EveryEntryIsWithinHalfASpacingOfTheTrueValue) {
  for (std::size_t m = 0; m < spec::kPoints; ++m) {
    const LongDoublePair truth = cosine_and_sine(m);
    const std::array<long double, 2> expected = {truth.cosine, -truth.sine};
    const Complex w = twiddle(m);
    const std::array<double, 2> parts = {w.re, w.im};
    for (std::size_t part = 0; part < 2; ++part) {
      const double magnitude = std::fabs(parts[part]);
      const double spacing = std::nextafter(magnitude, 2.0) - magnitude;
      const long double distance =
          std::fabs(static_cast<long double>(parts[part]) - expected[part]);
      EXPECT_LE(static_cast<double>(distance), 0.5 * spacing * 1.01)
          << "m = " << m << (part == 0 ? " real" : " imaginary");
    }
  }
}

TEST(FftLoadDouble, ReadsLittleEndianWhateverTheHostIs) {
  const std::vector<std::byte> bytes = {
      std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0xF0}, std::byte{0x3F}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0}};
  EXPECT_EQ(load_double(bytes, 0), 1.0);
  EXPECT_EQ(bits_of(load_double(bytes, 1)), 1U) << "the smallest subnormal: byte 0 is the low byte";
}

TEST(FftSampledBins, AreSortedDistinctPinnedAndAFunctionOfTheIndexAlone) {
  std::set<std::vector<std::uint32_t>> distinct_sets;
  for (std::uint64_t index = 0; index < 2000; ++index) {
    const auto bins = sampled_bins(index);
    EXPECT_TRUE(std::is_sorted(bins.begin(), bins.end())) << "index " << index;
    EXPECT_EQ(std::adjacent_find(bins.begin(), bins.end()), bins.end()) << "index " << index;
    EXPECT_EQ(bins.front(), 0U);
    EXPECT_TRUE(std::binary_search(bins.begin(), bins.end(), spec::kPoints / 2));
    EXPECT_LT(bins.back(), spec::kPoints);
    EXPECT_EQ(bins, sampled_bins(index)) << "a re-run checks the same bins";
    distinct_sets.insert(std::vector<std::uint32_t>(bins.begin(), bins.end()));
  }
  EXPECT_GT(distinct_sets.size(), 1990U) << "units are spread over the bins, not all alike";
}

TEST(FftSampledBins, TheLargestIndexIsSampledToo) {
  const auto bins = sampled_bins(~std::uint64_t{0});
  EXPECT_EQ(bins.size(), spec::kSampledBins);
  EXPECT_EQ(std::adjacent_find(bins.begin(), bins.end()), bins.end());
}

TEST(FftDirectDft, OfAnImpulseIsATwiddleFactor) {
  std::vector<double> x(2 * spec::kPoints, 0.0);
  x[6] = 1.0;  // x_3 = 1: the real part is element 2 * 3
  for (const std::size_t k :
       {std::size_t{0}, std::size_t{1}, std::size_t{700}, std::size_t{4095}}) {
    const Complex bin = direct_dft_bin(x, k);
    const Complex w = twiddle((3 * k) % spec::kPoints);
    EXPECT_EQ(bin.re, w.re) << "k = " << k;
    EXPECT_EQ(bin.im, w.im) << "k = " << k;
  }
}

TEST(FftDirectDft, OfAToneIsAllInOneBin) {
  // x_j = conj(w^{3j}): then X_k = n for k = 3 and 0 elsewhere.
  std::vector<double> x(2 * spec::kPoints, 0.0);
  for (std::size_t j = 0; j < spec::kPoints; ++j) {
    const Complex w = twiddle((3 * j) % spec::kPoints);
    x[2 * j] = w.re;
    x[2 * j + 1] = -w.im;
  }
  const Complex at_tone = direct_dft_bin(x, 3);
  EXPECT_NEAR(at_tone.re, 4096.0, 1e-9);
  EXPECT_NEAR(at_tone.im, 0.0, 1e-9);
  for (const std::size_t k : {std::size_t{0}, std::size_t{2}, std::size_t{4}, std::size_t{2048}}) {
    const Complex elsewhere = direct_dft_bin(x, k);
    EXPECT_NEAR(elsewhere.re, 0.0, 1e-9) << "k = " << k;
    EXPECT_NEAR(elsewhere.im, 0.0, 1e-9) << "k = " << k;
  }
}

TEST(FftTolerances, AreTheSpecificationsFactorsTimesRootNTimesTheNormWithTheSlack) {
  for (const std::uint32_t radix : spec::kRadices) {
    for (const double norm : {1.0, 0.25, 37.5}) {
      EXPECT_EQ(bin_tolerance(radix, norm),
                64.0 * norm * spec::bin_tolerance_factor(radix) * (1.0 + spec::kEvaluationSlack));
      EXPECT_EQ(
          parseval_tolerance(radix, norm),
          64.0 * norm * spec::parseval_tolerance_factor(radix) * (1.0 + spec::kEvaluationSlack));
      EXPECT_GT(bin_tolerance(radix, norm), 0.0);
      EXPECT_TRUE(std::isfinite(parseval_tolerance(radix, norm)));
    }
  }
  EXPECT_GT(bin_tolerance(4, 1.0), bin_tolerance(2, 1.0)) << "more stages' error, a larger bound";
}

TEST(FftCreate, GivesTheOracleForAPlanOfTheSpecification) {
  for (const std::uint32_t radix : spec::kRadices) {
    const workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 5, radix, 0, 0};
    auto created = create(workloads::WorkloadConfig{}, plan);
    ASSERT_TRUE(created.has_value());
    const auto oracle = std::move(created).value();
    EXPECT_EQ(oracle->workload(), "fft");
    EXPECT_EQ(oracle->contract(), Contract::kBounded);
  }
}

TEST(FftCreate, RefusesAPlanThatIsNotAUnitOfTheSpecification) {
  const auto refused = [](workloads::Plan plan) {
    auto created = create(workloads::WorkloadConfig{}, plan);
    EXPECT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(created.error().detail.empty());
  };
  const workloads::Plan good{spec::kInputBytes, spec::kOutputBytes, 0, 1, 4, 0, 0};
  workloads::Plan plan = good;
  plan.input_bytes = spec::kInputBytes - 1;
  refused(plan);
  plan = good;
  plan.input_bytes = spec::kInputBytes + 1;
  refused(plan);
  plan = good;
  plan.output_bytes = spec::kOutputBytes - 1;
  refused(plan);
  plan = good;
  plan.output_bytes = 0;
  refused(plan);
  for (const std::uint32_t radix : {0U, 1U, 3U, 8U, 16U}) {
    plan = good;
    plan.decomposition = radix;
    refused(plan);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::fft
