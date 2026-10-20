// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for src/primitives/atomic, declared BitExact (docs/determinism.md §5) for
// what is deterministic: the counter total and the sum of tickets. Which unit
// drew which ticket is not deterministic and is deliberately not compared (nor
// asserted to differ: that would be a test of the scheduler, and flaky).

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "primitives/atomic/atomic.hpp"
#include "unit/primitives/atomic/executors.hpp"

namespace loadforge::primitives::atomic {
namespace {

using mocks::ThreadedExecutor;

Outcome outcome_at(std::size_t threads, const Config& config, std::size_t units) {
  ThreadedExecutor executor(threads);
  std::vector<std::atomic<std::uint64_t>> backing(
      storage_elements(config.line_bytes, threads).value() + config.line_bytes / 8);
  const auto storage = aligned_storage(backing, config.line_bytes).value();
  std::vector<std::uint64_t> partials(units);
  return contend(executor, config, storage, partials).value();
}

TEST(AtomicDeterminism, TotalsAreByteIdenticalAcrossThreadCountsModesAndOrderings) {
  for (const Mode mode : {Mode::kShared, Mode::kSharded}) {
    for (const Ordering ordering : {Ordering::kWeakest, Ordering::kSeqCst}) {
      const Config config{.mode = mode, .ordering = ordering, .increments_per_unit = 1237,
                          .line_bytes = 64};
      const Outcome one = outcome_at(1, config, 23);
      for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
        EXPECT_EQ(outcome_at(threads, config, 23), one)
            << threads << " threads, mode " << static_cast<int>(mode);
      }
    }
  }
}

TEST(AtomicDeterminism, IdenticalAcrossRepeatRunsAndLineSizes) {
  Config config{.increments_per_unit = 900, .line_bytes = 64};
  const Outcome first = outcome_at(7, config, 17);
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(outcome_at(7, config, 17), first);
  }
  for (const std::uint32_t line : {8U, 16U, 128U, 256U}) {
    config.line_bytes = line;
    EXPECT_EQ(outcome_at(7, config, 17), first) << "line " << line;
  }
}

}  // namespace
}  // namespace loadforge::primitives::atomic
