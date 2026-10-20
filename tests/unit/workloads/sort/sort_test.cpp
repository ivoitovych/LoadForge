// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/workloads/sort: the factory's refusals, the descriptor and the
// plan, the generator, and the sort on inputs whose sorted output is written
// down by formula (never produced by sorting), plus the provider's
// WorkloadContract run against the implementation.

#include "workloads/sort/sort.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "contracts/workloads/workload_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/workloads/sort/executors.hpp"
#include "unit/workloads/sort/harness.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sort::testing {
namespace {

struct SortSubject {
  static constexpr std::string_view kName = "sort";
  static WorkloadConfig config() { return config_with(0, 0x5EED); }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(
      const WorkloadConfig& config) {
    return sort::create(config);
  }
};

TEST(SortFactory, ADecompositionOutsideTheSpecificationIsRefusedByName) {
  for (const std::uint32_t chunks : {3U, 5U, 48U, 65U, 128U, 4096U, 0xFFFFFFFFU}) {
    const auto created = create(config_with(chunks));
    ASSERT_FALSE(created.has_value()) << chunks;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(chunks)), std::string::npos)
        << created.error().detail;
  }
}

TEST(SortFactory, EveryChunkCountResolvesToItselfAndZeroToTheDefault) {
  for (const std::uint32_t chunks : kChunkCounts) {
    const auto workload = make(chunks);
    ASSERT_NE(workload, nullptr);
    EXPECT_EQ(workload->plan().decomposition, chunks);
  }
  const auto defaulted = make(0);
  ASSERT_NE(defaulted, nullptr);
  EXPECT_EQ(defaulted->plan().decomposition, kDefaultChunkCount);
  EXPECT_EQ(kDefaultChunkCount, 16U);
}

TEST(SortFactory, AnIsaPathOtherThanScalarIsRefusedAndNamed) {
  const std::array<std::pair<simd::IsaPath, std::string>, 3> paths = {
      {{simd::IsaPath::kAvx2, "avx2"},
       {simd::IsaPath::kAvx512, "avx512"},
       {simd::IsaPath::kNeon, "neon"}}};
  for (const auto& [path, name] : paths) {
    WorkloadConfig config = config_with(0);
    config.isa = path;
    const auto created = create(config);
    ASSERT_FALSE(created.has_value()) << name;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(name), std::string::npos) << created.error().detail;
  }
}

TEST(SortFactory, TheDecompositionIsCheckedBeforeThePath) {
  WorkloadConfig config = config_with(3);
  config.isa = simd::IsaPath::kAvx2;
  const auto created = create(config);
  ASSERT_FALSE(created.has_value());
  EXPECT_NE(created.error().detail.find("decomposition"), std::string::npos);
}

TEST(SortFactory, OneUnitFitsExactlyAtItsSizeAndNotOneByteBelow) {
  WorkloadConfig config = config_with(0);
  config.footprint_bytes = kUnitBytes - 1;
  const auto below = create(config);
  ASSERT_FALSE(below.has_value());
  EXPECT_EQ(below.error().fault, WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(below.error().detail.find(std::to_string(kUnitBytes - 1)), std::string::npos);
  EXPECT_NE(below.error().detail.find(std::to_string(kUnitBytes)), std::string::npos);

  config.footprint_bytes = 0;
  const auto none = create(config);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().fault, WorkloadFault::kFootprintTooSmall);

  config.footprint_bytes = kUnitBytes;
  const auto exact = create(config);
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(exact.value()->plan().units, 1U);

  config.footprint_bytes = 5 * kUnitBytes - 1;
  const auto four = create(config);
  ASSERT_TRUE(four.has_value());
  EXPECT_EQ(four.value()->plan().units, 4U);

  config.footprint_bytes = std::numeric_limits<std::uint64_t>::max();
  const auto huge = create(config);
  ASSERT_TRUE(huge.has_value());
  EXPECT_EQ(huge.value()->plan().units, std::numeric_limits<std::uint64_t>::max() / kUnitBytes);
}

TEST(SortDescriptor, SaysWhatTheSpecificationSays) {
  const auto workload = make();
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "sort");
  EXPECT_EQ(descriptor.version, kVersion);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBitExact);
  EXPECT_EQ(descriptor.contract, verification::Contract::kExact);
  EXPECT_EQ(descriptor.paths, simd::IsaSet{simd::IsaPath::kScalar});
}

TEST(SortPlan, IsTheGeometryOfTheSpecificationAndTheCostsWrittenOut) {
  const auto workload = make(8);
  ASSERT_NE(workload, nullptr);
  const Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, 1048576U);
  EXPECT_EQ(plan.output_bytes, 1572864U);
  // A second copy of the keys (1 MiB) and indices (0.5 MiB), and 8 ranges of
  // 256 four-byte counters.
  EXPECT_EQ(plan.scratch_bytes, 1572864U + 8U * 256U * 4U);
  EXPECT_EQ(plan.units, 3U);
  EXPECT_EQ(plan.decomposition, 8U);
  // 131072 keys * (28 bytes in the first pass + 7 * 32 in the others).
  EXPECT_EQ(plan.logical_bytes, 33030144U);
  // 131072 keys * 8 passes * (a count and a move).
  EXPECT_EQ(plan.operations, 2097152U);
  EXPECT_EQ(make(64)->plan().scratch_bytes, 1572864U + 64U * 256U * 4U);
}

TEST(SortGenerate, WritesTheKeysOfTheSpecificationLittleEndian) {
  const auto workload = make(0, 77);
  ASSERT_NE(workload, nullptr);
  const Bytes input = generate(*workload, 9);
  ASSERT_EQ(input.size(), kInputBytes);
  for (const std::size_t position :
       {std::size_t{0}, std::size_t{1}, std::size_t{4095}, kKeys / 2, kKeys - 1}) {
    EXPECT_EQ(key_of(input, position), rng::u64_of(rng::block_at(77, 9, position))) << position;
  }
  // The first key's bytes, one by one: seed 0, unit 0, position 0 is Random123's
  // known answer 6627e8d5 e169c58d, so the key is 0xe169c58d6627e8d5.
  const auto zero = make(0, 0);
  const Bytes first = generate(*zero, 0);
  const std::array<std::uint8_t, 8> expected = {0xD5, 0xE8, 0x27, 0x66, 0x8D, 0xC5, 0x69, 0xE1};
  for (std::size_t byte = 0; byte < expected.size(); ++byte) {
    EXPECT_EQ(std::to_integer<std::uint8_t>(first[byte]), expected.at(byte)) << byte;
  }
}

// Keys written as formulas, so the sorted output is written as a formula too.

TEST(SortKnownOutputs, KeysThatAreAPermutationOfTheirRank) {
  // e -> e * 40503 mod 2^17 is a bijection on [0, 2^17) (the multiplier is odd),
  // so the sorted keys are 0, 1, 2, ... and the index at rank r is the e with
  // e * 40503 = r, which is r * 40503^-1 mod 2^17. 40503 * 96135 = 3893755905 =
  // 29707 * 2^17 + 1.
  constexpr std::uint64_t kMultiplier = 40503;
  constexpr std::uint64_t kInverse = 96135;
  constexpr std::uint64_t kMask = kKeys - 1;
  static_assert(((kMultiplier * kInverse) & kMask) == 1);
  const Bytes input = input_of([](std::size_t e) { return (e * kMultiplier) & kMask; });
  SerialExecutor serial;
  for (const std::uint32_t chunks : kChunkCounts) {
    const auto workload = make(chunks);
    const Bytes output = sorted(*workload, 0, input, serial);
    for (std::size_t rank = 0; rank < kKeys; ++rank) {
      ASSERT_EQ(key_of(output, rank), rank) << "P " << chunks;
      ASSERT_EQ(index_of(output, rank), (rank * kInverse) & kMask) << "P " << chunks;
    }
  }
}

TEST(SortKnownOutputs, KeysInDescendingOrderComeOutReversed) {
  const Bytes input = input_of([](std::size_t e) { return std::uint64_t{kKeys - 1 - e}; });
  SerialExecutor serial;
  const auto workload = make();
  const Bytes output = sorted(*workload, 0, input, serial);
  for (std::size_t rank = 0; rank < kKeys; ++rank) {
    ASSERT_EQ(key_of(output, rank), rank);
    ASSERT_EQ(index_of(output, rank), kKeys - 1 - rank);
  }
}

TEST(SortKnownOutputs, KeysAlreadyAscendingAreTheIdentity) {
  const Bytes input = input_of([](std::size_t e) { return std::uint64_t{e} << 40U; });
  SerialExecutor serial;
  const auto workload = make();
  const Bytes output = sorted(*workload, 0, input, serial);
  for (std::size_t rank = 0; rank < kKeys; ++rank) {
    ASSERT_EQ(key_of(output, rank), std::uint64_t{rank} << 40U);
    ASSERT_EQ(index_of(output, rank), rank);
  }
}

TEST(SortKnownOutputs, EqualKeysKeepTheirInputOrder) {
  for (const std::uint64_t key :
       {std::uint64_t{0}, std::uint64_t{0x0123456789ABCDEF}, ~std::uint64_t{0}}) {
    const Bytes input = input_of([key](std::size_t) { return key; });
    SerialExecutor serial;
    for (const std::uint32_t chunks : {1U, 16U, 64U}) {
      const auto workload = make(chunks);
      const Bytes output = sorted(*workload, 0, input, serial);
      for (std::size_t rank = 0; rank < kKeys; ++rank) {
        ASSERT_EQ(key_of(output, rank), key);
        ASSERT_EQ(index_of(output, rank), rank) << "key " << key << " P " << chunks;
      }
    }
  }
}

TEST(SortKnownOutputs, TwoValuesInterleavedSplitByValueThenByPosition) {
  // Even positions hold 5, odd ones 3: all the 3s first (odd positions in
  // order), then all the 5s (even positions in order).
  const Bytes input = input_of([](std::size_t e) { return e % 2 == 0 ? 5U : 3U; });
  SerialExecutor serial;
  const auto workload = make(4);
  const Bytes output = sorted(*workload, 0, input, serial);
  for (std::size_t rank = 0; rank < kKeys / 2; ++rank) {
    ASSERT_EQ(key_of(output, rank), 3U);
    ASSERT_EQ(index_of(output, rank), 2 * rank + 1);
    ASSERT_EQ(key_of(output, kKeys / 2 + rank), 5U);
    ASSERT_EQ(index_of(output, kKeys / 2 + rank), 2 * rank);
  }
}

TEST(SortKnownOutputs, KeysAreUnsignedSoTheTopBitSortsLast) {
  // 0x8000... + e and e alternate: the small ones come first.
  const Bytes input = input_of(
      [](std::size_t e) { return e % 2 == 0 ? (std::uint64_t{1} << 63U) + e : std::uint64_t{e}; });
  SerialExecutor serial;
  const auto workload = make();
  const Bytes output = sorted(*workload, 0, input, serial);
  for (std::size_t rank = 0; rank < kKeys / 2; ++rank) {
    ASSERT_EQ(key_of(output, rank), 2 * rank + 1);
    ASSERT_EQ(index_of(output, rank), 2 * rank + 1);
    ASSERT_EQ(key_of(output, kKeys / 2 + rank), (std::uint64_t{1} << 63U) + 2 * rank);
    ASSERT_EQ(index_of(output, kKeys / 2 + rank), 2 * rank);
  }
}

TEST(SortKnownOutputs, EachByteOfTheKeyDecidesTheOrderOnItsOwn) {
  // Keys that differ in one byte only, e's low 17 bits placed in byte b: the
  // sorted order is by that byte, then position, for each of the eight passes.
  SerialExecutor serial;
  const auto workload = make();
  for (unsigned byte = 0; byte < 8; ++byte) {
    const Bytes input = input_of(
        [byte](std::size_t e) { return std::uint64_t{(kKeys - 1 - e) % 256} << (8U * byte); });
    const Bytes output = sorted(*workload, 0, input, serial);
    // Digit d = 255 - e % 256 (for e = kKeys-1-x, x%256... ) -- written as the
    // count: 512 keys per digit, keys with digit d at the positions e with
    // (kKeys - 1 - e) % 256 == d, i.e. e = kKeys - 1 - d - 256 m, in
    // ascending e: m from 511 down to 0.
    for (std::size_t rank = 0; rank < kKeys; ++rank) {
      const std::size_t digit = rank / 512;
      const std::size_t within = rank % 512;
      const std::size_t m = 511 - within;
      ASSERT_EQ(key_of(output, rank), std::uint64_t{digit} << (8U * byte)) << "byte " << byte;
      ASSERT_EQ(index_of(output, rank), kKeys - 1 - digit - 256 * m) << "byte " << byte;
    }
  }
}

TEST(SortCompute, WritesOnlyTheOutputAndScratchItIsGiven) {
  const auto workload = make();
  ASSERT_NE(workload, nullptr);
  const Bytes input = generate(*workload, 4);
  const Bytes before = generate(*workload, 4);
  SerialExecutor serial;
  (void)sorted(*workload, 4, input, serial);
  EXPECT_EQ(input, before) << "the input is read only";
}

TEST(SortCompute, TheUnitIndexIsNotAnInputOfTheSort) {
  // The output is a function of the input bytes alone (spec.hpp): the same
  // bytes given as another unit sort identically.
  const auto workload = make();
  const Bytes input = generate(*workload, 4);
  SerialExecutor serial;
  EXPECT_EQ(sorted(*workload, 4, input, serial), sorted(*workload, 99, input, serial));
}

}  // namespace
}  // namespace loadforge::workloads::sort::testing

// The contract header registers the registry suite too; this binary
// instantiates WorkloadContract only.
namespace loadforge::contracts {
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);
INSTANTIATE_TYPED_TEST_SUITE_P(Sort, WorkloadContract, workloads::sort::testing::SortSubject);
}  // namespace loadforge::contracts
