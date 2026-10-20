// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6: the declared class, BitExactFixedDecomposition, and what the
// specification promises beyond it. For one decomposition the output is the
// same bits at any thread count, on any schedule, in any run, on any ISA path
// and on either architecture; the distances are the same bits for every
// decomposition; and different decompositions give different rank bits.
//
// The golden digests at the end carry the cross-architecture claim: the same
// constants are asserted on x86-64 and on AArch64 (the scalar-only CI job
// builds on both), so a run that matched on one and not the other fails there.

#include <gtest/gtest.h>

#include <random>

#include "core/crc32.hpp"
#include "graph_support.hpp"

namespace loadforge::graph_test {
namespace {

struct Unit {
  Bytes input = reference_input(1, 0);
};

std::unique_ptr<workloads::Workload> workload_for(std::uint32_t accumulators,
                                                  simd::IsaPath path = simd::IsaPath::kScalar) {
  return make(config_for(accumulators, path));
}

TEST(GraphDeterminism, TheThreadCountAndTheScheduleNeverChangeTheOutput) {
  const Unit unit;
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const auto workload = workload_for(accumulators);
    ASSERT_NE(workload, nullptr);
    const Bytes expected = run_serial(*workload, unit.input);
    for (const std::size_t threads : {2U, 3U, 4U, 7U, 16U}) {
      PartitionExecutor partition(threads);
      EXPECT_EQ(run_compute(*workload, unit.input, partition), expected)
          << accumulators << " accumulators, " << threads << " threads, partitioned";
      PullExecutor pull(threads);
      EXPECT_EQ(run_compute(*workload, unit.input, pull), expected)
          << accumulators << " accumulators, " << threads << " threads, pulling";
    }
  }
}

TEST(GraphDeterminism, ThirtyRunsOnTheMostAdversarialExecutorAgreeWithTheFirst) {
  // Repeated, on threads that claim work in whatever order they get to it: the
  // ordering test for the relaxed atomics of the BFS (graph.cpp), together
  // with the TSan job. A weakening that mattered would show here as a
  // distance that differs between runs.
  const Unit unit;
  const auto workload = workload_for(0);
  ASSERT_NE(workload, nullptr);
  const Bytes expected = run_serial(*workload, unit.input);
  for (int run = 0; run < 30; ++run) {
    PullExecutor pull(8);
    ASSERT_EQ(run_compute(*workload, unit.input, pull), expected) << "run " << run;
  }
}

TEST(GraphDeterminism, EveryIsaPathProducesTheSameBitsForTheSameDecomposition) {
  const Unit unit;
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const Bytes expected = run_serial(*workload_for(accumulators), unit.input);
    for (const simd::IsaPath path : runnable_paths()) {
      PullExecutor pull(3);
      EXPECT_EQ(run_compute(*workload_for(accumulators, path), unit.input, pull), expected)
          << accumulators << " accumulators, " << simd::describe(path);
    }
  }
}

TEST(GraphDeterminism, TheDistancesAreTheSameForEveryDecompositionAndTheRanksDifferForEachPair) {
  const Unit unit;
  std::vector<Bytes> outputs;
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    outputs.push_back(run_serial(*workload_for(accumulators), unit.input));
  }
  for (std::size_t lhs = 0; lhs < outputs.size(); ++lhs) {
    for (std::size_t rhs = lhs + 1; rhs < outputs.size(); ++rhs) {
      EXPECT_EQ(distances_of(outputs[lhs]), distances_of(outputs[rhs])) << lhs << " " << rhs;
      EXPECT_NE(ranks_of(outputs[lhs]), ranks_of(outputs[rhs]))
          << "decompositions " << spec::kAccumulators[lhs] << " and " << spec::kAccumulators[rhs]
          << " sum in different orders";
    }
  }
}

TEST(GraphDeterminism, WhatTheScratchHeldNeverChangesTheOutput) {
  const Unit unit;
  const auto workload = workload_for(0);
  ASSERT_NE(workload, nullptr);
  const Bytes expected = run_serial(*workload, unit.input);
  SerialExecutor serial;
  for (const std::byte fill : {std::byte{0x00}, std::byte{0xFF}, std::byte{0xA5}}) {
    EXPECT_EQ(run_compute(*workload, unit.input, serial, fill), expected);
  }
  std::mt19937_64 engine(5);
  Bytes scratch(workload->plan().scratch_bytes);
  for (std::byte& byte : scratch) {
    byte = static_cast<std::byte>(engine() & 0xFFU);
  }
  Bytes output(spec::kOutputBytes);
  workload->compute(0, unit.input, output, scratch, serial);
  EXPECT_EQ(output, expected);
}

TEST(GraphDeterminism, WhereTheScratchStartsInMemoryNeverChangesTheOutput) {
  // The arena aligns itself inside the scratch it is given: every start
  // offset within a cache line is the same unit.
  const Unit unit;
  const auto workload = workload_for(0);
  ASSERT_NE(workload, nullptr);
  const Bytes expected = run_serial(*workload, unit.input);
  SerialExecutor serial;
  Bytes backing(workload->plan().scratch_bytes + 64);
  for (const std::size_t skew : {1U, 4U, 8U, 33U, 63U}) {
    Bytes output(spec::kOutputBytes);
    workload->compute(0, unit.input, output,
                      std::span<std::byte>(backing).subspan(skew, workload->plan().scratch_bytes),
                      serial);
    EXPECT_EQ(output, expected) << "skew " << skew;
  }
}

TEST(GraphDeterminism, ACorruptInputIsStillAFunctionOfItsInputAtAnyThreadCount) {
  Bytes noise(spec::kInputBytes);
  rng::fill_stream(0xBAD, 3, noise);
  const auto workload = workload_for(0);
  ASSERT_NE(workload, nullptr);
  const Bytes expected = run_serial(*workload, noise);
  PullExecutor pull(5);
  EXPECT_EQ(run_compute(*workload, noise, pull), expected);
}

// ---- golden digests -----------------------------------------------------------------
// CRC-32 (core/crc32.hpp) of each section of the output for seed 1, unit 0,
// scalar path, per decomposition. Produced by this tree's reference
// (graph_support.hpp), which is independent of the kernels, on x86-64 with
// GCC 13 on 2026-10-20; reproduced here on AArch64 by the CI job of that
// architecture. They change only when Descriptor::version does.

struct Golden {
  std::uint32_t accumulators;
  std::uint32_t distances;
  std::uint32_t ranks;
};

constexpr Golden kGolden[] = {
    {1, 0xD86C0152U, 0x44F21AD3U},
    {2, 0xD86C0152U, 0x7838F83AU},
    {4, 0xD86C0152U, 0x6B00CB77U},
    {8, 0xD86C0152U, 0x9F0E2AF6U},
};

TEST(GraphDeterminism, TheOutputMatchesTheDigestsRecordedForItsVersion) {
  const Unit unit;
  for (const Golden& golden : kGolden) {
    const Bytes reference = reference_output(unit.input, golden.accumulators);
    const Bytes output = run_serial(*workload_for(golden.accumulators), unit.input);
    ASSERT_EQ(output, reference);
    const std::uint32_t distances =
        core::crc32(std::span<const std::byte>(output).first(spec::kRanksOffset));
    const std::uint32_t ranks =
        core::crc32(std::span<const std::byte>(output).subspan(spec::kRanksOffset));
    EXPECT_EQ(distances, golden.distances) << golden.accumulators;
    EXPECT_EQ(ranks, golden.ranks) << golden.accumulators;
  }
}

}  // namespace
}  // namespace loadforge::graph_test
