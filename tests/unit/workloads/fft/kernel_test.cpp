// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stage kernels and the transform around them, at every length a radix
// supports, against an independent direct DFT in long double (T3 at the small
// sizes the direct DFT is cheap at), and the stage structure: how many stages,
// how many tasks, and that nothing in the split depends on the executor.

#include "workloads/fft/kernel.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "oracle/workloads/fft/fft_support.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

using testing::Bytes;
using testing::Threads;

std::vector<double> sample(std::size_t n, std::uint64_t stream) {
  std::vector<double> x(2 * n);
  for (std::size_t e = 0; e < x.size(); ++e) {
    x[e] = entry_value(rng::u64_of(rng::block_at(0xF00D, stream, e)));
  }
  return x;
}

Bytes run_transform(std::uint32_t radix, std::size_t n, const std::vector<double>& x,
                    worker::Executor& executor) {
  const Bytes in = testing::to_bytes(x);
  Bytes out(n * kElementBytes, std::byte{0xAA});
  Bytes scratch(n * kElementBytes, std::byte{0x55});
  transform(stage_kernels(radix).scalar, radix, n, in, out, scratch, executor);
  return out;
}

TEST(FftKernel, DoublesAreLittleEndianWhateverTheMachine) {
  Bytes bytes(16);
  store_double(bytes, 0, 1.0);
  store_double(bytes, 1, -2.0);
  const Bytes expected = {std::byte{0}, std::byte{0}, std::byte{0},    std::byte{0},
                          std::byte{0}, std::byte{0}, std::byte{0xF0}, std::byte{0x3F},
                          std::byte{0}, std::byte{0}, std::byte{0},    std::byte{0},
                          std::byte{0}, std::byte{0}, std::byte{0},    std::byte{0xC0}};
  EXPECT_EQ(bytes, expected);
  EXPECT_EQ(load_double(bytes, 0), 1.0);
  EXPECT_EQ(load_double(bytes, 1), -2.0);
}

TEST(FftKernel, EveryBitPatternRoundTripsThroughLoadAndStore) {
  const std::vector<std::uint64_t> patterns = {0,
                                               std::uint64_t{1} << 63U,
                                               1,
                                               0x7FF0000000000000ULL,
                                               0xFFF0000000000000ULL,
                                               0x7FF8000000000001ULL,
                                               0xFFFFFFFFFFFFFFFFULL,
                                               0x000FFFFFFFFFFFFFULL,
                                               0x0123456789ABCDEFULL};
  for (const std::uint64_t pattern : patterns) {
    Bytes bytes(8);
    store_double(bytes, 0, std::bit_cast<double>(pattern));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(load_double(bytes, 0)), pattern);
  }
}

TEST(FftKernel, TheTableOffersExactlyTheScalarKernelForEachRadix) {
  for (const std::uint32_t radix : kRadices) {
    const auto table = stage_kernels(radix);
    EXPECT_NE(table.scalar, nullptr);
    EXPECT_EQ(table.avx2, nullptr);
    EXPECT_EQ(table.avx512, nullptr);
    EXPECT_EQ(table.neon, nullptr);
    EXPECT_EQ(simd::kernel_paths(table), simd::IsaSet{simd::IsaPath::kScalar});
    EXPECT_EQ(simd::kernel_at(table, simd::IsaPath::kScalar), table.scalar);
    EXPECT_EQ(simd::kernel_at(table, static_cast<simd::IsaPath>(200)), nullptr)
        << "a value outside the enumeration has no kernel";
  }
  EXPECT_NE(stage_kernels(2).scalar, stage_kernels(4).scalar);
}

TEST(FftKernel, EveryLengthOfEveryRadixMatchesTheDirectDftWithinTheDerivedBound) {
  Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    for (std::size_t n = radix; n <= kPoints; n *= radix) {
      if (n > 1024) {
        continue;  // the full size has its own tests; the direct DFT is quadratic
      }
      const std::vector<double> x = sample(n, n);
      const auto want = testing::direct_dft(x);
      const std::vector<double> got = testing::to_doubles(run_transform(radix, n, x, serial));
      const double bound =
          std::sqrt(static_cast<double>(n)) * testing::norm2(x) * bin_tolerance_factor(radix);
      for (std::size_t k = 0; k < n; ++k) {
        EXPECT_LE(std::fabs(static_cast<long double>(got[2 * k]) - want[k].real()), bound)
            << "radix " << radix << " n " << n << " k " << k;
        EXPECT_LE(std::fabs(static_cast<long double>(got[2 * k + 1]) - want[k].imag()), bound)
            << "radix " << radix << " n " << n << " k " << k;
      }
    }
  }
}

TEST(FftKernel, AnImpulseTransformsToTheRowOfTheTwiddleTable) {
  Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    for (const std::size_t position : {std::size_t{0}, std::size_t{1}, std::size_t{37}}) {
      std::vector<double> x(std::size_t{2} * 64, 0.0);
      x[2 * position] = 1.0;
      const std::vector<double> got = testing::to_doubles(run_transform(radix, 64, x, serial));
      for (std::size_t k = 0; k < 64; ++k) {
        const Complex w = kTwiddles[(position * k * (kPoints / 64)) % kPoints];
        EXPECT_NEAR(got[2 * k], w.re, 4e-16) << "radix " << radix << " k " << k;
        EXPECT_NEAR(got[2 * k + 1], w.im, 4e-16) << "radix " << radix << " k " << k;
      }
    }
  }
}

TEST(FftKernel, ConstantInputGivesOneBinAndZeroElsewhereExactly) {
  Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    std::vector<double> x(std::size_t{2} * 256, 0.0);
    for (std::size_t j = 0; j < 256; ++j) {
      x[2 * j] = 0.5;
    }
    const std::vector<double> got = testing::to_doubles(run_transform(radix, 256, x, serial));
    EXPECT_EQ(got[0], 128.0);
    EXPECT_EQ(got[1], 0.0);
    for (std::size_t k = 1; k < 256; ++k) {
      EXPECT_NEAR(got[2 * k], 0.0, 1e-13) << k;
      EXPECT_NEAR(got[2 * k + 1], 0.0, 1e-13) << k;
    }
  }
}

TEST(FftKernel, ATransformHasOneExecutorRunPerStageAndTasksIndependentOfThreads) {
  for (const std::uint32_t radix : kRadices) {
    const std::vector<double> x = sample(kPoints, 5);
    Threads one(1);
    Threads five(5);
    const Bytes a = run_transform(radix, kPoints, x, one);
    const Bytes b = run_transform(radix, kPoints, x, five);
    EXPECT_EQ(a, b);
    EXPECT_EQ(one.runs, stage_count(radix));
    EXPECT_EQ(five.runs, stage_count(radix));
    EXPECT_EQ(one.tasks, five.tasks) << "the split does not depend on the thread count";
    EXPECT_EQ(one.tasks, stage_count(radix) * (kPoints / radix / kButterfliesPerTask));
  }
}

TEST(FftKernel, ABoundaryOfTheLastTaskIsClampedToTheButterflyCount) {
  // 16 points of radix 2: 8 butterflies, fewer than one task's worth.
  Threads serial(1);
  const std::vector<double> x = sample(16, 1);
  (void)run_transform(2, 16, x, serial);
  EXPECT_EQ(serial.runs, 4U);
  EXPECT_EQ(serial.tasks, 4U) << "one task per stage";
}

TEST(FftKernel, NonFiniteInputPropagatesWithoutTrapOrUndefinedBehaviour) {
  Threads serial(1);
  for (const std::uint32_t radix : kRadices) {
    std::vector<double> x = sample(64, 2);
    x[10] = std::numeric_limits<double>::quiet_NaN();
    x[21] = std::numeric_limits<double>::infinity();
    const std::vector<double> got = testing::to_doubles(run_transform(radix, 64, x, serial));
    std::size_t nans = 0;
    for (const double value : got) {
      nans += static_cast<std::size_t>(std::isnan(value));
    }
    EXPECT_GT(nans, 0U);
  }
}

}  // namespace
}  // namespace loadforge::workloads::fft
