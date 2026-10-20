// SPDX-License-Identifier: GPL-3.0-or-later
//
// ExecutorContract against this module's own mocks of the executor (ADR 0003):
// the workload consumes an Executor, and the readings of the contract it was
// tested against are these. The mocks are the ones the graph tests run the
// workload with, so the suite proves they are executors before any test relies
// on them.

#include <gtest/gtest.h>

#include <memory>

#include "contracts/worker/executor_contract.hpp"
#include "graph_support.hpp"

namespace loadforge::contracts {
namespace {

using namespace graph_test;

struct SerialMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};
struct PartitionMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<PartitionExecutor>(3); }
};
struct PullMock {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<PullExecutor>(4); }
};

using Mocks = ::testing::Types<SerialMock, PartitionMock, PullMock>;
INSTANTIATE_TYPED_TEST_SUITE_P(GraphMocks, ExecutorContract, Mocks);

}  // namespace
}  // namespace loadforge::contracts
