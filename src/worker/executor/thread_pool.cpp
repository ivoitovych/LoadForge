// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/executor/thread_pool.hpp"

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "worker/executor.hpp"

namespace loadforge::worker {

std::vector<ThreadPlacement> one_thread_per_cpu(const platform::CpuMask& cpus) {
  std::vector<ThreadPlacement> placements;
  for (const std::uint32_t cpu : platform::ids_of(cpus)) {
    platform::CpuMask alone;
    alone.cpus.set(cpu);
    placements.emplace_back(alone);
  }
  return placements;
}

// Translations, as in platform/syscalls.cpp: no decision here, so nothing for
// a fake to stand in for. Covered by every test that makes a real pool.
int PosixThreads::start(pthread_t& handle, Entry entry, void* argument) {
  return ::pthread_create(&handle, nullptr, entry, argument);
}

void PosixThreads::join(pthread_t handle) { (void)::pthread_join(handle, nullptr); }

namespace {

std::string describe_flag(const std::optional<bool>& flag) {
  if (!flag.has_value()) {
    return "absent";
  }
  if (*flag) {
    return "on";
  }
  return "off";
}

std::string describe_state(const fpenv::FpState& state) {
  std::string text = "rounding ";
  text += fpenv::describe(state.rounding);
  text += ", flush-to-zero ";
  text += describe_flag(state.flush_to_zero);
  text += ", denormals-are-zero ";
  text += describe_flag(state.denormals_are_zero);
  text += ", default-NaN ";
  text += describe_flag(state.default_nan);
  return text;
}

std::string describe_one(const NoThreads& /*error*/) {
  return "an executor needs at least one thread, and none was placed";
}

std::string describe_one(const ThreadNotStarted& error) {
  return "cannot start executor thread " + std::to_string(error.thread) + ": " +
         platform::describe(error.cause);
}

std::string describe_one(const ThreadNotPlaced& error) {
  return "executor thread " + std::to_string(error.thread) + ": " +
         platform::describe(error.refused);
}

std::string describe_one(const ThreadFpStateDiffers& error) {
  return "executor thread " + std::to_string(error.thread) +
         " started with a floating-point state other than its creator's: expected " +
         describe_state(error.expected) + "; found " + describe_state(error.found);
}

// THE HAND-OFF AND COMPLETION PROTOCOL, AND WHY IT IS CORRECT
// ----------------------------------------------------------
// One mutex guards everything a run publishes and everything a thread reports.
// The one atomic is the next index to claim, and every operation on it is
// seq_cst; nothing here is weaker (docs/PLAN.md §4.5). The argument, claim by
// claim of executor.hpp -- each tested in
// tests/unit/worker/executor/ordering_test.cpp, which repeats it:
//
//  1. A run's task, context and count reach every thread. run() writes them,
//     resets next_ and done_, and advances generation_, all under mutex_. A
//     thread reads them under mutex_ after seeing generation_ move. The
//     caller's release of mutex_ (in finished_.wait) synchronises-with the
//     thread's acquire, so the thread sees this run's values, never the last.
//
//  2. Every index in [0, count) runs exactly once, and no other. next_ is
//     claimed with fetch_add, a read-modify-write: RMWs on one object form a
//     single modification order and each reads the value the one before it
//     wrote, so no two claims return the same index, whatever the memory
//     order. A thread runs an index only when it is below count, and stops at
//     the first that is not; every thread claims until it sees one that is
//     not, so no index below count is left. next_ ends at most count +
//     threads() and never wraps short of count > SIZE_MAX - threads(): more
//     tasks than any run could execute in a lifetime.
//
//  3. Everything each call wrote happens-before run returns. A task's writes
//     are sequenced before its thread's next claim and then its increment of
//     done_ under mutex_; run returns only after seeing done_ == threads()
//     under mutex_, so each thread's release synchronises-with the caller's
//     acquire, and the writes are visible with no further synchronisation.
//
//  4. No thread claims from one run with another run's task. done_ ==
//     threads() means every thread has left its claim loop for this run, and a
//     thread re-enters it only after seeing generation_ advance under mutex_ --
//     which happens after the next run reset next_ under the same mutex.
//
//  5. Slots are unique among concurrent calls: a slot is a thread, and a
//     thread runs one task at a time.
//
// Every wait below is entered by a thread that has held mutex_ since it last
// changed what the wait's condition reads, so its first test of the condition
// is fixed by the protocol and not by the scheduler: both outcomes of each
// condition are taken by every test that makes a pool and runs it, never by
// luck (journal §4.11).

class ThreadPool;

/// One thread's record. Its address is fixed for the pool's life (the vector
/// is sized once), and is what the thread is started with.
struct Worker {
  ThreadPool* pool = nullptr;
  std::size_t slot = 0;
  pthread_t handle{};
  bool reported = false;             ///< Guarded by mutex_.
  std::optional<PoolError> failure;  ///< Guarded by mutex_; read once reported.
};

class ThreadPool final : public Executor {
 public:
  ThreadPool(std::vector<ThreadPlacement> placements, Threads& threads,
             platform::Syscalls& syscalls, const fpenv::FpControl& fp)
      : threads_(&threads),
        syscalls_(&syscalls),
        fp_(&fp),
        placements_(std::move(placements)),
        expected_(fp.read()),
        workers_(placements_.size()) {
    for (std::size_t slot = 0; slot < workers_.size(); ++slot) {
      workers_[slot].pool = this;
      workers_[slot].slot = slot;
    }
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  /// Stops and joins every thread started, whether the pool finished starting
  /// or is being unwound after a refusal. A thread that failed its own start
  /// has already returned; the join reaps it.
  ~ThreadPool() override {
    {
      const std::lock_guard lock(mutex_);
      stopping_ = true;
      wake_.notify_all();
    }
    for (std::size_t slot = 0; slot < started_; ++slot) {
      threads_->join(workers_[slot].handle);
    }
  }

  [[nodiscard]] std::size_t threads() const noexcept override { return workers_.size(); }

  void run(std::size_t count, Task task, void* context) noexcept override {
    if (count == 0) {
      return;
    }
    std::unique_lock lock(mutex_);
    task_ = task;
    context_ = context;
    count_ = count;
    done_ = 0;
    next_.store(0, std::memory_order_seq_cst);
    ++generation_;
    wake_.notify_all();
    while (done_ != workers_.size()) {
      finished_.wait(lock);
    }
  }

  /// Starts thread `slot` and waits for it to report: nullopt once it is
  /// placed, checked and waiting for work; otherwise why it is not.
  [[nodiscard]] std::optional<PoolError> start(std::size_t slot) {
    Worker& worker = workers_[slot];
    // Held across the start, so the thread's report -- made under mutex_ --
    // cannot come before this thread waits for it.
    std::unique_lock lock(mutex_);
    const int number = threads_->start(worker.handle, &ThreadPool::entry, &worker);
    if (number != 0) {
      return PoolError{ThreadNotStarted{
          slot,
          platform::SyscallError{number, "pthread_create", "thread " + std::to_string(slot)}}};
    }
    ++started_;
    while (!worker.reported) {
      reported_.wait(lock);
    }
    return worker.failure;
  }

 private:
  static void* entry(void* argument) noexcept {
    Worker& worker = *static_cast<Worker*>(argument);
    worker.pool->serve(worker);
    return nullptr;
  }

  /// Run on the new thread, before it reports: affinity with pid 0 and the
  /// floating-point registers are both the calling thread's.
  [[nodiscard]] std::optional<PoolError> prepare(std::size_t slot) const {
    const ThreadPlacement& placement = placements_[slot];
    if (placement.has_value()) {
      platform::Affinity affinity(*syscalls_);
      auto placed = affinity.place(*placement);
      if (!placed.has_value()) {
        return PoolError{ThreadNotPlaced{slot, placed.error()}};
      }
    }
    const fpenv::FpState found = fp_->read();
    if (!(found == expected_)) {
      return PoolError{ThreadFpStateDiffers{slot, expected_, found}};
    }
    return std::nullopt;
  }

  void serve(Worker& worker) {
    std::optional<PoolError> failure = prepare(worker.slot);
    std::unique_lock lock(mutex_);
    worker.failure = std::move(failure);
    worker.reported = true;
    reported_.notify_one();
    if (worker.failure.has_value()) {
      return;
    }
    std::uint64_t seen = generation_;
    for (;;) {
      while (generation_ == seen && !stopping_) {
        wake_.wait(lock);
      }
      if (stopping_) {
        return;
      }
      seen = generation_;
      const Task task = task_;
      void* const context = context_;
      const std::size_t count = count_;
      lock.unlock();
      for (std::size_t index = next_.fetch_add(1, std::memory_order_seq_cst); index < count;
           index = next_.fetch_add(1, std::memory_order_seq_cst)) {
        task(context, index, worker.slot);
      }
      lock.lock();
      ++done_;
      if (done_ == workers_.size()) {
        finished_.notify_one();
      }
    }
  }

  Threads* threads_;
  platform::Syscalls* syscalls_;  ///< Used only while make_thread_pool runs.
  const fpenv::FpControl* fp_;    ///< Used only while make_thread_pool runs.
  std::vector<ThreadPlacement> placements_;
  fpenv::FpState expected_;
  std::vector<Worker> workers_;
  std::size_t started_ = 0;  ///< Touched only by the thread that owns the pool.

  std::mutex mutex_;
  std::condition_variable reported_;  ///< A starting thread to the factory.
  std::condition_variable wake_;      ///< The caller to the threads: a run, or stop.
  std::condition_variable finished_;  ///< The last thread out of a run to the caller.
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  Task task_ = nullptr;
  void* context_ = nullptr;
  std::size_t count_ = 0;
  std::size_t done_ = 0;
  std::atomic<std::size_t> next_{0};
};

}  // namespace

std::string describe(const PoolError& error) {
  return std::visit([](const auto& one) { return describe_one(one); }, error);
}

core::Result<std::unique_ptr<Executor>, PoolError> make_thread_pool(
    const std::vector<ThreadPlacement>& placements, Threads& threads, platform::Syscalls& syscalls,
    const fpenv::FpControl& fp) {
  if (placements.empty()) {
    return PoolError{NoThreads{}};
  }
  auto pool = std::make_unique<ThreadPool>(placements, threads, syscalls, fp);
  for (std::size_t slot = 0; slot < placements.size(); ++slot) {
    if (std::optional<PoolError> failure = pool->start(slot); failure.has_value()) {
      // The pool's destructor stops and joins the threads already started.
      return std::move(*failure);
    }
  }
  return std::unique_ptr<Executor>{std::move(pool)};
}

}  // namespace loadforge::worker
