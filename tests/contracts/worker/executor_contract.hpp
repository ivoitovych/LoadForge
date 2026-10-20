// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_WORKER_EXECUTOR_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_WORKER_EXECUTOR_CONTRACT_HPP

// Provider-side suite for src/worker/executor.hpp (ADR 0003, 0004).
//
// ExecutorContract<Subject>: Subject::make() returns a std::unique_ptr to an
// Executor ready to run. A consumer's single-threaded stand-in passes too: the
// suite asserts what the contract promises, and "at most threads() at once" is
// a promise a serial executor keeps.
//
// The ordering claims (each call's writes happen-before run returns) are
// asserted by reading plain, non-atomic memory after run(): under TSan, which
// CI runs, a provider that broke them is reported as a race.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "worker/executor.hpp"

namespace loadforge::contracts {

template <typename Subject>
class ExecutorContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(ExecutorContract);

TYPED_TEST_P(ExecutorContract, HasAtLeastOneThreadAndTheCountNeverChanges) {
  const std::unique_ptr<worker::Executor> executor = TypeParam::make();
  const std::size_t threads = executor->threads();
  EXPECT_GE(threads, 1U);
  auto nothing = [](std::size_t, std::size_t) noexcept {};
  worker::for_each_index(*executor, 5, nothing);
  EXPECT_EQ(executor->threads(), threads);
}

TYPED_TEST_P(ExecutorContract, ZeroTasksCallsNothing) {
  const std::unique_ptr<worker::Executor> executor = TypeParam::make();
  std::atomic<int> calls{0};
  auto count = [&calls](std::size_t, std::size_t) noexcept {
    calls.fetch_add(1, std::memory_order_seq_cst);
  };
  worker::for_each_index(*executor, 0, count);
  EXPECT_EQ(calls.load(std::memory_order_seq_cst), 0);
}

TYPED_TEST_P(ExecutorContract, EveryIndexExactlyOnceAndNoOther) {
  const std::unique_ptr<worker::Executor> executor = TypeParam::make();
  const std::size_t threads = executor->threads();
  for (const std::size_t count : {std::size_t{1}, threads, threads + 1, std::size_t{7},
                                  std::size_t{1000}}) {
    std::vector<std::atomic<int>> seen(count);
    std::atomic<int> out_of_range{0};
    auto mark = [&](std::size_t index, std::size_t) noexcept {
      if (index < seen.size()) {
        seen[index].fetch_add(1, std::memory_order_seq_cst);
      } else {
        out_of_range.fetch_add(1, std::memory_order_seq_cst);
      }
    };
    worker::for_each_index(*executor, count, mark);
    for (std::size_t index = 0; index < count; ++index) {
      EXPECT_EQ(seen[index].load(std::memory_order_seq_cst), 1)
          << "index " << index << " of " << count;
    }
    EXPECT_EQ(out_of_range.load(std::memory_order_seq_cst), 0);
  }
}

TYPED_TEST_P(ExecutorContract, NoTwoConcurrentCallsShareASlot) {
  const std::unique_ptr<worker::Executor> executor = TypeParam::make();
  const std::size_t threads = executor->threads();
  std::vector<std::atomic<bool>> busy(threads);
  std::atomic<int> collisions{0};
  std::atomic<int> out_of_range{0};
  auto occupy = [&](std::size_t, std::size_t slot) noexcept {
    if (slot >= busy.size()) {
      out_of_range.fetch_add(1, std::memory_order_seq_cst);
      return;
    }
    if (busy[slot].exchange(true, std::memory_order_seq_cst)) {
      collisions.fetch_add(1, std::memory_order_seq_cst);
    }
    // Hold the slot long enough for another thread to try it.
    volatile std::size_t spin = 0;
    for (std::size_t i = 0; i < 2000; ++i) {
      spin = spin + i;
    }
    busy[slot].store(false, std::memory_order_seq_cst);
  };
  worker::for_each_index(*executor, 64 * threads, occupy);
  EXPECT_EQ(out_of_range.load(std::memory_order_seq_cst), 0) << "a slot is below threads()";
  EXPECT_EQ(collisions.load(std::memory_order_seq_cst), 0);
}

TYPED_TEST_P(ExecutorContract, EveryWriteIsVisibleWhenRunReturns) {
  const std::unique_ptr<worker::Executor> executor = TypeParam::make();
  for (int round = 0; round < 50; ++round) {
    std::vector<std::size_t> results(257, 0);  // plain memory, on purpose
    auto write = [&results, round](std::size_t index, std::size_t) noexcept {
      results[index] = index * 3 + static_cast<std::size_t>(round);
    };
    worker::for_each_index(*executor, results.size(), write);
    for (std::size_t index = 0; index < results.size(); ++index) {
      ASSERT_EQ(results[index], index * 3 + static_cast<std::size_t>(round)) << "round " << round;
    }
  }
}

TYPED_TEST_P(ExecutorContract, TheRawInterfacePassesTheContextThrough) {
  const std::unique_ptr<worker::Executor> executor = TypeParam::make();
  std::vector<std::size_t> squares(10, 0);
  executor->run(
      squares.size(),
      [](void* context, std::size_t index, std::size_t) noexcept {
        (*static_cast<std::vector<std::size_t>*>(context))[index] = index * index;
      },
      &squares);
  for (std::size_t index = 0; index < squares.size(); ++index) {
    EXPECT_EQ(squares[index], index * index);
  }
}

REGISTER_TYPED_TEST_SUITE_P(ExecutorContract, HasAtLeastOneThreadAndTheCountNeverChanges,
                            ZeroTasksCallsNothing, EveryIndexExactlyOnceAndNoOther,
                            NoTwoConcurrentCallsShareASlot, EveryWriteIsVisibleWhenRunReturns,
                            TheRawInterfacePassesTheContextThrough);

}  // namespace loadforge::contracts

#endif
