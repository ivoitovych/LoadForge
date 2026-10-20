// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_RAYTRACE_EXECUTORS_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_RAYTRACE_EXECUTORS_HPP

// The ray tracing workload's own mocks of worker::Executor (ADR 0003), each
// run against ExecutorContract in tests/unit/workloads/raytrace, and the
// helpers that run a unit through a workload.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include "oracle/workloads/raytrace/reference.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::tests::raytrace {

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// `threads` real threads claiming indices from one atomic counter, the
/// slot being the thread's number. Joined before run() returns, so every
/// write happens before it does.
class PoolExecutor final : public worker::Executor {
 public:
  explicit PoolExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
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

/// Runs every index but the ones it is told to drop: a fault injected into
/// the thing the workload consumes.
class DroppingExecutor final : public worker::Executor {
 public:
  explicit DroppingExecutor(std::set<std::size_t> dropped) : dropped_(std::move(dropped)) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      if (!dropped_.contains(index)) {
        task(context, index, 0);
      }
    }
  }

 private:
  std::set<std::size_t> dropped_;
};

[[nodiscard]] inline Bytes generate_unit(const workloads::Workload& workload,
                                         std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

[[nodiscard]] inline Bytes compute_unit(const workloads::Workload& workload, std::uint64_t index,
                                        const Bytes& input, worker::Executor& executor,
                                        std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

}  // namespace loadforge::tests::raytrace

#endif
