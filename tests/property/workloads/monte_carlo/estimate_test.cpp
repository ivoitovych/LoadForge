// SPDX-License-Identifier: GPL-3.0-or-later
// T4: invariants and relations the Monte Carlo workload keeps whatever the
// data -- what the numbers mean (spec.hpp, A UNIT), and how the batches of a
// unit relate to one another. The seeds are fixed, so a bound that holds here
// holds on every run; the bounds are statistical (several standard errors
// wide) because the specification derives the spread of a sum and not its
// value.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "oracle/workloads/monte_carlo/replay.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::workloads::monte_carlo {
namespace {

using testing::compute_unit;
using testing::generate_unit;
using testing::make;
using testing::native_path;
using testing::SerialExecutor;
using testing::ThreadedExecutor;

constexpr double kSamples = static_cast<double>(kSamplesPerBatch);
// The specification's standard deviation of one batch sum: S times the
// variance 2/3 - pi^2/16 of one sample, square-rooted.
const double kBatchSigma =
    std::sqrt(kSamples * (2.0 / 3.0 - std::numbers::pi * std::numbers::pi / 16.0));

std::vector<double> sums_of(const std::vector<std::byte>& output) {
  std::vector<double> sums;
  for (std::size_t batch = 0; batch < kBatches; ++batch) {
    sums.push_back(replay::load_double(std::span<const std::byte>(output).subspan(batch * 8, 8)));
  }
  return sums;
}

TEST(MonteCarloProperty, EveryBatchSumLiesWhereTheIntegrandCanTakeIt) {
  SerialExecutor serial;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    const auto workload = make(11, 0, path);
    ASSERT_NE(workload, nullptr);
    for (std::uint64_t index = 0; index < 3; ++index) {
      const std::vector<std::byte> input = generate_unit(*workload, index);
      for (const double sum : sums_of(compute_unit(*workload, index, input, serial))) {
        EXPECT_TRUE(std::isfinite(sum));
        EXPECT_GE(sum, kSamples * 0x1p-26) << "each sample is at least 2^-26";
        EXPECT_LE(sum, kSamples) << "each sample is at most 1";
      }
    }
  }
}

TEST(MonteCarloProperty, TheMeanAndTheSpreadOfTheSumsAreThoseOfTheIntegral) {
  SerialExecutor serial;
  const auto workload = make(2024, 0, native_path());
  ASSERT_NE(workload, nullptr);
  std::vector<double> sums;
  for (std::uint64_t index = 0; index < 8; ++index) {
    const std::vector<std::byte> input = generate_unit(*workload, index);
    const std::vector<double> unit = sums_of(compute_unit(*workload, index, input, serial));
    sums.insert(sums.end(), unit.begin(), unit.end());
  }
  const double count = static_cast<double>(sums.size());
  double mean = 0.0;
  for (const double sum : sums) {
    mean += sum / count;
  }
  double variance = 0.0;
  for (const double sum : sums) {
    variance += (sum - mean) * (sum - mean) / (count - 1.0);
  }
  const double expected = kSamples * std::numbers::pi / 4.0;
  // Six standard errors of the mean; the spread of a sample variance of 512
  // sums is about 6%, so 30% is more than four of its standard errors.
  EXPECT_NEAR(mean, expected, 6.0 * kBatchSigma / std::sqrt(count));
  EXPECT_GT(variance, 0.7 * kBatchSigma * kBatchSigma);
  EXPECT_LT(variance, 1.3 * kBatchSigma * kBatchSigma);
}

TEST(MonteCarloProperty, ABatchSumDependsOnItsOwnKeyAlone) {
  SerialExecutor serial;
  const auto workload = make(5, 0, native_path());
  ASSERT_NE(workload, nullptr);
  const std::vector<std::byte> input = generate_unit(*workload, 1);
  const std::vector<std::byte> base = compute_unit(*workload, 1, input, serial);

  std::vector<std::byte> changed = input;
  changed[5 * 8] ^= std::byte{0x10};
  const std::vector<std::byte> after = compute_unit(*workload, 1, changed, serial);
  for (std::size_t batch = 0; batch < kBatches; ++batch) {
    const auto before_word = std::span<const std::byte>(base).subspan(batch * 8, 8);
    const auto after_word = std::span<const std::byte>(after).subspan(batch * 8, 8);
    EXPECT_EQ(std::equal(before_word.begin(), before_word.end(), after_word.begin()), batch != 5)
        << "batch " << batch;
  }

  std::vector<std::byte> swapped = input;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    std::swap(swapped[3 * 8 + byte], swapped[40 * 8 + byte]);
  }
  const std::vector<std::byte> swapped_output = compute_unit(*workload, 1, swapped, serial);
  const std::vector<double> original_sums = sums_of(base);
  const std::vector<double> swapped_sums = sums_of(swapped_output);
  EXPECT_EQ(swapped_sums[3], original_sums[40]);
  EXPECT_EQ(swapped_sums[40], original_sums[3]);
  EXPECT_EQ(swapped_sums[7], original_sums[7]);
}

TEST(MonteCarloProperty, BitsOfAKeyAcrossItsWidthChangeTheSum) {
  SerialExecutor serial;
  const auto workload = make(5, 0, simd::IsaPath::kScalar);
  ASSERT_NE(workload, nullptr);
  const std::vector<std::byte> input = generate_unit(*workload, 0);
  const std::vector<double> base = sums_of(compute_unit(*workload, 0, input, serial));
  for (unsigned bit = 0; bit < 64; bit += 5) {  // 13 bits, 0 .. 60, across all eight bytes
    std::vector<std::byte> flipped = input;
    flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    EXPECT_NE(sums_of(compute_unit(*workload, 0, flipped, serial))[0], base[0]) << "bit " << bit;
  }
}

TEST(MonteCarloProperty, ArbitraryKeysGiveFiniteSumsInRangeAtAnyThreadCount) {
  ThreadedExecutor four(4);
  const auto workload = make(1, 4096, native_path());
  ASSERT_NE(workload, nullptr);
  for (std::uint64_t stream = 0; stream < 3; ++stream) {
    std::vector<std::byte> input(kInputBytes);
    rng::fill_stream(0xDEAD, stream, input);
    for (const double sum : sums_of(compute_unit(*workload, stream, input, four))) {
      EXPECT_TRUE(std::isfinite(sum));
      EXPECT_GE(sum, kSamples * 0x1p-26);
      EXPECT_LE(sum, kSamples);
    }
  }
}

TEST(MonteCarloProperty, DifferentSeedsGiveDifferentUnits) {
  SerialExecutor serial;
  const auto first = make(1, 0, simd::IsaPath::kScalar);
  const auto second = make(2, 0, simd::IsaPath::kScalar);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(compute_unit(*first, 0, generate_unit(*first, 0), serial),
            compute_unit(*second, 0, generate_unit(*second, 0), serial));
}

}  // namespace
}  // namespace loadforge::workloads::monte_carlo
