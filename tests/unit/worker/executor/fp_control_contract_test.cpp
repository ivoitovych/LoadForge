// SPDX-License-Identifier: GPL-3.0-or-later
//
// The executor consumes FpControl (fpenv.hpp): each thread reads its own
// registers on start. Its tests stand in for the registers with their own
// model (fakes.hpp), and ADR 0003 asks that the provider's suite be run
// against that model, so that this stream's reading of the contract and the
// numerics stream's are compared by execution.

#include <memory>

#include "contracts/fpenv/fpenv_contract.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "unit/worker/executor/fakes.hpp"

// In the suite's own namespace: the instantiation names the suite unqualified.
namespace loadforge::contracts {
namespace {

using worker::testing::ArmFpControl;
using worker::testing::X86FpControl;

template <typename Model>
struct Modelled {
  static constexpr bool kHasDenormalsAreZero = Model::kHasDenormalsAreZero;
  static constexpr bool kHasDefaultNan = Model::kHasDefaultNan;
  static std::unique_ptr<fpenv::FpControl> make() { return std::make_unique<Model>(); }
};

using ExecutorFpModels = ::testing::Types<Modelled<X86FpControl>, Modelled<ArmFpControl>>;
INSTANTIATE_TYPED_TEST_SUITE_P(ExecutorModel, FpControlContract, ExecutorFpModels);

}  // namespace

// The header declares check()'s suite beside FpControl's. The executor
// does not call check(): a thread compares its registers with its creator's,
// and judging the creator's against the required state is the worker's, before
// it makes the pool. So there is no reading of check() here to run it against.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(FpCheckContract);

}  // namespace loadforge::contracts
