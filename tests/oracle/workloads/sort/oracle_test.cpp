// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for src/workloads/sort: the sort's output is checked by a permutation
// check and an order check written from the specification (check.hpp), never
// by sorting the input a second time (verification.md §3).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "oracle/workloads/sort/check.hpp"
#include "unit/workloads/sort/executors.hpp"
#include "unit/workloads/sort/harness.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::workloads::sort::testing {
namespace {

using checking::Verdict;

void expect_agreement(const Bytes& input, const Bytes& output, const std::string& what) {
  const checking::Outcome outcome = checking::check(input, output);
  EXPECT_EQ(outcome.verdict, Verdict::kAgreement) << what << ": position " << outcome.position;
  EXPECT_EQ(outcome.blocks_compared, kKeys) << what;
}

TEST(SortOracle, EveryDecompositionSortsGeneratedUnitsRight) {
  SerialExecutor serial;
  ThreadedExecutor three(3);
  for (const std::uint64_t seed : {std::uint64_t{1}, ~std::uint64_t{0}}) {
    for (const std::uint32_t chunks : kChunkCounts) {
      const auto workload = make(chunks, seed);
      ASSERT_NE(workload, nullptr);
      for (const std::uint64_t unit : {seed == 1 ? std::uint64_t{0} : std::uint64_t{7}}) {
        const Bytes input = generate(*workload, unit);
        const std::string what = "seed " + std::to_string(seed) + " unit " + std::to_string(unit) +
                                 " P " + std::to_string(chunks);
        expect_agreement(input, sorted(*workload, unit, input, serial), what + " serial");
        expect_agreement(input, sorted(*workload, unit, input, three, std::byte{0xA5}),
                         what + " three threads");
      }
    }
  }
}

TEST(SortOracle, InputsWithTiesAreSortedByPosition) {
  // A key space of 64 values over 131072 keys: every key repeats about 2000 times.
  SerialExecutor serial;
  const auto workload = make();
  Bytes input = generate(*workload, 3);
  for (std::size_t position = 0; position < kKeys; ++position) {
    put_key(input, position, key_of(input, position) & 63U);
  }
  expect_agreement(input, sorted(*workload, 3, input, serial), "64 distinct keys");
}

TEST(SortOracle, TheCheckerAgreesWithTheKnownOutputOfAFormula) {
  // Keys e * 40503 mod 2^17 -> the output is written down: key r at rank r,
  // carrying input position r * 96135 mod 2^17. The checker must agree with
  // the formula as well as with the sort, or one of them is wrong.
  const Bytes input = input_of([](std::size_t e) { return (e * 40503U) & (kKeys - 1); });
  Bytes formula(kOutputBytes);
  for (std::size_t rank = 0; rank < kKeys; ++rank) {
    put_key(formula, rank, rank);
    const std::uint64_t position = (rank * 96135U) & (kKeys - 1);
    for (std::size_t byte = 0; byte < kIndexBytes; ++byte) {
      formula[index_offset(rank) + byte] =
          static_cast<std::byte>((position >> (8U * byte)) & 0xFFU);
    }
  }
  expect_agreement(input, formula, "the formula's output");
  SerialExecutor serial;
  const auto workload = make();
  EXPECT_EQ(sorted(*workload, 0, input, serial), formula);
}

}  // namespace
}  // namespace loadforge::workloads::sort::testing
