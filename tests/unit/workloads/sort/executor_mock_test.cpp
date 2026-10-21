// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sort's mocks of worker::Executor against the provider's suite (ADR 0003):
// what the sort's tests assume of an executor is what the contract promises.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

#include "contracts/worker/executor_contract.hpp"
#include "unit/workloads/sort/executors.hpp"
#include "worker/executor.hpp"

namespace loadforge::workloads::sort::testing {
namespace {

struct SerialSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};
struct ReversedSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ReversedExecutor>(); }
};
struct ThreadedSubject {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ThreadedExecutor>(4); }
};
// A dropping executor that drops nothing (its call number is never reached).
struct NeverDroppingSubject {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<DroppingExecutor>(~std::size_t{0}, 0);
  }
};

using Mocks =
    ::testing::Types<SerialSubject, ReversedSubject, ThreadedSubject, NeverDroppingSubject>;

}  // namespace
}  // namespace loadforge::workloads::sort::testing

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(SortMocks, ExecutorContract, workloads::sort::testing::Mocks);
}  // namespace loadforge::contracts
