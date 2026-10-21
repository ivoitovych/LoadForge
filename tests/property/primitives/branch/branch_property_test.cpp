// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/primitives/branch: properties over generated cases.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "primitives/branch/branch.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/branch/executors.hpp"

namespace loadforge::primitives::branch {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

constexpr std::uint64_t kCases = 150;

Config case_config(std::uint64_t index, bool sorted) {
  const rng::Block draw = rng::block_at(0xB4A4C, 1, index);
  return Config{.seed = rng::u64_of(draw),
                .elements = draw[2] % 300,
                .threshold = rng::u64_of(rng::block_at(0xB4A4C, 2, index)),
                .sorted = sorted};
}

TEST(BranchProperties, SortedAndUnsortedGiveOneTally) {
  for (std::uint64_t index = 0; index < kCases; ++index) {
    const Config config = case_config(index, false);
    const Config sorted = case_config(index, true);
    std::vector<std::uint64_t> scratch(config.elements);
    EXPECT_EQ(unit_tally(config, index, scratch), unit_tally(sorted, index, scratch))
        << "case " << index;
  }
}

TEST(BranchProperties, TakenNeverExceedsTheElementsAndRaisingTheThresholdNeverLowersIt) {
  for (std::uint64_t index = 0; index < kCases; ++index) {
    Config config = case_config(index, false);
    std::vector<std::uint64_t> scratch(config.elements);
    const std::uint64_t at = unit_tally(config, 0, scratch).taken;
    EXPECT_LE(at, config.elements);
    config.threshold += (~std::uint64_t{0} - config.threshold) / 2;
    const std::uint64_t higher = unit_tally(config, 0, scratch).taken;
    EXPECT_GE(higher, at) << "case " << index;
  }
}

TEST(BranchProperties, SplittingTheUnitsAcrossExecutorsGivesOneTotal) {
  for (std::uint64_t index = 0; index < kCases / 5; ++index) {
    const Config config = case_config(index, index % 2 == 0);
    const std::size_t units = 1 + index % 8;
    const std::size_t threads = 1 + index % 5;
    SerialExecutor serial;
    ThreadedExecutor threaded(threads);
    std::vector<std::uint64_t> serial_scratch(config.elements);
    std::vector<std::uint64_t> threaded_scratch(threads * config.elements);
    std::vector<Tally> a(units);
    std::vector<Tally> b(units);
    EXPECT_EQ(run(serial, config, serial_scratch, a).value(),
              run(threaded, config, threaded_scratch, b).value());
    EXPECT_EQ(a, b) << "case " << index;
  }
}

}  // namespace
}  // namespace loadforge::primitives::branch
