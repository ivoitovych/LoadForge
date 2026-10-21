// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_SPARSE_EXECUTORS_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_SPARSE_EXECUTORS_HPP

// The sparse tests' own readings of worker/executor.hpp (ADR 0003): executors
// that keep the contract and differ from each other in every way it leaves
// free -- thread count, the order indices are called in, which thread gets
// which index. The sparse stream's output must not show any of that.
// ExecutorContract is run against each of them (executor_mock_test.cpp), so
// what the workload is tested with is what the contract says an executor is.

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "worker/executor.hpp"

namespace loadforge::sparse_tests {

/// One thread, indices in ascending order.
class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// One thread, indices in descending order.
class ReverseExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = count; index-- > 0;) {
      task(context, index, 0);
    }
  }
};

/// `threads` threads, started for each run; each claims the next unclaimed
/// index from a shared counter, so which thread runs which index changes from
/// run to run. A slot is the claiming thread's number.
class PoolExecutor final : public worker::Executor {
 public:
  explicit PoolExecutor(std::size_t threads) noexcept : threads_(threads) {}

  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }

  void run(std::size_t count, Task task, void* context) noexcept override {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([&next, count, task, context, slot] {
        for (;;) {
          const std::size_t index = next.fetch_add(1, std::memory_order_seq_cst);
          if (index >= count) {
            return;
          }
          task(context, index, slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
};

}  // namespace loadforge::sparse_tests

#endif
