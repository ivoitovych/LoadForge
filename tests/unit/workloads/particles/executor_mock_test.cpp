// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two executors this stream's tests use, run against the provider-side
// suite of the contract they are written from (ADR 0003): a mock that fails
// the suite is a reading of worker/executor.hpp that the provider's differs from.

#include <gtest/gtest.h>

#include <memory>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/particles/support.hpp"

namespace loadforge::contracts {
namespace {

namespace testing = ::loadforge::workloads::particles::testing;

struct SerialSubject {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<testing::SerialExecutor>();
  }
};

struct PoolOfThreeSubject {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<testing::PoolExecutor>(3);
  }
};

struct PoolOfSevenSubject {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<testing::PoolExecutor>(7);
  }
};

using Mocks = ::testing::Types<SerialSubject, PoolOfThreeSubject, PoolOfSevenSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(ParticlesMocks, ExecutorContract, Mocks);

}  // namespace
}  // namespace loadforge::contracts
