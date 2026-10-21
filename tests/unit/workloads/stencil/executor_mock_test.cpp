// SPDX-License-Identifier: GPL-3.0-or-later
//
// The executor this workload consumes (ADR 0003): the module's own mocks of
// worker::Executor, run against ExecutorContract so they are shown to keep the
// contract the workload relies on, and then used to see how the workload uses
// an executor.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/stencil/stencil_support.hpp"
#include "worker/executor.hpp"

namespace loadforge::contracts {
namespace {

struct MockSerial {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<workloads::stencil::testing::SerialExecutor>();
  }
};
struct MockReverse {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<workloads::stencil::testing::ReverseExecutor>();
  }
};
struct MockInterleaved {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<workloads::stencil::testing::ThreadedExecutor>(3, true);
  }
};
struct MockBlocks {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<workloads::stencil::testing::ThreadedExecutor>(7, false);
  }
};

using Mocks = ::testing::Types<MockSerial, MockReverse, MockInterleaved, MockBlocks>;
INSTANTIATE_TYPED_TEST_SUITE_P(StencilMocks, ExecutorContract, Mocks);

}  // namespace
}  // namespace loadforge::contracts

namespace loadforge::workloads::stencil {
namespace {

/// Records every run() the workload makes.
class RecordingExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 2; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    counts.push_back(count);
    std::vector<int> seen(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
      ++seen[index];
      task(context, index, index % 2);
    }
    EXPECT_EQ(std::count(seen.begin(), seen.end(), 1), static_cast<std::ptrdiff_t>(count));
    ++runs;
  }
  std::vector<std::size_t> counts;
  std::size_t runs = 0;
};

TEST(StencilExecutorUse, OneRunPerSweepWithOneTaskPerTile) {
  for (const std::uint32_t tile : {16U, 32U, 64U, 128U}) {
    const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, tile));
    ASSERT_NE(workload, nullptr);
    RecordingExecutor executor;
    const testing::Bytes input = testing::generated(*workload, 0);
    (void)testing::computed(*workload, 0, input, executor);
    const std::size_t per_side = kEdge / tile;
    ASSERT_EQ(executor.counts.size(), kSteps) << tile;
    for (const std::size_t count : executor.counts) {
      EXPECT_EQ(count, per_side * per_side) << tile;
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::stencil
