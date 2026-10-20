// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for the compression workload: BitExact (determinism.md §2), enforced.
//
//   * across executor thread counts -- 1, 2, 3, 4 and 7, on executors whose
//     slot for an index changes from run to run -- and across repeated runs;
//   * across what scratch and output held before;
//   * across workloads made from configurations that differ only in what
//     the bits must not depend on: the footprint, and decomposition 0 or 1;
//   * across architectures and builds: the output is the committed bytes the
//     real lz4 tool decoded (tests/fixtures/lz4-workload/), which CI checks
//     on x86-64 and ARM64 alike.
//
// And no decomposition but 1 is accepted: a BitExact workload with nothing to
// decompose has no second decomposition to be exact across.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "unit/workloads/compression/executors.hpp"
#include "unit/workloads/compression/support.hpp"
#include "worker/executor.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Bytes;

constexpr std::uint64_t kUnitBytes = kInputBytes + output_bytes_for(kInputBytes);

std::unique_ptr<Workload> made(std::uint64_t seed, std::uint64_t footprint = 4 * kUnitBytes,
                               std::uint32_t decomposition = 0) {
  auto created = create(WorkloadConfig{seed, footprint, decomposition, simd::IsaPath::kScalar});
  EXPECT_TRUE(created.has_value());
  return created.has_value() ? std::move(created).value() : nullptr;
}

Bytes generated(const Workload& workload, std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

Bytes computed(const Workload& workload, std::uint64_t index, const Bytes& input,
               worker::Executor& executor, std::byte fill) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

TEST(CompressionBitExact, TheDeclaredClassIsBitExact) {
  const auto workload = made(1);
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->descriptor().determinism, DeterminismClass::kBitExact);
  EXPECT_EQ(describe(workload->descriptor().determinism), "BitExact");
}

TEST(CompressionBitExact, AtOneTwoThreeFourAndSevenThreadsAndAcrossRepeatedRuns) {
  const auto workload = made(42);
  ASSERT_NE(workload, nullptr);
  testing::SerialExecutor serial;
  for (const std::uint64_t index :
       {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{7}, ~std::uint64_t{0}}) {
    const Bytes input = generated(*workload, index);
    const Bytes reference = computed(*workload, index, input, serial, std::byte{0});
    for (const std::size_t threads : {1U, 2U, 3U, 4U, 7U}) {
      testing::ThreadedExecutor executor(threads);
      for (int run = 0; run < 5; ++run) {
        const std::byte fill = static_cast<std::byte>(0x3B * (run + 1));
        EXPECT_EQ(computed(*workload, index, input, executor, fill), reference)
            << "unit " << index << ", " << threads << " threads, run " << run;
      }
    }
  }
}

TEST(CompressionBitExact, WhicheverSlotRunsTheUnit) {
  const auto workload = made(42);
  ASSERT_NE(workload, nullptr);
  const Bytes input = generated(*workload, 3);
  testing::SerialExecutor serial;
  const Bytes reference = computed(*workload, 3, input, serial, std::byte{0});
  for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
    testing::LastSlotExecutor last(threads);
    EXPECT_EQ(computed(*workload, 3, input, last, std::byte{0xFF}), reference) << threads;
  }
}

TEST(CompressionBitExact, ManyUnitsAtOnceOnSevenThreadsAreEachTheirSerialOutput) {
  // The runner's shape: units spread over the executor, each computed by a
  // task on a serial executor of its own (a workload's compute() is called
  // with an executor, never re-entered on the one running it).
  const auto workload = made(9);
  ASSERT_NE(workload, nullptr);
  constexpr std::size_t kUnits = 21;
  std::vector<Bytes> inputs;
  std::vector<Bytes> alone;
  testing::SerialExecutor serial;
  for (std::uint64_t index = 0; index < kUnits; ++index) {
    inputs.push_back(generated(*workload, index));
    alone.push_back(computed(*workload, index, inputs.back(), serial, std::byte{0}));
  }
  std::vector<Bytes> together(kUnits, Bytes(workload->plan().output_bytes, std::byte{0xAA}));
  std::vector<Bytes> scratch(7, Bytes(workload->plan().scratch_bytes, std::byte{0xAA}));
  auto unit = [&](std::size_t index, std::size_t slot) noexcept {
    testing::SerialExecutor own;
    workload->compute(index, inputs[index], together[index], scratch[slot], own);
  };
  testing::ThreadedExecutor seven(7);
  for (int run = 0; run < 3; ++run) {
    worker::for_each_index(seven, kUnits, unit);
    EXPECT_EQ(together, alone) << "run " << run;
  }
}

TEST(CompressionBitExact, TheFootprintAndDecompositionZeroOrOneDoNotTouchTheBits) {
  testing::SerialExecutor serial;
  const auto base = made(5, kUnitBytes, 0);
  ASSERT_NE(base, nullptr);
  const Bytes input = generated(*base, 2);
  const Bytes reference = computed(*base, 2, input, serial, std::byte{0});
  for (const auto& [footprint, decomposition] :
       std::vector<std::pair<std::uint64_t, std::uint32_t>>{
           {kUnitBytes, 1},
           {1000 * kUnitBytes, 0},
           {std::numeric_limits<std::uint64_t>::max(), 1}}) {
    const auto other = made(5, footprint, decomposition);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(generated(*other, 2), input);
    EXPECT_EQ(computed(*other, 2, input, serial, std::byte{0}), reference);
  }
}

TEST(CompressionBitExact, NoDecompositionButOneIsAccepted) {
  for (std::uint32_t decomposition = 2; decomposition <= 256; ++decomposition) {
    const auto created =
        create(WorkloadConfig{1, kUnitBytes, decomposition, simd::IsaPath::kScalar});
    ASSERT_FALSE(created.has_value()) << decomposition;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
  }
  EXPECT_FALSE(create(WorkloadConfig{1, kUnitBytes, std::numeric_limits<std::uint32_t>::max(),
                                     simd::IsaPath::kScalar})
                   .has_value());
}

TEST(CompressionBitExact, TheOutputIsTheCommittedBytesOnThisArchitectureAndBuild) {
  struct Unit {
    std::uint64_t seed;
    std::uint64_t index;
    std::string name;
  };
  testing::ThreadedExecutor three(3);
  for (const Unit& unit : {Unit{1, 0, "unit-seed1-index0"}, Unit{1, 1, "unit-seed1-index1"},
                           Unit{~std::uint64_t{0}, ~std::uint64_t{0}, "unit-seedmax-indexmax"}}) {
    const auto workload = made(unit.seed);
    ASSERT_NE(workload, nullptr);
    const Bytes input = generated(*workload, unit.index);
    EXPECT_EQ(input, testing::fixture(unit.name + ".bin")) << unit.name;
    const Bytes output = computed(*workload, unit.index, input, three, std::byte{0x77});
    const testing::Frame frame = testing::legacy_blocks(testing::fixture(unit.name + ".ours.lz4"));
    ASSERT_TRUE(frame.ok) << unit.name;
    ASSERT_EQ(frame.blocks.size(), 1U) << unit.name;
    const Bytes& block = frame.blocks[0];
    EXPECT_EQ(load_le32(output, 4), block.size()) << unit.name;
    EXPECT_EQ(
        Bytes(output.begin() + 8, output.begin() + 8 + static_cast<std::ptrdiff_t>(block.size())),
        block)
        << unit.name;
  }
}

}  // namespace
}  // namespace loadforge::workloads::compression
