// SPDX-License-Identifier: GPL-3.0-or-later
// ExecutorContract (worker/executor.hpp) against this module's own executors
// (ADR 0003): the consumer's mocks are held to what the contract promises, so
// that every other test here runs on executors that keep it.

#include <gtest/gtest.h>

#include <memory>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "worker/executor.hpp"

namespace loadforge::workloads::monte_carlo {
namespace {

using testing::SerialExecutor;
using testing::ThreadedExecutor;

struct SerialSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};
struct ThreeThreadSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ThreadedExecutor>(3); }
};
struct SevenThreadSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ThreadedExecutor>(7); }
};

}  // namespace
}  // namespace loadforge::workloads::monte_carlo

namespace loadforge::contracts {

using MonteCarloMocks = ::testing::Types<workloads::monte_carlo::SerialSubject,
                                         workloads::monte_carlo::ThreeThreadSubject,
                                         workloads::monte_carlo::SevenThreadSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(MonteCarloMocks, ExecutorContract, MonteCarloMocks);

}  // namespace loadforge::contracts
