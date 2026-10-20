// SPDX-License-Identifier: GPL-3.0-or-later
//
// The workload's own executors against the executor contract's suite: the
// cross-execution ADR 0003 asks of a consumer. If one of them read the
// contract differently from its provider, this fails, and every workload test
// that ran on it is suspect.

#include "unit/workloads/compression/executors.hpp"

#include <cstddef>
#include <memory>

#include "contracts/worker/executor_contract.hpp"
#include "worker/executor.hpp"

namespace loadforge::contracts {
namespace {

using workloads::compression::testing::LastSlotExecutor;
using workloads::compression::testing::SerialExecutor;
using workloads::compression::testing::ThreadedExecutor;

struct CompressionSerial {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};

struct CompressionThreaded {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ThreadedExecutor>(7); }
};

struct CompressionLastSlot {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<LastSlotExecutor>(4); }
};

using CompressionExecutors =
    ::testing::Types<CompressionSerial, CompressionThreaded, CompressionLastSlot>;
INSTANTIATE_TYPED_TEST_SUITE_P(CompressionMock, ExecutorContract, CompressionExecutors);

}  // namespace
}  // namespace loadforge::contracts
