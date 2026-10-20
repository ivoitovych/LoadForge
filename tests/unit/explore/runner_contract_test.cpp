// SPDX-License-Identifier: GPL-3.0-or-later
//
// The trial-runner contract's suite, run against this stream's own mock
// (ADR 0003): the cross-execution that shows the mock reads the contract the
// way its provider's suite does.

#include <memory>

#include "contracts/explore/trial_contract.hpp"
#include "unit/explore/model_runner.hpp"

namespace loadforge::contracts {
namespace {

struct ModelSubject {
  static constexpr const char* kPresentGauge = "thermal.package";
  static std::unique_ptr<explore::TrialRunner> make() {
    return std::make_unique<explore_test::ModelRunner>(
        [](const explore::Candidate&, std::int64_t, std::size_t) { return 50.0; });
  }
  static explore::Candidate candidate() { return explore_test::make_candidate("model"); }
};

using ModelSubjects = ::testing::Types<ModelSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(Model, TrialRunnerContract, ModelSubjects);

}  // namespace
}  // namespace loadforge::contracts
