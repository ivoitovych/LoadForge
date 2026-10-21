// SPDX-License-Identifier: GPL-3.0-or-later
// T3: the workload against a scalar replay written from spec.hpp alone
// (replay.hpp), on a fixed-seed subset: the bits for every chunk length and
// every path this machine runs, and the specification's bounded check.

#include "oracle/workloads/monte_carlo/replay.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::workloads::monte_carlo {
namespace {

using testing::compute_unit;
using testing::generate_unit;
using testing::lanes_of;
using testing::make;
using testing::native_path;
using testing::SerialExecutor;

struct Subset {
  std::uint64_t seed;
  std::uint64_t index;
};

// Fixed seeds and units: the extremes of the index, a small one, and a seed
// of zero. The subset is what the replay can afford; the property tier
// covers the rest by statistics.
constexpr Subset kSubset[] = {{0xC0FFEE, 17}, {0, (std::uint64_t{1} << 40) + 3}};

std::span<const std::byte> word_at(const std::vector<std::byte>& bytes, std::uint64_t batch) {
  return std::span<const std::byte>(bytes).subspan(batch * 8, 8);
}

TEST(OracleReplay, SampledBatchesMatchTheOrderedReplayBitForBit) {
  SerialExecutor serial;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    for (const std::uint32_t chunk : kChunkLengths) {
      for (const Subset& subset : kSubset) {
        const auto workload = make(subset.seed, chunk, path);
        ASSERT_NE(workload, nullptr);
        const std::vector<std::byte> input = generate_unit(*workload, subset.index);
        const std::vector<std::byte> output = compute_unit(*workload, subset.index, input, serial);
        for (std::size_t m = 0; m < kSampledBatches; ++m) {
          const std::uint64_t batch = sampled_batch(subset.index, m);
          const double replayed =
              replay::ordered_sum(replay::key_of(input, batch), lanes_of(path), chunk);
          EXPECT_EQ(std::bit_cast<std::uint64_t>(replay::load_double(word_at(output, batch))),
                    std::bit_cast<std::uint64_t>(replayed))
              << simd::describe(path) << " chunk " << chunk << " seed " << subset.seed << " unit "
              << subset.index << " batch " << batch;
        }
      }
    }
  }
}

TEST(OracleReplay, TheSpecifiedCheckAgreesForEveryPathAndChunkLength) {
  SerialExecutor serial;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    for (const std::uint32_t chunk : kChunkLengths) {
      const auto workload = make(0xC0FFEE, chunk, path);
      ASSERT_NE(workload, nullptr);
      const std::vector<std::byte> input = generate_unit(*workload, 5);
      const std::vector<std::byte> output = compute_unit(*workload, 5, input, serial);
      const replay::Outcome outcome = replay::check(5, input, output);
      EXPECT_EQ(outcome.verdict, replay::Verdict::kAgreement)
          << simd::describe(path) << " chunk " << chunk << ": batch " << outcome.batch
          << " residual " << outcome.residual << " tolerance " << outcome.tolerance;
      EXPECT_EQ(outcome.blocks_compared, kSampledBatches);
    }
  }
}

TEST(OracleReplay, EveryBatchOfAUnitAgreesWhichBatchesAreSampled) {
  // Any 16 consecutive units between them sample every batch; here every
  // batch of one unit is judged by moving the sampled set, with a unit index
  // for each residue of the sampling rule.
  SerialExecutor serial;
  const auto workload = make(77, 0, native_path());
  ASSERT_NE(workload, nullptr);
  const std::vector<std::byte> input = generate_unit(*workload, 0);
  const std::vector<std::byte> output = compute_unit(*workload, 0, input, serial);
  for (std::uint64_t index = 0; index < kBatches / kSampledBatches; ++index) {
    EXPECT_EQ(replay::check(index, input, output).verdict, replay::Verdict::kAgreement)
        << "sampling as unit " << index;
  }
}

TEST(OracleReplay, TheExactSumIsWhatAnExtendedPrecisionSumGives) {
  for (const std::uint64_t key : {std::uint64_t{0}, std::uint64_t{0xABCDEF}, ~std::uint64_t{0}}) {
    long double extended = 0.0L;
    for (std::uint64_t k = 0; k < kSamplesPerBatch; ++k) {
      extended += static_cast<long double>(replay::f(key, k));
    }
    const double exact = replay::exact_sum(key);
    EXPECT_NEAR(static_cast<double>(extended), exact, 1e-12 * exact) << key;
    EXPECT_GT(exact, 0.0);
  }
}

TEST(OracleReplay, TheOrderedSumsOfAllPathsStayWithinTheBoundOfTheExactSum) {
  const std::uint64_t key = 0x1234567890ABCDEFULL;
  const double exact = replay::exact_sum(key);
  for (const unsigned lanes : {1U, 2U, 4U, 8U}) {
    for (const std::uint32_t chunk : kChunkLengths) {
      const double sum = replay::ordered_sum(key, lanes, chunk);
      EXPECT_LE(std::fabs(sum - exact), tolerance(exact)) << lanes << " lanes, chunk " << chunk;
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::monte_carlo
