// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 3: the provider's suite of the record encoding, run against
// the worker's own reading of it.

#include "unit/worker/model_codec.hpp"

#include <gtest/gtest.h>

#include "contracts/verification/record_contract.hpp"

namespace loadforge::contracts {
namespace {

INSTANTIATE_TYPED_TEST_SUITE_P(WorkerModel, RecordCodecContract,
                               ::testing::Types<worker_testing::ModelCodec>);

}  // namespace
}  // namespace loadforge::contracts
