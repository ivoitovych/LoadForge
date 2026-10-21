// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/workloads/sort: BitExact means the same bits for every
// decomposition, thread count, run, scratch content and executor order
// (determinism.md §2), and the same bits as digests made without the workload
// (tests/fixtures/determinism/workloads/sort/golden.txt).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "unit/workloads/sort/executors.hpp"
#include "unit/workloads/sort/harness.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::workloads::sort::testing {
namespace {

std::uint64_t fnv1a64(const Bytes& bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const std::byte byte : bytes) {
    hash = (hash ^ std::to_integer<std::uint64_t>(byte)) * 0x100000001B3ULL;
  }
  return hash;
}

struct Golden {
  std::uint64_t seed = 0;
  std::uint64_t unit = 0;
  std::uint64_t input = 0;
  std::uint64_t output = 0;
};

std::vector<Golden> goldens() {
  std::ifstream in(std::string(LOADFORGE_SOURCE_DIR) +
                   "/tests/fixtures/determinism/workloads/sort/golden.txt");
  std::vector<Golden> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream fields(line);
    Golden row;
    fields >> std::hex >> row.seed >> row.unit >> row.input >> row.output;
    rows.push_back(row);
  }
  return rows;
}

TEST(SortDeterminism, TheFixtureHoldsEveryCaseItWasMadeFor) { EXPECT_EQ(goldens().size(), 5U); }

TEST(SortDeterminism, InputAndOutputMatchDigestsMadeWithoutTheWorkload) {
  SerialExecutor serial;
  for (const Golden& row : goldens()) {
    for (const std::uint32_t chunks : {1U, 16U, 64U}) {
      const auto workload = make(chunks, row.seed);
      ASSERT_NE(workload, nullptr);
      const Bytes input = generate(*workload, row.unit);
      EXPECT_EQ(fnv1a64(input), row.input) << "seed " << row.seed << " unit " << row.unit;
      EXPECT_EQ(fnv1a64(sorted(*workload, row.unit, input, serial)), row.output)
          << "seed " << row.seed << " unit " << row.unit << " P " << chunks;
    }
  }
}

TEST(SortDeterminism, EveryDecompositionThreadCountAndOrderGivesTheSameBits) {
  SerialExecutor serial;
  ReversedExecutor reversed;
  ThreadedExecutor two(2);
  ThreadedExecutor seven(7);
  const auto reference_workload = make(16, 4);
  const Bytes input = generate(*reference_workload, 1);
  const Bytes reference = sorted(*reference_workload, 1, input, serial);
  for (const std::uint32_t chunks : kChunkCounts) {
    const auto workload = make(chunks, 4);
    ASSERT_NE(workload, nullptr);
    for (worker::Executor* executor :
         std::vector<worker::Executor*>{&serial, &reversed, &two, &seven}) {
      EXPECT_EQ(sorted(*workload, 1, input, *executor), reference)
          << "P " << chunks << " threads " << executor->threads();
    }
  }
}

TEST(SortDeterminism, WhatTheScratchAndOutputHeldBeforeChangesNothing) {
  SerialExecutor serial;
  const auto workload = make(8, 4);
  const Bytes input = generate(*workload, 1);
  const Bytes reference = sorted(*workload, 1, input, serial, std::byte{0x00});
  for (const std::uint8_t fill : {0x01, 0x5A, 0xFF}) {
    EXPECT_EQ(sorted(*workload, 1, input, serial, std::byte{fill}), reference) << int{fill};
  }
}

TEST(SortDeterminism, RepeatedRunsOnThreadsGiveTheSameBits) {
  ThreadedExecutor five(5);
  const auto workload = make(32, 4);
  const Bytes input = generate(*workload, 1);
  const Bytes reference = sorted(*workload, 1, input, five);
  for (int run = 0; run < 5; ++run) {
    EXPECT_EQ(sorted(*workload, 1, input, five), reference) << run;
  }
}

TEST(SortDeterminism, TheConfiguredFootprintAndIsaPathDoNotChangeAUnit) {
  SerialExecutor serial;
  const auto small = create(WorkloadConfig{4, kUnitBytes, 16, simd::IsaPath::kScalar});
  const auto large = create(WorkloadConfig{4, 100 * kUnitBytes, 16, simd::IsaPath::kScalar});
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(large.has_value());
  const Bytes input = generate(*small.value(), 1);
  EXPECT_EQ(input, generate(*large.value(), 1));
  EXPECT_EQ(sorted(*small.value(), 1, input, serial), sorted(*large.value(), 1, input, serial));
}

}  // namespace
}  // namespace loadforge::workloads::sort::testing
