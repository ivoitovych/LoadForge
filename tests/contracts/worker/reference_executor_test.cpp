// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two trivial reference executors run against the executor suite: one serial,
// and one that starts a thread per slot for every run. Neither is the pool the
// executor stream builds; they prove the suite runs, and that it passes both a
// single-threaded stand-in and a genuinely concurrent implementation.

#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "worker/executor.hpp"

namespace loadforge::contracts {
namespace {

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// Slot s takes indices s, s + n, s + 2n, ...: a fixed partition, joined
/// before run returns, which is what makes every write visible to the caller.
class ThreadPerSlotExecutor final : public worker::Executor {
 public:
  explicit ThreadPerSlotExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, count, task, context] {
        for (std::size_t index = slot; index < count; index += threads_) {
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

struct ReferenceSerial {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};

struct ReferenceThreads {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<ThreadPerSlotExecutor>(3);
  }
};

using ReferenceExecutors = ::testing::Types<ReferenceSerial, ReferenceThreads>;
INSTANTIATE_TYPED_TEST_SUITE_P(Reference, ExecutorContract, ReferenceExecutors);

}  // namespace
}  // namespace loadforge::contracts
