// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/primitives/atomic: configuration, alignment, the contention
// load's exact totals at every mode and ordering, the handoff's protocol (P5),
// and the executor contract run against this module's mock.

#include "primitives/atomic/atomic.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "unit/primitives/atomic/executors.hpp"

namespace loadforge::contracts {
using AtomicMockExecutors = ::testing::Types<primitives::atomic::mocks::SerialSubject,
                                             primitives::atomic::mocks::ThreadedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(AtomicMocks, ExecutorContract, AtomicMockExecutors);
}  // namespace loadforge::contracts

namespace loadforge::primitives::atomic {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

constexpr std::uint32_t kLine = 64;

/// Counters for `slots` slots of `line` bytes, with room to align the start.
struct Storage {
  Storage(std::size_t slots, std::uint32_t line)
      : backing(storage_elements(line, slots).value() + line / 8),
        counters(aligned_storage(backing, line).value()) {}
  std::vector<std::atomic<std::uint64_t>> backing;
  std::span<std::atomic<std::uint64_t>> counters;
};

// --- configuration ------------------------------------------------------------

TEST(AtomicConfig, IsDeclaredBitExact) {
  EXPECT_EQ(kDeterminism, workloads::DeterminismClass::kBitExact);
}

TEST(AtomicConfig, OneLineOfCountersPerSlot) {
  EXPECT_EQ(storage_elements(64, 3).value(), 24U);
  EXPECT_EQ(storage_elements(128, 2).value(), 32U);
  EXPECT_EQ(storage_elements(8, 5).value(), 5U);
}

TEST(AtomicConfig, ABadLineSizeIsRefusedByEveryEntryPoint) {
  for (const std::uint32_t line : {0U, 4U, 12U, 100U, kMaxLineBytes + 8}) {
    EXPECT_EQ(storage_elements(line, 2).error(), ConfigError::kLineBytesInvalid) << line;
    std::vector<std::atomic<std::uint64_t>> counters(8);
    EXPECT_EQ(aligned_storage(counters, line).error(), ConfigError::kLineBytesInvalid) << line;
  }
  EXPECT_TRUE(storage_elements(kMaxLineBytes, 1).has_value());
  EXPECT_TRUE(storage_elements(8, 1).has_value());
}

TEST(AtomicConfig, AlignedStorageStartsOnALineBoundaryWhateverTheBackingAddress) {
  std::vector<std::atomic<std::uint64_t>> backing(64);
  for (std::size_t offset = 0; offset < 8; ++offset) {
    const std::span<std::atomic<std::uint64_t>> shifted = std::span(backing).subspan(offset);
    const auto aligned = aligned_storage(shifted, kLine);
    ASSERT_TRUE(aligned.has_value());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned.value().data()) % kLine, 0U) << offset;
    EXPECT_LT(shifted.size() - aligned.value().size(), 8U) << "at most a line's worth is dropped";
  }
}

TEST(AtomicConfig, AlignedStorageNeverSkipsMoreThanItHas) {
  std::vector<std::atomic<std::uint64_t>> backing(8);
  for (std::size_t offset = 0; offset < 8; ++offset) {
    const auto aligned = aligned_storage(std::span(backing).subspan(offset, 1), 4096);
    ASSERT_TRUE(aligned.has_value());
    EXPECT_LE(aligned.value().size(), 1U);
  }
}

// --- the ticket sum -----------------------------------------------------------

TEST(AtomicTickets, SmallTotalsAreTheTriangularNumbers) {
  const std::uint64_t expected[] = {0, 0, 1, 3, 6, 10, 15, 21, 28, 36, 45};
  for (std::uint64_t total = 0; total <= 10; ++total) {
    EXPECT_EQ(expected_ticket_sum(total), expected[total]) << total;
  }
}

TEST(AtomicTickets, LargeTotalsWrapModuloTwoToTheSixtyFour) {
  const std::uint64_t two32 = std::uint64_t{1} << 32U;
  EXPECT_EQ(expected_ticket_sum(two32), (std::uint64_t{1} << 31U) * (two32 - 1));
  // 2^33 * (2^33 - 1) / 2 = 2^32 * (2^33 - 1), which exceeds 2^64: wraps to
  // 2^32 * (2^33 - 1) mod 2^64 = 2^65 - 2^32 mod 2^64 = 2^64 - 2^32.
  EXPECT_EQ(expected_ticket_sum(std::uint64_t{1} << 33U), ~std::uint64_t{0} - (two32 - 1));
  EXPECT_EQ(expected_ticket_sum(~std::uint64_t{0}),
            (~std::uint64_t{0}) * ((~std::uint64_t{0} - 1) / 2))
      << "an odd total halves the other factor";
}

// --- contention ---------------------------------------------------------------

TEST(AtomicContend, EachRefusalIsNamedAndRunsNothing) {
  ThreadedExecutor executor(3);
  std::vector<std::uint64_t> partials(4, 0xAAAA);
  const Storage storage(3, kLine);

  const auto bad_line = contend(executor, Config{.line_bytes = 12}, storage.counters, partials);
  EXPECT_EQ(bad_line.error(), ConfigError::kLineBytesInvalid);

  const auto small = contend(executor, Config{.increments_per_unit = 5, .line_bytes = kLine},
                             storage.counters.first(3 * 8 - 1), partials);  // one counter short
  EXPECT_EQ(small.error(), ConfigError::kStorageTooSmall);

  const auto misaligned = contend(executor, Config{.increments_per_unit = 5, .line_bytes = kLine},
                                  storage.counters.subspan(1, std::size_t{3} * 8), partials);
  EXPECT_EQ(misaligned.error(), ConfigError::kStorageMisaligned);

  EXPECT_EQ(partials, std::vector<std::uint64_t>(4, 0xAAAA)) << "no unit ran";
}

TEST(AtomicContend, ExactlyEnoughStorageIsEnough) {
  ThreadedExecutor executor(3);
  std::vector<std::uint64_t> partials(2);
  const Storage storage(3, kLine);
  EXPECT_TRUE(contend(executor, Config{.increments_per_unit = 1, .line_bytes = kLine},
                      storage.counters.first(std::size_t{3} * 8), partials)
                  .has_value());
}

TEST(AtomicContend, NoUnitsOrNoIncrementsIsAnEmptyOutcome) {
  SerialExecutor executor;
  const Storage storage(1, kLine);
  std::vector<std::uint64_t> none;
  EXPECT_EQ(contend(executor, Config{.increments_per_unit = 99, .line_bytes = kLine},
                    storage.counters, none)
                .value(),
            Outcome{});
  std::vector<std::uint64_t> three(3, 7);
  EXPECT_EQ(contend(executor, Config{.increments_per_unit = 0, .line_bytes = kLine},
                    storage.counters, three)
                .value(),
            Outcome{});
}

TEST(AtomicContend, EveryModeAndOrderingLosesNoIncrementAtAnyThreadCount) {
  for (const Mode mode : {Mode::kShared, Mode::kSharded}) {
    for (const Ordering ordering : {Ordering::kWeakest, Ordering::kSeqCst}) {
      for (const std::size_t threads : {1U, 2U, 3U, 7U}) {
        ThreadedExecutor executor(threads);
        const Storage storage(threads, kLine);
        std::vector<std::uint64_t> partials(11);
        const Config config{
            .mode = mode, .ordering = ordering, .increments_per_unit = 2000, .line_bytes = kLine};
        const auto outcome = contend(executor, config, storage.counters, partials);
        ASSERT_TRUE(outcome.has_value());
        const std::uint64_t total = std::uint64_t{11} * 2000;
        EXPECT_EQ(outcome.value().total, total)
            << "mode " << static_cast<int>(mode) << " ordering " << static_cast<int>(ordering)
            << " threads " << threads;
        EXPECT_EQ(outcome.value().ticket_sum,
                  mode == Mode::kShared ? expected_ticket_sum(total) : 0U)
            << "every ticket 0..total-1 handed out exactly once";
      }
    }
  }
}

TEST(AtomicContend, ShardedUsesOneCounterPerSlotAndTouchesNothingBetween) {
  ThreadedExecutor executor(3);
  Storage storage(3, kLine);
  for (auto& word : storage.counters) {
    word.store(0xFEEDFACEFEEDFACEULL, std::memory_order_relaxed);
  }
  std::vector<std::uint64_t> partials(9);
  const Config config{.mode = Mode::kSharded, .increments_per_unit = 100, .line_bytes = kLine};
  ASSERT_TRUE(contend(executor, config, storage.counters, partials).has_value());
  std::uint64_t slot_sum = 0;
  for (std::size_t word = 0; word < 24; ++word) {
    const std::uint64_t value = storage.counters[word].load(std::memory_order_relaxed);
    if (word % 8 == 0) {
      slot_sum += value;
    } else {
      EXPECT_EQ(value, 0xFEEDFACEFEEDFACEULL) << "word " << word << " is between two counters";
    }
  }
  EXPECT_EQ(slot_sum, 900U);
}

TEST(AtomicContend, ASecondRunStartsFromZero) {
  ThreadedExecutor executor(2);
  const Storage storage(2, kLine);
  std::vector<std::uint64_t> partials(4);
  const Config config{.increments_per_unit = 300, .line_bytes = kLine};
  const auto first = contend(executor, config, storage.counters, partials);
  const auto second = contend(executor, config, storage.counters, partials);
  EXPECT_EQ(first.value(), second.value());
  EXPECT_EQ(second.value().total, 1200U);
}

// --- handoff ------------------------------------------------------------------

TEST(AtomicHandoff, PayloadsAreNeverZeroAndDifferByUnitAndSeed) {
  EXPECT_NE(payload_of(1, 0), 0U);
  EXPECT_NE(payload_of(1, 0), payload_of(1, 1));
  EXPECT_NE(payload_of(1, 0), payload_of(2, 0));
  EXPECT_EQ(payload_of(1, 5), payload_of(1, 5));
}

TEST(AtomicHandoff, ASerialRunSeesExactlyTheEarlierUnits) {
  // One unit at a time, so unit i finds flags 0..i-1 set and none after: the
  // only run whose `observed` is a known number, and the protocol's simplest case.
  for (const Ordering ordering : {Ordering::kWeakest, Ordering::kSeqCst}) {
    SerialExecutor executor;
    std::vector<Cell> cells(10);
    std::vector<Seen> seen(10);
    const HandoffOutcome outcome = handoff(executor, ordering, 99, cells, seen);
    EXPECT_EQ(outcome.observed, 45U);
    EXPECT_EQ(outcome.mismatches, 0U);
    EXPECT_EQ(outcome.published, 10U);
    for (std::size_t i = 0; i < 10; ++i) {
      EXPECT_EQ(seen[i].observed, i);
      EXPECT_EQ(cells[i].payload, payload_of(99, i));
    }
  }
}

TEST(AtomicHandoff, ConcurrentUnitsNeverReadAnUnpublishedPayload) {
  for (const Ordering ordering : {Ordering::kWeakest, Ordering::kSeqCst}) {
    ThreadedExecutor executor(7);
    std::vector<Cell> cells(64);
    std::vector<Seen> seen(64);
    for (int round = 0; round < 200; ++round) {
      const HandoffOutcome outcome =
          handoff(executor, ordering, static_cast<std::uint64_t>(round), cells, seen);
      ASSERT_EQ(outcome.mismatches, 0U) << "round " << round;
      ASSERT_EQ(outcome.published, 64U);
      ASSERT_LE(outcome.observed, 64U * 63U);
    }
  }
}

TEST(AtomicHandoff, AReusedCellStartsUnpublishedAgain) {
  SerialExecutor executor;
  std::vector<Cell> cells(4);
  std::vector<Seen> seen(4);
  (void)handoff(executor, Ordering::kWeakest, 1, cells, seen);
  const HandoffOutcome again = handoff(executor, Ordering::kWeakest, 2, cells, seen);
  EXPECT_EQ(again.observed, 6U) << "the first run's flags did not carry over";
  EXPECT_EQ(again.mismatches, 0U);
}

TEST(AtomicHandoff, NoCellsIsAnEmptyOutcome) {
  SerialExecutor executor;
  std::vector<Cell> none;
  std::vector<Seen> no_seen;
  const HandoffOutcome outcome = handoff(executor, Ordering::kSeqCst, 1, none, no_seen);
  EXPECT_EQ(outcome.observed, 0U);
  EXPECT_EQ(outcome.mismatches, 0U);
  EXPECT_EQ(outcome.published, 0U);
}

TEST(AtomicHandoff, TheReaderCountsAWrongPayloadAndAnObservation) {
  // The check has to be able to fail: a healthy run never takes this arm, so
  // the reader's half is exercised directly, with the value a stale read would
  // have returned.
  Seen seen;
  record(seen, 0x1234, 0x1234);
  EXPECT_EQ(seen.observed, 1U);
  EXPECT_EQ(seen.mismatches, 0U);
  record(seen, 0, payload_of(5, 0));  // the unwritten payload
  record(seen, payload_of(5, 0) ^ 1U, payload_of(5, 0));
  EXPECT_EQ(seen.observed, 3U);
  EXPECT_EQ(seen.mismatches, 2U);
}

}  // namespace
}  // namespace loadforge::primitives::atomic
