// SPDX-License-Identifier: GPL-3.0-or-later
//
// The twiddle table (spec.hpp, "The twiddle table"): the pinned entries, the
// exact images, and the whole table against a file computed directly, entry by
// entry, with 120-digit arithmetic and no symmetry (tests/fixtures/fft/).

#include "workloads/fft/twiddle.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

TEST(FftTwiddle, ThePinnedEntriesAreWhereLibmTablesGoWrong) {
  EXPECT_EQ(bits(kTwiddles[0].re), bits(1.0));
  EXPECT_EQ(bits(kTwiddles[0].im), bits(0.0)) << "+0, not -0";
  EXPECT_EQ(bits(kTwiddles[kPoints / 4].re), bits(0.0)) << "cos(pi/2) is exactly 0 here";
  EXPECT_EQ(bits(kTwiddles[kPoints / 4].im), bits(-1.0));
  EXPECT_EQ(bits(kTwiddles[kPoints / 8].re), kSqrtHalfBits);
  EXPECT_EQ(bits(kTwiddles[kPoints / 8].im), kSqrtHalfBits | (std::uint64_t{1} << 63U));
  EXPECT_EQ(kTwiddles[kPoints / 8].re, kSqrtHalf);
  EXPECT_EQ(bits(kTwiddles[kPoints / 2].re), bits(-1.0));
  EXPECT_EQ(bits(kTwiddles[kPoints / 2].im), bits(0.0)) << "sin(pi) is exactly 0 here";
  EXPECT_EQ(bits(kTwiddles[3 * kPoints / 4].re), bits(0.0));
  EXPECT_EQ(bits(kTwiddles[3 * kPoints / 4].im), bits(1.0));
}

TEST(FftTwiddle, NoEntryHasANegativeZero) {
  for (std::size_t m = 0; m < kPoints; ++m) {
    for (const double part : {kTwiddles[m].re, kTwiddles[m].im}) {
      if (part == 0.0) {
        EXPECT_EQ(bits(part), bits(0.0)) << "m = " << m;
      }
    }
  }
}

TEST(FftTwiddle, TheOnlyZeroPartsAreTheFourAxes) {
  std::size_t zeros = 0;
  for (std::size_t m = 0; m < kPoints; ++m) {
    zeros += static_cast<std::size_t>(kTwiddles[m].re == 0.0) +
             static_cast<std::size_t>(kTwiddles[m].im == 0.0);
  }
  EXPECT_EQ(zeros, 4U) << "cos zero at n/4 and 3n/4, sin zero at 0 and n/2";
}

TEST(FftTwiddle, EveryEntryIsAnExactImageOfTheOnesNamedInTheSpecification) {
  for (std::size_t m = 0; m < kPoints / 2; ++m) {
    // W[m + n/2] = -W[m].
    EXPECT_EQ(kTwiddles[m + kPoints / 2].re, -kTwiddles[m].re) << m;
    EXPECT_EQ(kTwiddles[m + kPoints / 2].im, -kTwiddles[m].im) << m;
  }
  for (std::size_t m = 0; m < kPoints / 4; ++m) {
    // W[m + n/4] = -i W[m]: swap the parts, negate one.
    EXPECT_EQ(kTwiddles[m + kPoints / 4].re, kTwiddles[m].im) << m;
    EXPECT_EQ(kTwiddles[m + kPoints / 4].im, -kTwiddles[m].re) << m;
  }
  for (std::size_t m = 0; m <= kPoints / 4; ++m) {
    // W[n/4 - m] = (-Im W[m], -Re W[m]).
    EXPECT_EQ(kTwiddles[kPoints / 4 - m].re, -kTwiddles[m].im) << m;
    EXPECT_EQ(kTwiddles[kPoints / 4 - m].im, -kTwiddles[m].re) << m;
  }
}

TEST(FftTwiddle, EachEntryIsOnTheUnitCircleWithinTheDocumentedBound) {
  for (std::size_t m = 0; m < kPoints; ++m) {
    const long double re = kTwiddles[m].re;
    const long double im = kTwiddles[m].im;
    EXPECT_LE(std::fabs(std::sqrt(re * re + im * im) - 1.0L), 3.0L * kUnitRoundoff) << m;
    EXPECT_LE(std::sqrt(re * re + im * im), 1.0L + static_cast<long double>(kUnitRoundoff)) << m;
  }
}

TEST(FftTwiddle, EveryEntryIsWithinHalfAnUlpOfTheTrueValueInLongDouble) {
  const long double pi = std::acos(-1.0L);
  for (std::size_t m = 0; m < kPoints; ++m) {
    const long double angle = 2.0L * pi * static_cast<long double>(m) / kPoints;
    const long double cosine = std::cos(angle);
    const long double sine = -std::sin(angle);
    // Half an ulp of the double nearest each part, with room for the error of the
    // long double pi (under 4e-19 of absolute error near a zero of cos or sin). The
    // file test below is the strict one.
    for (const auto& [got, want] : {std::pair<double, long double>{kTwiddles[m].re, cosine},
                                    std::pair<double, long double>{kTwiddles[m].im, sine}}) {
      if (got == 0.0) {
        continue;  // the exact zeros are pinned above; the long double's own pi is not exact
      }
      const double ulp = std::nextafter(std::fabs(got), 2.0) - std::fabs(got);
      EXPECT_LE(std::fabs(static_cast<long double>(got) - want), 0.5L * ulp * 1.0001L + 4e-19L)
          << "m = " << m;
    }
  }
}

TEST(FftTwiddle, TheTableMatchesTheFileComputedEntryByEntryWithNoSymmetry) {
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) + "/tests/fixtures/fft/twiddles_full.txt");
  ASSERT_TRUE(file.is_open());
  std::string line;
  std::size_t seen = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    std::size_t m = 0;
    std::string re_hex;
    std::string im_hex;
    fields >> m >> re_hex >> im_hex;
    ASSERT_EQ(m, seen) << line;
    ASSERT_LT(m, kPoints);
    EXPECT_EQ(bits(twiddle(m).re), std::stoull(re_hex, nullptr, 16)) << "Re W[" << m << "]";
    EXPECT_EQ(bits(twiddle(m).im), std::stoull(im_hex, nullptr, 16)) << "Im W[" << m << "]";
    EXPECT_EQ(bits(kTwiddles[m].re), bits(twiddle(m).re));
    EXPECT_EQ(bits(kTwiddles[m].im), bits(twiddle(m).im));
    ++seen;
  }
  EXPECT_EQ(seen, kPoints);
}

TEST(FftTwiddle, TheOctantTableHoldsTheBoundariesItShould) {
  EXPECT_EQ(kOctant.size(), kPoints / 8 + 1);
  EXPECT_EQ(kOctant.front().cos_bits, bits(1.0));
  EXPECT_EQ(kOctant.front().sin_bits, 0U);
  EXPECT_EQ(kOctant.back().cos_bits, kSqrtHalfBits);
  EXPECT_EQ(kOctant.back().sin_bits, kSqrtHalfBits);
}

}  // namespace
}  // namespace loadforge::workloads::fft
