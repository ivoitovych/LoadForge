// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/primitives/atomic: properties over generated cases, with real
// threads. The invariants are exact, so any lost or duplicated increment fails.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "primitives/atomic/atomic.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/atomic/executors.hpp"

namespace loadforge::primitives::atomic {
namespace {

using mocks::ThreadedExecutor;

constexpr std::uint64_t kCases = 60;

TEST(AtomicProperties, TotalAndTicketSumAreExactForAnyShape) {
  static constexpr std::uint32_t kLines[] = {8, 16, 64, 128, 256};
  for (std::uint64_t index = 0; index < kCases; ++index) {
    const rng::Block draw = rng::block_at(0xA70C, 1, index);
    const std::size_t threads = 1 + draw[0] % 7;
    const std::size_t units = draw[1] % 40;
    const std::uint32_t line = kLines[draw[2] % 5];
    const Config config{.mode = (draw[3] & 1U) != 0 ? Mode::kShared : Mode::kSharded,
                        .ordering = (draw[3] & 2U) != 0 ? Ordering::kWeakest : Ordering::kSeqCst,
                        .increments_per_unit = draw[3] % 500,
                        .line_bytes = line};
    ThreadedExecutor executor(threads);
    std::vector<std::atomic<std::uint64_t>> backing(storage_elements(line, threads).value() +
                                                    line / 8);
    const auto storage = aligned_storage(backing, line).value();
    std::vector<std::uint64_t> partials(units);
    const auto outcome = contend(executor, config, storage, partials);
    ASSERT_TRUE(outcome.has_value()) << "case " << index;
    const std::uint64_t total = units * config.increments_per_unit;
    EXPECT_EQ(outcome.value().total, total) << "case " << index;
    EXPECT_EQ(outcome.value().ticket_sum,
              config.mode == Mode::kShared ? expected_ticket_sum(total) : 0U)
        << "case " << index;
  }
}

TEST(AtomicProperties, EveryTicketIsDrawnOnceWhoeverDrawsIt) {
  // The per-unit sums differ from run to run, and their sum does not.
  ThreadedExecutor executor(7);
  std::vector<std::atomic<std::uint64_t>> backing(64 + 8);
  const auto storage = aligned_storage(backing, 64).value();
  const Config config{.increments_per_unit = 1500, .line_bytes = 64};
  std::vector<std::uint64_t> partials(16);
  for (int repeat = 0; repeat < 20; ++repeat) {
    const auto outcome = contend(executor, config, storage, partials);
    EXPECT_EQ(outcome.value().ticket_sum, expected_ticket_sum(std::uint64_t{16} * 1500));
  }
}

TEST(AtomicProperties, HandoffNeverShowsAStalePayloadForAnyShape) {
  for (std::uint64_t index = 0; index < kCases; ++index) {
    const rng::Block draw = rng::block_at(0xA70C, 2, index);
    ThreadedExecutor executor(1 + draw[0] % 7);
    std::vector<Cell> cells(draw[1] % 50);
    std::vector<Seen> seen(cells.size());
    const Ordering ordering = (draw[2] & 1U) != 0 ? Ordering::kWeakest : Ordering::kSeqCst;
    const HandoffOutcome outcome = handoff(executor, ordering, draw[3], cells, seen);
    EXPECT_EQ(outcome.mismatches, 0U) << "case " << index;
    EXPECT_EQ(outcome.published, cells.size()) << "case " << index;
  }
}

}  // namespace
}  // namespace loadforge::primitives::atomic
