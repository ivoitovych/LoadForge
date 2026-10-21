// SPDX-License-Identifier: GPL-3.0-or-later
//
// The executor consumes FpControl (fpenv.hpp): each thread reads its own
// registers on start. Its tests stand in for the registers with their own
// model (fakes.hpp), and ADR 0003 asks that the provider's suite be run
// against that model, so that this stream's reading of the contract and the
// numerics stream's are compared by execution.

#include "contracts/fpenv/fp_control_contract.hpp"

#include <memory>

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

}  // namespace loadforge::contracts
