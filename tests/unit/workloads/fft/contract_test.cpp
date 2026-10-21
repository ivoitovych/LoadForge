// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract this module provides, run against the workload at each radix;
// and the contract it consumes, run against this module's own stand-ins for an
// executor (ADR 0003).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "contracts/workloads/workload_contract.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {

namespace {

template <std::uint32_t Radix>
struct FftSubject {
  static constexpr const char* kName = "fft";
  static workloads::WorkloadConfig config() {
    // Four units' worth: the suite wants at least two.
    return workloads::WorkloadConfig{
        0xF1F7, 4 * (workloads::fft::kInputBytes + workloads::fft::kOutputBytes), Radix,
        simd::IsaPath::kScalar};
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return workloads::fft::create(config);
  }
};

struct DefaultRadix : FftSubject<0> {};

using Subjects = ::testing::Types<DefaultRadix, FftSubject<2>, FftSubject<4>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Fft, WorkloadContract, Subjects);

// This module's reading of the executor contract, as stand-ins: one that runs
// every task on the calling thread, and one that starts a thread per slot for
// each run and joins them before it returns.
class CallerExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

class JoiningExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return kThreads; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    for (std::size_t slot = 0; slot < kThreads; ++slot) {
      pool.emplace_back([slot, count, task, context] {
        for (std::size_t index = slot; index < count; index += kThreads) {
          task(context, index, slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  static constexpr std::size_t kThreads = 3;
};

struct CallerMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<CallerExecutor>(); }
};
struct JoiningMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<JoiningExecutor>(); }
};

using Mocks = ::testing::Types<CallerMock, JoiningMock>;
INSTANTIATE_TYPED_TEST_SUITE_P(FftMock, ExecutorContract, Mocks);

}  // namespace
}  // namespace loadforge::contracts
