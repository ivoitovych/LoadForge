// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_COMPRESSION_EXECUTORS_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_COMPRESSION_EXECUTORS_HPP

// The compression workload's own executors: its reading of
// src/worker/executor.hpp, which it consumes (ADR 0003). The executor stream
// writes the real pool; these exist so the workload can be run at a chosen
// thread count without it, and each is run against ExecutorContract in
// executors_test.cpp, so a reading that drifted from the contract's would
// fail there rather than make a workload test pass for the wrong reason.
//
//   SerialExecutor        one thread, slot 0, indices in order.
//   ThreadedExecutor(n)   n threads started per run, taking indices from a
//                         shared counter, so which slot runs which index
//                         changes from run to run -- the contract's "varies
//                         between runs", made to happen.
//   LastSlotExecutor(n)   claims n threads and runs every index on the
//                         calling thread in slot n - 1: a workload that
//                         ignored its slot and used the first piece of
//                         scratch would be caught by what it left in piece 0.

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "worker/executor.hpp"

namespace loadforge::workloads::compression::testing {

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

class ThreadedExecutor final : public worker::Executor {
 public:
  explicit ThreadedExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    // seq_cst throughout: the counter hands out each index once, and the
    // joins are what publish every task's writes to the caller.
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

class LastSlotExecutor final : public worker::Executor {
 public:
  explicit LastSlotExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, threads_ - 1);
    }
  }

 private:
  std::size_t threads_;
};

}  // namespace loadforge::workloads::compression::testing

#endif
