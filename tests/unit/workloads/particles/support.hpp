// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_PARTICLES_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_PARTICLES_SUPPORT_HPP

// What the particles tests share: this stream's own executors (written from
// worker/executor.hpp, run against ExecutorContract in executor_mock_test.cpp),
// and helpers that make a workload, a unit's input and its output.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/particles/kernel.hpp"
#include "workloads/particles/particles.hpp"
#include "workloads/particles/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::particles::testing {

/// One thread: the calling one, in index order.
class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// `threads` threads started for each run, each taking the next unclaimed
/// index: which thread runs which index, and in what order, differs from run
/// to run, which is the freedom the contract gives an executor.
class PoolExecutor final : public worker::Executor {
 public:
  explicit PoolExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
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

using Bytes = std::vector<std::byte>;

/// The ISA paths this build has a particles kernel for and this machine can
/// run. The AVX2 build of a machine without AVX2 would fault, so it is left
/// out; the dispatch contract has the worker keep it out of a run.
inline std::vector<simd::IsaPath> runnable_paths() {
  std::vector<simd::IsaPath> paths{simd::IsaPath::kScalar};
#if defined(LOADFORGE_PARTICLES_AVX2)
  if (__builtin_cpu_supports("avx2")) {
    paths.push_back(simd::IsaPath::kAvx2);
  }
#elif defined(LOADFORGE_PARTICLES_NEON)
  paths.push_back(simd::IsaPath::kNeon);
#endif
  return paths;
}

inline WorkloadConfig config_for(std::uint32_t cells, simd::IsaPath isa, std::uint64_t seed = 7,
                                 std::uint64_t units = 4) {
  WorkloadConfig config;
  config.seed = seed;
  config.footprint_bytes = units * (kInputBytes + kOutputBytes);
  config.decomposition = cells;
  config.isa = isa;
  return config;
}

inline std::unique_ptr<Workload> make(const WorkloadConfig& config) {
  auto created = create(config);
  return created.has_value() ? std::move(created).value() : nullptr;
}

inline Bytes generate(const Workload& workload, std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

/// Computes unit `index` on `input`, the scratch and the output pre-filled
/// with `fill`.
inline Bytes compute(const Workload& workload, std::uint64_t index, std::span<const std::byte> input,
                     worker::Executor& executor, std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

inline std::vector<double> as_doubles(std::span<const std::byte> bytes) {
  std::vector<double> values(bytes.size() / sizeof(double));
  std::memcpy(values.data(), bytes.data(), values.size() * sizeof(double));
  return values;
}

inline Bytes as_bytes(std::span<const double> values) {
  Bytes bytes(values.size() * sizeof(double));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

}  // namespace loadforge::workloads::particles::testing

#endif
