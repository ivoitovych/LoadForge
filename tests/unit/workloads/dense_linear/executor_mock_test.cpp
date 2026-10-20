// SPDX-License-Identifier: GPL-3.0-or-later
//
// ExecutorContract against this module's own executors (support.hpp), ADR
// 0003's cross-execution: the dense-linear workload is a consumer of
// worker/executor.hpp, its tests run units on executors written from this
// module's reading of that contract, and the provider's suite is how that
// reading is compared with the provider's. The mocks include the shuffled
// and the claiming schedules the determinism tier relies on, so the suite
// vouches for exactly the executors the T6 claims are made on.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/dense_linear/support.hpp"
#include "worker/executor.hpp"

namespace loadforge::workloads::dense_linear {
namespace {

struct SerialMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<testing::Serial>(); }
};

struct ShuffledThreeMock {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<testing::Shuffled>(3, 1);
  }
};

struct ShuffledSevenMock {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<testing::Shuffled>(7, 2);
  }
};

struct ClaimingFourMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<testing::Claiming>(4); }
};

using Mocks = ::testing::Types<SerialMock, ShuffledThreeMock, ShuffledSevenMock, ClaimingFourMock>;

TEST(DenseLinearMockExecutors, AShuffleIsAPermutationAndTheSeedChangesIt) {
  const auto one = testing::Shuffled::permutation(64, 1);
  const auto two = testing::Shuffled::permutation(64, 2);
  EXPECT_NE(one, two);
  auto sorted = one;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t index = 0; index < sorted.size(); ++index) {
    EXPECT_EQ(sorted[index], index);
  }
  EXPECT_TRUE(testing::Shuffled::permutation(0, 1).empty());
  EXPECT_EQ(testing::Shuffled::permutation(1, 1), std::vector<std::size_t>{0});
}

}  // namespace
}  // namespace loadforge::workloads::dense_linear

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(DenseLinearMock, ExecutorContract, workloads::dense_linear::Mocks);
}  // namespace loadforge::contracts
