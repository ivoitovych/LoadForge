// SPDX-License-Identifier: GPL-3.0-or-later
//
// P5: the pool's hand-off and completion protocol, tested claim by claim.
//
// THE SYNCHRONISATION ARGUMENT (repeated from thread_pool.cpp, where the code
// it argues for is; each claim is followed by the test of its consequence)
// ---------------------------------------------------------------------------
// One mutex guards everything a run publishes and everything a thread reports.
// The one atomic is the next index to claim, and every operation on it is
// seq_cst: nothing is weaker, so PLAN §4.5's rule for weaker orders is not
// owed, and what is tested here is the protocol itself.
//
//  1. A run's task, context and count reach every thread: written under the
//     mutex with the generation advanced, read under the mutex after the
//     generation is seen to move. The caller's release synchronises-with each
//     thread's acquire.
//       -> EachRunSeesItsOwnTaskAndContext
//
//  2. Every index in [0, count) runs exactly once, and no other: claims are
//     fetch_add on one atomic, whose RMWs read each the last one's value, and
//     every thread claims until it draws an index at or past count.
//       -> EveryIndexOnceAcrossManyRunsOfEveryShape
//
//  3. Everything each call wrote happens-before run returns: a thread's tasks
//     are sequenced before its increment of the finished count under the
//     mutex, and run returns only after seeing every thread's increment there.
//       -> PlainWritesAreVisibleWithNoSynchronisationOfTheCallers
//     (plain, non-atomic memory, read by the caller: under TSan, which CI
//     runs, a protocol that broke this is reported as a race, and without it
//     the values are checked)
//
//  4. No thread claims from one run with another run's task: a run returns
//     only when every thread has left its claim loop, and a thread re-enters
//     it only after the next run has reset the claim counter, under the mutex.
//       -> ALongRunFollowedAtOnceByAShortOneDoesNotBleed
//
//  5. Slots are unique among concurrent calls: a slot is a thread.
//       -> ASlotIsAlwaysTheSameThreadAndNeverTheCaller
//
// Each of these fails when the protocol is broken. Recorded in the journal
// fragment (docs/journal/08-executor.md): with run() returning before waiting
// for the finished count, 3 and 4 fail, and TSan reports the read in 3.

#include <gtest/gtest.h>
#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "support/fake_syscalls.hpp"
#include "unit/worker/executor/fakes.hpp"
#include "worker/executor.hpp"
#include "worker/executor/thread_pool.hpp"

namespace loadforge::worker {
namespace {

using testing::X86FpControl;

class Ordering : public ::testing::Test {
 protected:
  void SetUp() override {
    auto made = make_thread_pool(std::vector<ThreadPlacement>(kThreads), threads_, syscalls_, fp_);
    ASSERT_TRUE(made.has_value());
    executor_ = std::move(made).value();
  }

  static constexpr std::size_t kThreads = 4;
  PosixThreads threads_;
  loadforge::testing::FakeSyscalls syscalls_;
  X86FpControl fp_;
  std::unique_ptr<Executor> executor_;
};

TEST_F(Ordering, PlainWritesAreVisibleWithNoSynchronisationOfTheCallers) {
  std::vector<std::size_t> results;
  for (std::size_t round = 0; round < 2000; ++round) {
    const std::size_t count = 1 + round % (3 * kThreads);
    results.assign(count, 0);  // plain memory, on purpose
    auto write = [&results, round](std::size_t index, std::size_t) noexcept {
      results[index] = index * 7 + round;
    };
    for_each_index(*executor_, count, write);
    for (std::size_t index = 0; index < count; ++index) {
      ASSERT_EQ(results[index], index * 7 + round) << "round " << round << " index " << index;
    }
  }
}

TEST_F(Ordering, EachRunSeesItsOwnTaskAndContext) {
  // Two tasks and two contexts, alternating: a thread that ran this run's
  // index with the last run's task or context would write the wrong mark into
  // the wrong vector.
  std::vector<int> first(64, 0);
  std::vector<int> second(64, 0);
  auto mark_first = [](void* context, std::size_t index, std::size_t) noexcept {
    (*static_cast<std::vector<int>*>(context))[index] += 1;
  };
  auto mark_second = [](void* context, std::size_t index, std::size_t) noexcept {
    (*static_cast<std::vector<int>*>(context))[index] += 100;
  };
  constexpr int kRounds = 500;
  for (int round = 0; round < kRounds; ++round) {
    executor_->run(first.size(), mark_first, &first);
    executor_->run(second.size(), mark_second, &second);
  }
  for (std::size_t index = 0; index < first.size(); ++index) {
    ASSERT_EQ(first[index], kRounds) << index;
    ASSERT_EQ(second[index], 100 * kRounds) << index;
  }
}

TEST_F(Ordering, EveryIndexOnceAcrossManyRunsOfEveryShape) {
  // Fewer indices than threads, as many, one more, and many more: the shapes
  // in which a thread draws nothing, exactly one, or many.
  for (std::size_t count = 1; count <= 8 * kThreads + 1; ++count) {
    for (int round = 0; round < 50; ++round) {
      std::vector<std::atomic<int>> seen(count);
      std::atomic<int> outside{0};
      auto mark = [&](std::size_t index, std::size_t) noexcept {
        if (index < seen.size()) {
          seen[index].fetch_add(1, std::memory_order_seq_cst);
        } else {
          outside.fetch_add(1, std::memory_order_seq_cst);
        }
      };
      for_each_index(*executor_, count, mark);
      for (std::size_t index = 0; index < count; ++index) {
        ASSERT_EQ(seen[index].load(std::memory_order_seq_cst), 1) << count << ":" << index;
      }
      ASSERT_EQ(outside.load(std::memory_order_seq_cst), 0) << count;
    }
  }
}

TEST_F(Ordering, ALongRunFollowedAtOnceByAShortOneDoesNotBleed) {
  // The long run leaves every thread busy until its last index; the short one
  // starts the instant it returns. A thread still in the long run's claim loop
  // would draw index 0 of the short run and run it with the long run's task,
  // counting it in `wide` and missing it in `narrow`.
  std::vector<int> wide(4096, 0);
  std::vector<int> narrow(1, 0);
  for (int round = 0; round < 200; ++round) {
    auto into_wide = [&wide](std::size_t index, std::size_t) noexcept { wide[index] += 1; };
    auto into_narrow = [&narrow](std::size_t index, std::size_t) noexcept { narrow[index] += 1; };
    for_each_index(*executor_, wide.size(), into_wide);
    for_each_index(*executor_, narrow.size(), into_narrow);
    ASSERT_EQ(narrow[0], round + 1);
  }
  for (std::size_t index = 0; index < wide.size(); ++index) {
    ASSERT_EQ(wide[index], 200) << index;
  }
}

TEST_F(Ordering, ASlotIsAlwaysTheSameThreadAndNeverTheCaller) {
  // A property of this pool beyond the contract's "no two at once", and the
  // one a placement means anything by: slot s is thread s, the thread that was
  // placed by placements[s]. The caller waits and runs nothing.
  std::vector<pthread_t> owner(kThreads);
  std::vector<std::atomic<int>> mismatches(kThreads);
  std::vector<std::atomic<bool>> claimed(kThreads);
  const pthread_t caller = pthread_self();
  std::atomic<int> on_caller{0};
  auto record = [&](std::size_t, std::size_t slot) noexcept {
    const pthread_t self = pthread_self();
    if (pthread_equal(self, caller) != 0) {
      on_caller.fetch_add(1, std::memory_order_seq_cst);
    }
    if (!claimed[slot].exchange(true, std::memory_order_seq_cst)) {
      owner[slot] = self;  // the slot's own thread, and only it, writes here
    } else if (pthread_equal(owner[slot], self) == 0) {
      mismatches[slot].fetch_add(1, std::memory_order_seq_cst);
    }
  };
  for (int round = 0; round < 200; ++round) {
    for_each_index(*executor_, 10 * kThreads, record);
  }
  EXPECT_EQ(on_caller.load(std::memory_order_seq_cst), 0);
  for (std::size_t slot = 0; slot < kThreads; ++slot) {
    EXPECT_EQ(mismatches[slot].load(std::memory_order_seq_cst), 0) << slot;
    // Which threads drew an index at all is the scheduler's business: a slot
    // that drew none in two hundred runs has no owner to compare.
    for (std::size_t other = 0; other < slot; ++other) {
      if (claimed[slot].load(std::memory_order_seq_cst) &&
          claimed[other].load(std::memory_order_seq_cst)) {
        EXPECT_EQ(pthread_equal(owner[slot], owner[other]), 0) << slot << " and " << other;
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::worker
