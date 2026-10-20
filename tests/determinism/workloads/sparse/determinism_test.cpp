// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6, for the class BitExactFixedDecomposition (determinism.md §2, §5):
//
//   * for one chunk size and one ISA path, the output is byte-identical at 1,
//     2, 3, 4 and 7 threads, with indices called in either order, on repeated
//     runs, and whatever the scratch held;
//   * across chunk sizes the output may differ, and the difference is held to
//     the bound derived from the residual bound of the specification;
//   * the scalar path's output has one recorded digest per chunk size, the
//     same on every architecture: the portability workflow runs this test on
//     x86-64 and ARM64, which makes the scalar path's bit-identity across
//     them a failing test rather than a claim. A digest is a tripwire, not an
//     oracle (verification.md §4): the oracle tier is what says the numbers
//     are right.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/crc32.hpp"
#include "oracle/workloads/sparse/spec_check.hpp"
#include "oracle/workloads/sparse/unit_run.hpp"
#include "unit/workloads/sparse/executors.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {
namespace {

using tests::Bytes;

constexpr std::uint64_t kSeed = 0x5EED;
constexpr std::uint64_t kUnitIndex = 2;

struct Executors {
  sparse_tests::SerialExecutor serial;
  sparse_tests::ReverseExecutor reverse;
  sparse_tests::PoolExecutor one{1};
  sparse_tests::PoolExecutor two{2};
  sparse_tests::PoolExecutor three{3};
  sparse_tests::PoolExecutor four{4};
  sparse_tests::PoolExecutor seven{7};
};

TEST(SparseDeterminism, ForOneChunkSizeTheBytesAreTheSameAtAnyThreadCountAndOrder) {
  Executors executors;
  worker::Executor* const all[] = {&executors.reverse, &executors.one,  &executors.two,
                                   &executors.three,   &executors.four, &executors.seven};
  for (const std::uint32_t rows : kChunkRows) {
    const auto workload = tests::make_workload(kSeed, rows);
    ASSERT_NE(workload, nullptr);
    const Bytes input = tests::generate_input(*workload, kUnitIndex);
    const Bytes reference = tests::solve(*workload, kUnitIndex, input, executors.serial);
    EXPECT_EQ(tests::solve(*workload, kUnitIndex, input, executors.serial), reference)
        << "chunk rows " << rows << ": a repeat run";
    for (worker::Executor* executor : all) {
      EXPECT_EQ(tests::solve(*workload, kUnitIndex, input, *executor), reference)
          << "chunk rows " << rows << " at " << executor->threads() << " thread(s)";
      EXPECT_EQ(tests::solve(*workload, kUnitIndex, input, *executor, std::byte{0xA5}), reference)
          << "chunk rows " << rows << ", scratch and output pre-filled";
    }
  }
}

TEST(SparseDeterminism, ThreadsRepeatedlyRacingForChunksNeverChangeABit) {
  sparse_tests::PoolExecutor seven(7);
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(kSeed, 256);  // the most chunks: 64
  ASSERT_NE(workload, nullptr);
  const Bytes input = tests::generate_input(*workload, 0);
  const Bytes reference = tests::solve(*workload, 0, input, serial);
  for (int run = 0; run < 12; ++run) {
    ASSERT_EQ(tests::solve(*workload, 0, input, seven), reference) << "run " << run;
  }
}

TEST(SparseDeterminism, AcrossChunkSizesTheOutputsDifferByLessThanTheDerivedBound) {
  sparse_tests::SerialExecutor serial;
  std::vector<Bytes> outputs;
  Bytes input;
  for (const std::uint32_t rows : kChunkRows) {
    const auto workload = tests::make_workload(kSeed, rows);
    ASSERT_NE(workload, nullptr);
    input = tests::generate_input(*workload, kUnitIndex);
    outputs.push_back(tests::solve(*workload, kUnitIndex, input, serial));
  }
  double b_squares = 0.0;
  for (std::size_t i = 0; i < kRows; ++i) {
    const double b = check::f64_at(input, kRhsOffset, i);
    b_squares += b * b;
  }
  // Each iterate is within |r|_2 / lambda_min of the solution, and |r|_2 is at
  // most tau (derivation 2), so two iterates differ by at most 2 tau / (5/4).
  const double bound = 2.0 * residual_norm_factor() * std::sqrt(b_squares) / 1.25;
  bool any_difference = false;
  for (std::size_t a = 0; a < outputs.size(); ++a) {
    for (std::size_t b = a + 1; b < outputs.size(); ++b) {
      double squares = 0.0;
      for (std::size_t i = 0; i < kRows; ++i) {
        const double difference = check::f64_at(outputs[a], 0, i) - check::f64_at(outputs[b], 0, i);
        squares += difference * difference;
      }
      EXPECT_LE(std::sqrt(squares), bound) << kChunkRows[a] << " against " << kChunkRows[b];
      any_difference = any_difference || outputs[a] != outputs[b];
    }
    EXPECT_EQ(check::check(input, outputs[a]).verdict, check::Verdict::kAgreement);
  }
  EXPECT_TRUE(any_difference) << "the decomposition is part of the bits: that is the class";
}

// CRC-32 of the output of unit 2 of seed 0x5EED, for the chunk sizes 256, 512, 1024 and 2048.
// Recorded on 2026-10-20 by running the test below with zeros in this table and reading the
// values it reported, on x86-64 with GCC 13.3 (Linux 6.18, a 4-core virtual machine, debug build);
// the scalar path must reproduce them on ARM64 and under every compiler and optimisation level.
constexpr std::uint32_t kDigests[] = {231517602U, 77685606U, 2838486056U, 4027288572U};

TEST(SparseDeterminism, TheScalarOutputHasOneDigestPerChunkSizeOnEveryArchitecture) {
  sparse_tests::SerialExecutor serial;
  for (std::size_t i = 0; i < kChunkRows.size(); ++i) {
    const auto workload = tests::make_workload(kSeed, kChunkRows[i]);
    ASSERT_NE(workload, nullptr);
    const Bytes input = tests::generate_input(*workload, kUnitIndex);
    const Bytes output = tests::solve(*workload, kUnitIndex, input, serial);
    EXPECT_EQ(core::crc32(output), kDigests[i]) << "chunk rows " << kChunkRows[i];
  }
}

}  // namespace
}  // namespace loadforge::workloads::sparse
