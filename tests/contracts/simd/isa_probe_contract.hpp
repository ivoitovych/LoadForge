// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_SIMD_ISA_PROBE_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_SIMD_ISA_PROBE_CONTRACT_HPP

// Provider-side suite for simd::IsaProbe in src/primitives/simd/dispatch.hpp
// (ADR 0003, 0004).
//
// IsaProbeContract<Subject>: Subject::make() returns a std::unique_ptr to an
// IsaProbe. Subject::kArchitecture names the architecture the probe claims to
// describe: "x86_64", "aarch64" or "other".

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string_view>

#include "primitives/simd/dispatch.hpp"

namespace loadforge::contracts {

using simd::IsaPath;
using simd::IsaSet;

template <typename Subject>
class IsaProbeContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(IsaProbeContract);

TYPED_TEST_P(IsaProbeContract, ScalarIsAlwaysSupported) {
  const std::unique_ptr<simd::IsaProbe> probe = TypeParam::make();
  EXPECT_TRUE(probe->supported().contains(IsaPath::kScalar));
}

TYPED_TEST_P(IsaProbeContract, NoPathOfAnotherArchitectureIsEverReported) {
  const std::unique_ptr<simd::IsaProbe> probe = TypeParam::make();
  const IsaSet set = probe->supported();
  const std::string_view architecture = TypeParam::kArchitecture;
  if (architecture != "x86_64") {
    EXPECT_FALSE(set.contains(IsaPath::kAvx2));
    EXPECT_FALSE(set.contains(IsaPath::kAvx512));
  }
  if (architecture != "aarch64") {
    EXPECT_FALSE(set.contains(IsaPath::kNeon));
  }
}

TYPED_TEST_P(IsaProbeContract, TheAnswerIsTheSameOnEveryCall) {
  const std::unique_ptr<simd::IsaProbe> probe = TypeParam::make();
  const IsaSet first = probe->supported();
  for (int call = 0; call < 3; ++call) {
    EXPECT_EQ(probe->supported(), first);
  }
  EXPECT_EQ(TypeParam::make()->supported(), first) << "and from a second probe in one process";
}

REGISTER_TYPED_TEST_SUITE_P(IsaProbeContract, ScalarIsAlwaysSupported,
                            NoPathOfAnotherArchitectureIsEverReported,
                            TheAnswerIsTheSameOnEveryCall);

}  // namespace loadforge::contracts

#endif
