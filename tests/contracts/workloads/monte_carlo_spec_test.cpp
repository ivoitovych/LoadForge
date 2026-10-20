// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the Monte Carlo specification carries. Expected values are
// written out, never computed by the function under test; the Philox words
// are Random123's known-answer vectors (tests/fixtures/rng/philox4x32-10.kat).

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "workloads/monte_carlo/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

namespace mc = monte_carlo;

TEST(MonteCarloSpecDefinition, NameAndVersion) {
  EXPECT_EQ(mc::kName, "monte_carlo");
  EXPECT_TRUE(is_valid_name(mc::kName));
  EXPECT_EQ(mc::kVersion, 1U);
}

TEST(MonteCarloSpecDefinition, Sizes) {
  EXPECT_EQ(mc::kBatches, 64U);
  EXPECT_EQ(mc::kSamplesPerBatch, 16384U);
  EXPECT_EQ(mc::kInputBytes, 512U);
  EXPECT_EQ(mc::kOutputBytes, 512U);
  EXPECT_EQ(mc::kSampledBatches, 4U);
}

TEST(MonteCarloSpecDefinition, BatchKeysAreTheFirstTwoWordsOfOneBlockPerBatch) {
  // Counter 0, key 0: 6627e8d5 e169c58d bc57ac4c 9b00dbd8.
  EXPECT_EQ(mc::batch_key(0, 0, 0), 0xE169C58D6627E8D5ULL);
  // Every counter and key word ffffffff: 408f276d 41c83b0e a20bc7c6 6d5451fd.
  constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};
  EXPECT_EQ(mc::batch_key(kAllOnes, kAllOnes, kAllOnes), 0x41C83B0E408F276DULL);
  // The arguments are (seed, unit, batch) in block_at's order.
  EXPECT_EQ(mc::batch_key(7, 3, 11), rng::u64_of(rng::block_at(7, 3, 11)));
  EXPECT_NE(mc::batch_key(7, 3, 11), mc::batch_key(7, 11, 3));
}

TEST(MonteCarloSpecDefinition, EvenSamplesTakeWordsZeroAndOneOddSamplesWordsTwoAndThree) {
  // Batch key 0, block 0 is counter 0 under key 0: 6627e8d5 e169c58d bc57ac4c 9b00dbd8.
  EXPECT_EQ(mc::sample_bits(0, 0), 0xE169C58D6627E8D5ULL);
  EXPECT_EQ(mc::sample_bits(0, 1), 0x9B00DBD8BC57AC4CULL);
  // Samples 2 and 3 are block 1 of stream 0, under the batch key as the seed.
  const rng::Block second = rng::block_at(0x0123456789ABCDEFULL, 0, 1);
  EXPECT_EQ(mc::sample_bits(0x0123456789ABCDEFULL, 2),
            std::uint64_t{second[0]} | (std::uint64_t{second[1]} << 32U));
  EXPECT_EQ(mc::sample_bits(0x0123456789ABCDEFULL, 3),
            std::uint64_t{second[2]} | (std::uint64_t{second[3]} << 32U));
  EXPECT_NE(mc::sample_bits(0x0123456789ABCDEFULL, 2), mc::sample_bits(0x0123456789ABCDEFULL, 3));
}

TEST(MonteCarloSpecDefinition, TheIntegrandSpansTwoToMinus26ToOneExactly) {
  EXPECT_EQ(mc::sample_value(0), 1.0) << "x = 0";
  EXPECT_EQ(mc::sample_value(0x7FFU), 1.0) << "the low 11 bits are discarded";
  EXPECT_EQ(mc::sample_value(~std::uint64_t{0}), 0x1p-26)
      << "x = 1 - 2^-53, x * x rounds to 1 - 2^-52, the subtraction leaves 2^-52";
  EXPECT_EQ(mc::sample_value(std::uint64_t{1} << 63U), 0x1.bb67ae8584caap-1) << "sqrt(0.75)";
}

TEST(MonteCarloSpecDefinition, ChunkLengthsDivideTheBatch) {
  for (const std::uint32_t length : {64U, 256U, 1024U, 4096U, 16384U}) {
    EXPECT_TRUE(mc::is_chunk_length(length)) << length;
    EXPECT_EQ(mc::kSamplesPerBatch % length, 0U) << length;
  }
  for (const std::uint32_t length : {0U, 1U, 128U, 2048U, 32768U}) {
    EXPECT_FALSE(mc::is_chunk_length(length)) << length;
  }
  EXPECT_EQ(mc::kDefaultChunkLength, 1024U);
  EXPECT_TRUE(mc::is_chunk_length(mc::kDefaultChunkLength));
}

TEST(MonteCarloSpecDefinition, SampledBatchesAreFourApartAndRotateWithTheUnit) {
  EXPECT_EQ(mc::sampled_batch(0, 0), 0U);
  EXPECT_EQ(mc::sampled_batch(0, 1), 16U);
  EXPECT_EQ(mc::sampled_batch(0, 2), 32U);
  EXPECT_EQ(mc::sampled_batch(0, 3), 48U);
  EXPECT_EQ(mc::sampled_batch(5, 3), 53U);
  EXPECT_EQ(mc::sampled_batch(63, 1), 15U);
  EXPECT_EQ(mc::sampled_batch(64, 2), 32U) << "a function of index mod 64";
  EXPECT_EQ(mc::sampled_batch(~std::uint64_t{0}, 0), 63U) << "no overflow at the top";
  std::set<std::uint64_t> every_batch;
  for (std::uint64_t index = 0; index < 16; ++index) {
    std::set<std::uint64_t> in_unit;
    for (std::size_t m = 0; m < mc::kSampledBatches; ++m) {
      in_unit.insert(mc::sampled_batch(index, m));
      every_batch.insert(mc::sampled_batch(index, m));
    }
    EXPECT_EQ(in_unit.size(), mc::kSampledBatches) << "distinct in unit " << index;
  }
  EXPECT_EQ(every_batch.size(), mc::kBatches) << "sixteen consecutive units sample every batch";
}

TEST(MonteCarloSpecDefinition, TheToleranceIsTwoToMinus38OfTheReference) {
  EXPECT_EQ(mc::kUnitRoundoff, 0x1p-53);
  EXPECT_EQ(mc::kToleranceScale, 0x1p-38);
  EXPECT_EQ(mc::tolerance(1.0), 0x1p-38);
  EXPECT_EQ(mc::tolerance(12868.0), 0x1.9220000000000p-25) << "12868 = 0x3244, scaled exactly";
  // The inequality the derivation rests on, in integers: 2 S (S - 1) <= 2^53.
  EXPECT_EQ(2 * mc::kSamplesPerBatch * (mc::kSamplesPerBatch - 1), 536838144U);
  EXPECT_LE(2 * mc::kSamplesPerBatch * (mc::kSamplesPerBatch - 1), std::size_t{1} << 53U);
}

// The derivation at work on a real batch: three summation orders of the same
// f_k stay within the tolerance of one another, and every f_k is in range.
TEST(MonteCarloSpecDefinition, SumsInDifferentOrdersStayWithinTheTolerance) {
  const std::uint64_t key = mc::batch_key(0x5EED, 3, 17);
  std::vector<double> samples;
  samples.reserve(mc::kSamplesPerBatch);
  for (std::uint64_t k = 0; k < mc::kSamplesPerBatch; ++k) {
    samples.push_back(mc::sample_value(mc::sample_bits(key, k)));
    ASSERT_GE(samples.back(), 0x1p-26);
    ASSERT_LE(samples.back(), 1.0);
  }
  double forward = 0.0;
  for (const double f : samples) {
    forward += f;
  }
  double backward = 0.0;
  for (std::size_t k = samples.size(); k > 0; --k) {
    backward += samples[k - 1];
  }
  std::vector<double> tree = samples;
  for (std::size_t width = tree.size(); width > 1; width /= 2) {
    for (std::size_t i = 0; i < width / 2; ++i) {
      tree[i] = tree[2 * i] + tree[(2 * i) + 1];
    }
  }
  const double pairwise = tree.front();
  EXPECT_LE(std::fabs(forward - pairwise), mc::tolerance(pairwise));
  EXPECT_LE(std::fabs(backward - pairwise), mc::tolerance(pairwise));
  EXPECT_LE(std::fabs(forward - backward), mc::tolerance(backward));
  // About S pi / 4 with a standard deviation near 28.6: six of them is far.
  EXPECT_NEAR(pairwise, 16384.0 * 0.78539816339744831, 6.0 * 28.6);
}

}  // namespace
}  // namespace loadforge::workloads
