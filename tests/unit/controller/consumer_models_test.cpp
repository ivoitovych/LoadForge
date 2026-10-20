// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 3: the providers' suites for the contracts the controller and
// the command line consume, run against their own models of them.

#include <gtest/gtest.h>

#include "contracts/fpenv/fpenv_contract.hpp"
#include "contracts/simd/dispatch_contract.hpp"
#include "contracts/telemetry/telemetry_contract.hpp"
#include "contracts/verification/isolation_contract.hpp"
#include "unit/controller/consumer_models.hpp"

namespace loadforge::contracts {
namespace {

using controller_testing::ModelFp;
using controller_testing::ModelIsolator;
using controller_testing::ModelProbe;
using controller_testing::ModelSelect;
using controller_testing::ModelSource;

struct SourceSubject {
  static std::unique_ptr<telemetry::Source> make() { return std::make_unique<ModelSource>(); }
  static constexpr int kRounds = 3;
};
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerModel, SourceContract, ::testing::Types<SourceSubject>);

template <bool kDaz, bool kDn>
struct FpSubject {
  static constexpr bool kHasDenormalsAreZero = kDaz;
  static constexpr bool kHasDefaultNan = kDn;
  static std::unique_ptr<fpenv::FpControl> make() { return std::make_unique<ModelFp<kDaz, kDn>>(); }
};
using FpSubjects = ::testing::Types<FpSubject<true, false>, FpSubject<false, true>>;
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerModel, FpControlContract, FpSubjects);

struct X86Probe {
  static constexpr std::string_view kArchitecture = "x86_64";
  static std::unique_ptr<simd::IsaProbe> make() {
    return std::make_unique<ModelProbe>(
        simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2});
  }
};
struct ArmProbe {
  static constexpr std::string_view kArchitecture = "aarch64";
  static std::unique_ptr<simd::IsaProbe> make() {
    return std::make_unique<ModelProbe>(simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kNeon});
  }
};
using ProbeSubjects = ::testing::Types<X86Probe, ArmProbe>;
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerModel, IsaProbeContract, ProbeSubjects);
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerModel, SelectPathContract,
                               ::testing::Types<ModelSelect>);

struct IsolatorSubject {
  static std::unique_ptr<verification::Isolator> make() { return std::make_unique<ModelIsolator>(); }
};
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerModel, IsolatorContract,
                               ::testing::Types<IsolatorSubject>);

}  // namespace

// The headers register the suites of contracts this file does not consume.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(FpCheckContract);
}  // namespace loadforge::contracts
