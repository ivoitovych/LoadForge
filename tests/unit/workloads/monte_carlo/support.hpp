// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_MONTE_CARLO_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_MONTE_CARLO_SUPPORT_HPP

// What the Monte Carlo tests of every tier share: executors written from
// worker/executor.hpp (ADR 0003: this module's own mocks of the contract it
// consumes, run against ExecutorContract in executor_mock_test.cpp), and the
// vector path this machine can run.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/monte_carlo/monte_carlo.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::monte_carlo::testing {

/// One thread, indices in order.
class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// `threads` threads started per run, each taking the next index from a shared
/// counter, so which thread runs which index differs from run to run. The
/// join is what makes every write visible when run() returns.
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

/// The widest vector path this build has a kernel for and this CPU runs;
/// the scalar path where there is none. Chosen as the worker would choose it.
[[nodiscard]] inline simd::IsaPath native_path() noexcept {
#if defined(__x86_64__) && !defined(LOADFORGE_SIMD_SCALAR)
  return __builtin_cpu_supports("avx2") ? simd::IsaPath::kAvx2 : simd::IsaPath::kScalar;
#elif defined(__aarch64__) && !defined(LOADFORGE_SIMD_SCALAR)
  return simd::IsaPath::kNeon;
#else
  return simd::IsaPath::kScalar;
#endif
}

/// The accumulators a path's order of summation has (kernels.hpp), as this
/// test tree states it; AVX-512 has no kernel.
[[nodiscard]] constexpr unsigned lanes_of(simd::IsaPath path) noexcept {
  switch (path) {
    case simd::IsaPath::kScalar:
      return 1;
    case simd::IsaPath::kAvx2:
      return 4;
    case simd::IsaPath::kNeon:
      return 2;
    case simd::IsaPath::kAvx512:
      break;
  }
  return 0;
}

[[nodiscard]] inline std::unique_ptr<Workload> make(std::uint64_t seed, std::uint32_t decomposition,
                                                    simd::IsaPath isa,
                                                    std::uint64_t footprint = 16384) {
  auto created = create(WorkloadConfig{seed, footprint, decomposition, isa});
  return created.has_value() ? std::move(created).value() : nullptr;
}

/// A unit's output, computed on `executor`.
[[nodiscard]] inline std::vector<std::byte> compute_unit(const Workload& workload,
                                                         std::uint64_t index,
                                                         std::span<const std::byte> input,
                                                         worker::Executor& executor) {
  std::vector<std::byte> output(workload.plan().output_bytes, std::byte{0xA5});
  std::vector<std::byte> scratch(workload.plan().scratch_bytes * executor.threads());
  workload.compute(index, input, output, scratch, executor);
  return output;
}

[[nodiscard]] inline std::vector<std::byte> generate_unit(const Workload& workload,
                                                          std::uint64_t index) {
  std::vector<std::byte> input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

}  // namespace loadforge::workloads::monte_carlo::testing

#endif
