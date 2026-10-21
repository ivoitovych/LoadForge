// SPDX-License-Identifier: GPL-3.0-or-later
//
// T10: millions of runs on one pool, with no growth and no lost index.
//
// What a soak can see that a unit test cannot: a protocol that loses one index
// in a million runs, a run that allocates and so grows a long worker's heap, a
// pool that leaks a thread per run. So every run here is checked -- each
// index exactly once, every slot in range -- and around the whole soak the
// process's allocations and threads are counted, before and after.
//
// "No growth" is measured, not inferred: this binary replaces the global
// allocation functions with counting ones, and the soak asserts that the runs
// made no allocation at all. The count is taken where the replacement is
// possible -- not under AddressSanitizer or ThreadSanitizer, whose runtimes
// own the allocator -- and the threads, read from the kernel's own record in
// /proc/self/status, are counted under every build.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "support/fake_syscalls.hpp"
#include "unit/worker/executor/fakes.hpp"
#include "worker/executor.hpp"
#include "worker/executor/thread_pool.hpp"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LOADFORGE_SOAK_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LOADFORGE_SOAK_SANITIZED 1
#endif
#endif

namespace {

// The replaceable operator new has to bump one process-wide counter.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): must be mutable global state
std::atomic<std::uint64_t> allocations{0};

}  // namespace

#if !defined(LOADFORGE_SOAK_SANITIZED)
// The replaceable allocation functions ([new.delete]): every form that
// allocates funnels into the two counted ones, and every deallocation into
// free(). Defined for this test binary only.
void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_seq_cst);
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {  // NOLINT
    return memory;
  }
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  allocations.fetch_add(1, std::memory_order_seq_cst);
  const auto align = static_cast<std::size_t>(alignment);
  const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
  if (void* memory = std::aligned_alloc(align, rounded)) {  // NOLINT
    return memory;
  }
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void* memory) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete[](void* memory) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete(void* memory, std::size_t /*size*/) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete[](void* memory, std::size_t /*size*/) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  std::free(memory);  // NOLINT
}
void operator delete[](void* memory, std::size_t /*size*/,
                       std::align_val_t /*alignment*/) noexcept {
  std::free(memory);  // NOLINT
}
#endif

namespace loadforge::worker {
namespace {

using testing::X86FpControl;

/// The kernel's count of this process's threads.
long threads_now() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with("Threads:")) {
      return std::stol(line.substr(8));
    }
  }
  return -1;
}

// A million runs, under every build: the time is the kernel's futex calls,
// which the sanitizers barely touch (a hundred thousand runs took 7.7 s in the
// debug build, 9 s under TSan and 8 s under ASan; see the journal fragment).
// LOADFORGE_SOAK_RUNS overrides it, for a longer soak by hand or a shorter one
// repeated.
constexpr std::uint64_t kDefaultRuns = 1'000'000;

std::uint64_t runs() {
  const char* text = std::getenv("LOADFORGE_SOAK_RUNS");  // NOLINT(concurrency-mt-unsafe)
  if (text == nullptr) {
    return kDefaultRuns;
  }
  return std::strtoull(text, nullptr, 10);
}

constexpr std::size_t kThreads = 4;
// Counts cycle through every shape up to here: fewer indices than threads, as
// many, and several times as many.
constexpr std::size_t kLargestCount = 3 * kThreads + 1;

struct Tally {
  // Written by whichever thread ran the index -- one per index per run -- and
  // read by the caller after run returns, both plain: the contract's ordering
  // is what is being soaked.
  std::size_t marks[kLargestCount] = {};  // NOLINT(modernize-avoid-c-arrays)
  std::size_t slot_out_of_range = 0;
};

TEST(ExecutorSoak, MillionsOfRunsLoseNoIndexAndGrowNothing) {
  PosixThreads threads;
  loadforge::testing::FakeSyscalls syscalls;
  const X86FpControl fp;
  const long threads_without_pool = threads_now();
  auto made = make_thread_pool(std::vector<ThreadPlacement>(kThreads), threads, syscalls, fp);
  ASSERT_TRUE(made.has_value());
  const std::unique_ptr<Executor> executor = std::move(made).value();

  Tally tally;
  auto task = [](void* context, std::size_t index, std::size_t slot) noexcept {
    auto& into = *static_cast<Tally*>(context);
    into.marks[index] += 1;
    if (slot >= kThreads) {
      into.slot_out_of_range += 1;  // never concurrent with itself unless broken
    }
  };

  executor->run(kLargestCount, task, &tally);  // the first run, outside the count
  for (std::size_t& mark : tally.marks) {
    mark = 0;
  }
  const long threads_before = threads_now();
  const std::uint64_t allocations_before = allocations.load(std::memory_order_seq_cst);

  const std::uint64_t total = runs();
  ASSERT_GT(total, 0U) << "LOADFORGE_SOAK_RUNS asks for no runs";
  std::uint64_t lost = 0;
  std::uint64_t repeated = 0;
  for (std::uint64_t run = 0; run < total; ++run) {
    const std::size_t count = 1 + static_cast<std::size_t>(run % kLargestCount);
    executor->run(count, task, &tally);
    for (std::size_t index = 0; index < count; ++index) {
      lost += tally.marks[index] == 0 ? 1 : 0;
      repeated += tally.marks[index] > 1 ? 1 : 0;
      tally.marks[index] = 0;
    }
  }

  const std::uint64_t allocations_after = allocations.load(std::memory_order_seq_cst);
  const long threads_after = threads_now();
  EXPECT_EQ(lost, 0U);
  EXPECT_EQ(repeated, 0U);
  EXPECT_EQ(tally.slot_out_of_range, 0U);
  for (const std::size_t mark : tally.marks) {
    EXPECT_EQ(mark, 0U) << "an index past every run's count was run";
  }
  ASSERT_GT(threads_before, 0) << "/proc/self/status names no thread count";
  EXPECT_EQ(threads_after, threads_before);
#if !defined(LOADFORGE_SOAK_SANITIZED)
  // ThreadSanitizer's runtime starts a thread of its own once the process has
  // made one (measured: six threads where this build has five), so the count
  // of what the pool added is exact only without it.
  EXPECT_EQ(threads_before, threads_without_pool + static_cast<long>(kThreads))
      << "the pool's threads, and no more";
#else
  (void)threads_without_pool;
#endif
  EXPECT_EQ(allocations_after, allocations_before) << "a run allocated";
}

#if !defined(LOADFORGE_SOAK_SANITIZED)
// The counter is shown able to count before its zero is believed: a soak whose
// allocation figure could not move would pass for the wrong reason.
TEST(ExecutorSoak, TheAllocationCountCanFail) {
  const std::uint64_t before = allocations.load(std::memory_order_seq_cst);
  auto held = std::make_unique<std::vector<int>>(3);
  EXPECT_GE(allocations.load(std::memory_order_seq_cst), before + 2);
}
#endif

}  // namespace
}  // namespace loadforge::worker
