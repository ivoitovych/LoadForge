// SPDX-License-Identifier: GPL-3.0-or-later
//
// ExecutorContract against this module's own executors (support.hpp), ADR
// 0003's cross-execution: the adaptive mesh workload is a consumer of
// worker/executor.hpp, its tests run units on executors written from this
// module's reading of that contract, and the provider's suite is how that
// reading is compared with the provider's. The mocks include the dealt, the
// shuffled and the claiming schedules the determinism tier relies on, so the
// suite vouches for exactly the executors the T6 claims are made on.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/adaptive_mesh/support.hpp"
#include "worker/executor.hpp"

namespace loadforge::workloads::adaptive_mesh {
namespace {

struct SerialMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<testing::Serial>(); }
};

struct DealtThreeMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<testing::Dealt>(3, 1); }
};

struct ShuffledSevenMock {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<testing::Shuffled>(7, 2);
  }
};

struct ClaimingFourMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<testing::Claiming>(4); }
};

using Mocks = ::testing::Types<SerialMock, DealtThreeMock, ShuffledSevenMock, ClaimingFourMock>;

TEST(AdaptiveMeshMockExecutors, AShuffleIsAPermutationAndTheSeedChangesIt) {
  const auto one = testing::Dealt::permutation(64, 1);
  const auto two = testing::Dealt::permutation(64, 2);
  EXPECT_NE(one, two);
  auto sorted = one;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t index = 0; index < sorted.size(); ++index) {
    EXPECT_EQ(sorted[index], index);
  }
  EXPECT_TRUE(testing::Dealt::permutation(0, 1).empty());
  EXPECT_EQ(testing::Dealt::permutation(1, 1), std::vector<std::size_t>{0});
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(AdaptiveMeshMock, ExecutorContract, workloads::adaptive_mesh::Mocks);
}  // namespace loadforge::contracts
