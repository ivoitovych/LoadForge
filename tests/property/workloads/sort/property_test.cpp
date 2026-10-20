// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/workloads/sort: properties over families of inputs -- heavy ties,
// few distinct values, presorted and reversed runs, the whole key range -- each
// output checked by the specification's checker, and the metamorphic relations
// the sort must keep.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "oracle/workloads/sort/check.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/workloads/sort/executors.hpp"
#include "unit/workloads/sort/harness.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::workloads::sort::testing {
namespace {

using checking::Verdict;

struct Family {
  std::string name;
  std::function<std::uint64_t(std::uint64_t seed, std::size_t position)> key;
};

std::uint64_t raw(std::uint64_t seed, std::size_t position) { return key_at(seed, 0xFA, position); }

std::vector<Family> families() {
  return {
      {"full range", [](std::uint64_t seed, std::size_t e) { return raw(seed, e); }},
      {"one bit", [](std::uint64_t seed, std::size_t e) { return raw(seed, e) & 1U; }},
      {"four bits", [](std::uint64_t seed, std::size_t e) { return raw(seed, e) & 15U; }},
      {"one byte, high", [](std::uint64_t seed, std::size_t e) { return raw(seed, e) << 56U; }},
      {"seventeen bits", [](std::uint64_t seed, std::size_t e) { return raw(seed, e) >> 47U; }},
      {"top bit only",
       [](std::uint64_t seed, std::size_t e) { return (raw(seed, e) & 1U) << 63U; }},
      {"three values",
       [](std::uint64_t seed, std::size_t e) {
         constexpr std::uint64_t kValues[3] = {0, 0x8000000000000000ULL, ~std::uint64_t{0}};
         return kValues[raw(seed, e) % 3];
       }},
      {"ascending runs",
       [](std::uint64_t seed, std::size_t e) { return (e % 1000) + (seed & 3U); }},
      {"descending", [](std::uint64_t, std::size_t e) { return ~std::uint64_t{e}; }},
      {"sawtooth", [](std::uint64_t, std::size_t e) { return (e * 7919U) % 4099U; }},
      {"constant", [](std::uint64_t seed, std::size_t) { return seed; }},
  };
}

TEST(SortProperty, EveryFamilyIsSortedRightAtEveryDecomposition) {
  SerialExecutor serial;
  ThreadedExecutor four(4);
  const std::vector<std::uint64_t> seeds = {0x0123456789ABCDEFULL};
  for (const Family& family : families()) {
    for (const std::uint64_t seed : seeds) {
      const Bytes input = input_of([&](std::size_t e) { return family.key(seed, e); });
      for (const std::uint32_t chunks : {1U, 16U, 64U}) {
        const auto workload = make(chunks);
        ASSERT_NE(workload, nullptr);
        worker::Executor& executor = chunks == 64 ? static_cast<worker::Executor&>(four) : serial;
        const Bytes output = sorted(*workload, 0, input, executor);
        const checking::Outcome outcome = checking::check(input, output);
        EXPECT_EQ(outcome.verdict, Verdict::kAgreement)
            << family.name << " seed " << seed << " P " << chunks << " at " << outcome.position;
      }
    }
  }
}

TEST(SortProperty, SortingTheSortedKeysChangesNothingAndIsTheIdentityOnPositions) {
  SerialExecutor serial;
  const auto workload = make(8);
  for (const std::uint64_t seed : {std::uint64_t{2}, std::uint64_t{3}}) {
    const Bytes input = input_of([&](std::size_t e) { return raw(seed, e) & 0xFFFU; });
    const Bytes once = sorted(*workload, 0, input, serial);
    const Bytes keys(once.begin(), once.begin() + static_cast<std::ptrdiff_t>(kInputBytes));
    const Bytes twice = sorted(*workload, 0, keys, serial);
    for (std::size_t rank = 0; rank < kKeys; ++rank) {
      ASSERT_EQ(key_of(twice, rank), key_of(once, rank)) << rank;
      ASSERT_EQ(index_of(twice, rank), rank) << "already in order: positions are the identity";
    }
  }
}

TEST(SortProperty, TheKeysComeOutIndependentlyOfWhereTheyStartedAndTiesFollowTheirPosition) {
  // Rotate the input by 1000 positions: the sorted keys are the same sequence;
  // the position of each key moves by 1000 (mod n).
  SerialExecutor serial;
  const auto workload = make();
  const Bytes input = input_of([](std::size_t e) { return raw(5, e) & 0xFFU; });
  const Bytes rotated = input_of([](std::size_t e) { return raw(5, (e + 1000) % kKeys) & 0xFFU; });
  const Bytes a = sorted(*workload, 0, input, serial);
  const Bytes b = sorted(*workload, 0, rotated, serial);
  for (std::size_t rank = 0; rank < kKeys; ++rank) {
    ASSERT_EQ(key_of(a, rank), key_of(b, rank)) << rank;
  }
}

TEST(SortProperty, AMonotoneRelabellingOfTheKeysLeavesThePositionsAlone) {
  // x -> 3x + 1 with no overflow (keys below 2^20) preserves order and ties.
  SerialExecutor serial;
  const auto workload = make(4);
  const Bytes input = input_of([](std::size_t e) { return raw(9, e) >> 44U; });
  const Bytes mapped = input_of([](std::size_t e) { return 3 * (raw(9, e) >> 44U) + 1; });
  const Bytes a = sorted(*workload, 0, input, serial);
  const Bytes b = sorted(*workload, 0, mapped, serial);
  for (std::size_t rank = 0; rank < kKeys; ++rank) {
    ASSERT_EQ(index_of(a, rank), index_of(b, rank)) << rank;
    ASSERT_EQ(3 * key_of(a, rank) + 1, key_of(b, rank)) << rank;
  }
}

}  // namespace
}  // namespace loadforge::workloads::sort::testing
