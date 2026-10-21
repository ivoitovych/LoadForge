// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_SIMD_SELECT_PATH_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_SIMD_SELECT_PATH_CONTRACT_HPP

// Provider-side suite for simd::select_path in src/primitives/simd/dispatch.hpp
// (ADR 0003, 0004).
//
// SelectPathContract<Subject>: Subject::select is a function with the
// signature of simd::select_path. The provider instantiates it with a subject
// whose select calls its select_path; a consumer with its own stand-in.
//
//   struct Mine {
//     static core::Result<IsaPath, DispatchError> select(std::optional<IsaPath>,
//                                                        IsaSet, IsaSet);
//   };
//   INSTANTIATE_TYPED_TEST_SUITE_P(Mine, SelectPathContract, ::testing::Types<Mine>);

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string_view>

#include "primitives/simd/dispatch.hpp"

namespace loadforge::contracts {

template <typename Subject>
class SelectPathContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(SelectPathContract);

using simd::DispatchFault;
using simd::IsaPath;
using simd::IsaSet;

inline constexpr IsaPath kAllPaths[] = {IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512,
                                        IsaPath::kNeon};

TYPED_TEST_P(SelectPathContract, ARequestedPathThatIsCompiledAndSupportedIsUsed) {
  for (const IsaPath path : kAllPaths) {
    const auto chosen = TypeParam::select(path, IsaSet{path}, IsaSet{path});
    ASSERT_TRUE(chosen.has_value()) << simd::describe(path);
    EXPECT_EQ(chosen.value(), path);
  }
}

TYPED_TEST_P(SelectPathContract, ARequestedPathNotCompiledIsRefusedAsSuch) {
  const auto chosen = TypeParam::select(IsaPath::kAvx2, IsaSet{IsaPath::kScalar, IsaPath::kAvx2},
                                        IsaSet{IsaPath::kScalar, IsaPath::kNeon});
  ASSERT_FALSE(chosen.has_value());
  EXPECT_EQ(chosen.error().fault, DispatchFault::kNotCompiled);
  EXPECT_EQ(chosen.error().requested, IsaPath::kAvx2);
  EXPECT_FALSE(chosen.error().detail.empty());
}

TYPED_TEST_P(SelectPathContract, NotCompiledIsReportedBeforeNotSupported) {
  // Neither set holds it: the build's lack is the first thing to fix.
  const auto chosen =
      TypeParam::select(IsaPath::kAvx512, IsaSet{IsaPath::kScalar}, IsaSet{IsaPath::kScalar});
  ASSERT_FALSE(chosen.has_value());
  EXPECT_EQ(chosen.error().fault, DispatchFault::kNotCompiled);
}

TYPED_TEST_P(SelectPathContract, ARequestedPathTheMachineLacksIsRefusedNeverReplaced) {
  const auto chosen = TypeParam::select(IsaPath::kAvx512, IsaSet{IsaPath::kScalar, IsaPath::kAvx2},
                                        IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512});
  ASSERT_FALSE(chosen.has_value()) << "a request is used or refused, never silently replaced";
  EXPECT_EQ(chosen.error().fault, DispatchFault::kNotSupported);
  EXPECT_EQ(chosen.error().requested, IsaPath::kAvx512);
  EXPECT_FALSE(chosen.error().detail.empty());
}

TYPED_TEST_P(SelectPathContract, ScalarIsNeverRefusedWhateverTheSetsSay) {
  const auto requested = TypeParam::select(IsaPath::kScalar, IsaSet{}, IsaSet{});
  ASSERT_TRUE(requested.has_value());
  EXPECT_EQ(requested.value(), IsaPath::kScalar);
  const auto unrequested = TypeParam::select(std::nullopt, IsaSet{}, IsaSet{});
  ASSERT_TRUE(unrequested.has_value());
  EXPECT_EQ(unrequested.value(), IsaPath::kScalar);
}

TYPED_TEST_P(SelectPathContract, WithoutARequestTheWidestPathBothSetsHoldIsChosen) {
  const IsaSet everything{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon};
  EXPECT_EQ(TypeParam::select(std::nullopt, everything, everything).value(), IsaPath::kAvx512);
  EXPECT_EQ(TypeParam::select(std::nullopt, IsaSet{IsaPath::kAvx2, IsaPath::kAvx512},
                              IsaSet{IsaPath::kScalar, IsaPath::kAvx2})
                .value(),
            IsaPath::kAvx2)
      << "the intersection decides, not either set alone";
  EXPECT_EQ(TypeParam::select(std::nullopt, IsaSet{IsaPath::kNeon}, IsaSet{IsaPath::kNeon}).value(),
            IsaPath::kNeon);
  EXPECT_EQ(TypeParam::select(std::nullopt, IsaSet{IsaPath::kAvx2}, IsaSet{IsaPath::kNeon}).value(),
            IsaPath::kScalar)
      << "nothing in common: scalar, and no failure";
}

REGISTER_TYPED_TEST_SUITE_P(SelectPathContract, ARequestedPathThatIsCompiledAndSupportedIsUsed,
                            ARequestedPathNotCompiledIsRefusedAsSuch,
                            NotCompiledIsReportedBeforeNotSupported,
                            ARequestedPathTheMachineLacksIsRefusedNeverReplaced,
                            ScalarIsNeverRefusedWhateverTheSetsSay,
                            WithoutARequestTheWidestPathBothSetsHoldIsChosen);

}  // namespace loadforge::contracts

#endif
