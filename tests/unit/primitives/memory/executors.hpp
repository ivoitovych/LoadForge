// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_PRIMITIVES_MEMORY_EXECUTORS_HPP
#define LOADFORGE_TESTS_UNIT_PRIMITIVES_MEMORY_EXECUTORS_HPP

// This module's own readings of worker::Executor (ADR 0003), written from
// src/worker/executor.hpp alone: a serial one, and one that starts a thread per
// slot for every run and deals indices round-robin. Neither is the pool the
// executor stream builds; both keep the contract, which is what the load needs.

#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "worker/executor.hpp"

namespace loadforge::primitives::memory::mocks {

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

struct SerialSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};

struct ThreadedSubject {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<ThreadedExecutor>(3);
  }
};

}  // namespace loadforge::primitives::memory::mocks

#endif
