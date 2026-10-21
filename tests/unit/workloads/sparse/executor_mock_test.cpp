// SPDX-License-Identifier: GPL-3.0-or-later
//
// The executors the sparse tests run against, held to ExecutorContract (ADR
// 0003): a workload tested against a stand-in is only tested against what the
// contract says if the stand-in keeps the contract.

#include <gtest/gtest.h>

#include <memory>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/sparse/executors.hpp"

namespace loadforge::contracts {
namespace {

using sparse_tests::PoolExecutor;
using sparse_tests::ReverseExecutor;
using sparse_tests::SerialExecutor;

struct Serial {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};
struct Reverse {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ReverseExecutor>(); }
};
struct Pool3 {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<PoolExecutor>(3); }
};
struct Pool7 {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<PoolExecutor>(7); }
};

using Mocks = ::testing::Types<Serial, Reverse, Pool3, Pool7>;
INSTANTIATE_TYPED_TEST_SUITE_P(SparseMocks, ExecutorContract, Mocks);

}  // namespace
}  // namespace loadforge::contracts
