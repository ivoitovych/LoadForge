// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_SORT_EXECUTORS_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_SORT_EXECUTORS_HPP

// The sort's own mocks of worker::Executor, written from the contract in
// src/worker/executor.hpp (ADR 0003) and checked against the provider's suite
// in tests/unit/workloads/sort/executor_mock_test.cpp. They differ from one
// another in what the contract leaves open: the thread count, the order the
// indices run in, and which slot an index gets.

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

#include "worker/executor.hpp"

namespace loadforge::workloads::sort::testing {

/// One thread: the calling one, indices in order, slot 0.
class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// One thread, indices from the last to the first.
class ReversedExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = count; index-- > 0;) {
      task(context, index, 0);
    }
  }
};

/// `threads` real threads per run(), each claiming the next unclaimed index, so
/// which thread runs which index differs from run to run.
class ThreadedExecutor final : public worker::Executor {
 public:
  explicit ThreadedExecutor(std::size_t threads) noexcept : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([&next, count, task, context, slot] {
        for (std::size_t index = next.fetch_add(1, std::memory_order_seq_cst); index < count;
             index = next.fetch_add(1, std::memory_order_seq_cst)) {
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

/// A serial executor that can be told to drop one task, to inject the fault of
/// an executor that does not keep "exactly once for each index". The calls to
/// run() are numbered from 0 in the order they are made.
class DroppingExecutor final : public worker::Executor {
 public:
  DroppingExecutor(std::size_t call, std::size_t index) noexcept : call_(call), index_(index) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      if (calls_ != call_ || index != index_) {
        task(context, index, 0);
      }
    }
    ++calls_;
  }

 private:
  std::size_t call_;
  std::size_t index_;
  std::size_t calls_ = 0;
};

}  // namespace loadforge::workloads::sort::testing

#endif
