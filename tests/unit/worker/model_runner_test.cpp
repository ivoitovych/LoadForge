// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 3: the provider's suite of the runner contract, run against
// the worker's own model of it. A model that fails a test here is a reading of
// the contract that the input-integrity stream did not share.

#include <gtest/gtest.h>

#include "contracts/verification/runner_contract.hpp"
#include "unit/worker/model_runner.hpp"

namespace loadforge::contracts {
namespace {

struct ModelRunnerSubject {
  static core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError> make(
      const verification::RunnerSetup& setup) {
    return worker_testing::ModelRunner::make(setup);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(WorkerModel, SliceRunnerContract,
                               ::testing::Types<ModelRunnerSubject>);

}  // namespace
}  // namespace loadforge::contracts
