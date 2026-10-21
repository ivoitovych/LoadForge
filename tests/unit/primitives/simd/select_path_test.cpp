// SPDX-License-Identifier: GPL-3.0-or-later
//
// select_path(): the contract suite against the real function, and the parts
// the suite does not pin -- the messages a person reads, and the full table of
// what is chosen from every pair of sets.
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "contracts/simd/select_path_contract.hpp"
#include "primitives/simd/dispatch.hpp"

namespace loadforge::simd {
namespace {

TEST(SelectPathTest, TheNotCompiledMessageNamesThePathAndWhatTheBuildHolds) {
  const auto chosen = select_path(IsaPath::kNeon, IsaSet{IsaPath::kScalar, IsaPath::kNeon},
                                  IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512});
  ASSERT_FALSE(chosen.has_value());
  EXPECT_NE(chosen.error().detail.find("neon"), std::string::npos) << chosen.error().detail;
  EXPECT_NE(chosen.error().detail.find("compiled: avx512, avx2, scalar"), std::string::npos)
      << chosen.error().detail;
}

TEST(SelectPathTest, TheNotSupportedMessageNamesThePathAndWhatTheMachineHas) {
  const auto chosen = select_path(IsaPath::kAvx512, IsaSet{IsaPath::kScalar, IsaPath::kAvx2},
                                  IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512});
  ASSERT_FALSE(chosen.has_value());
  EXPECT_NE(chosen.error().detail.find("avx512"), std::string::npos) << chosen.error().detail;
  EXPECT_NE(chosen.error().detail.find("supported: avx2, scalar"), std::string::npos)
      << chosen.error().detail;
}

TEST(SelectPathTest, AnEmptySetIsNamedNone) {
  const auto chosen = select_path(IsaPath::kAvx2, IsaSet{}, IsaSet{});
  ASSERT_FALSE(chosen.has_value());
  EXPECT_NE(chosen.error().detail.find("compiled: none"), std::string::npos)
      << chosen.error().detail;
}

TEST(SelectPathTest, ARequestOutsideTheEnumerationIsNotCompiled) {
  const auto chosen =
      select_path(static_cast<IsaPath>(99), IsaSet{IsaPath::kScalar}, IsaSet{IsaPath::kScalar});
  ASSERT_FALSE(chosen.has_value());
  EXPECT_EQ(chosen.error().fault, DispatchFault::kNotCompiled);
  EXPECT_NE(chosen.error().detail.find("unrecognised ISA path"), std::string::npos);
}

TEST(SelectPathTest, EveryPairOfSetsChoosesTheWidestInTheIntersectionOrScalar) {
  const IsaPath all[] = {IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon};
  for (unsigned supported_bits = 0; supported_bits < 16; ++supported_bits) {
    for (unsigned compiled_bits = 0; compiled_bits < 16; ++compiled_bits) {
      IsaSet supported;
      IsaSet compiled;
      for (unsigned i = 0; i < 4; ++i) {
        if (((supported_bits >> i) & 1U) != 0U) {
          supported = supported.with(all[i]);
        }
        if (((compiled_bits >> i) & 1U) != 0U) {
          compiled = compiled.with(all[i]);
        }
      }
      IsaPath expected = IsaPath::kScalar;
      for (const IsaPath path : kPreference) {
        if (supported.contains(path) && compiled.contains(path)) {
          expected = path;
          break;
        }
      }
      const auto chosen = select_path(std::nullopt, supported, compiled);
      ASSERT_TRUE(chosen.has_value());
      EXPECT_EQ(chosen.value(), expected) << supported_bits << " " << compiled_bits;
    }
  }
}

}  // namespace
}  // namespace loadforge::simd

namespace loadforge::contracts {
namespace {

struct RealSelect {
  static core::Result<simd::IsaPath, simd::DispatchError> select(
      std::optional<simd::IsaPath> requested, simd::IsaSet supported, simd::IsaSet compiled) {
    return simd::select_path(requested, supported, compiled);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Real, SelectPathContract, ::testing::Types<RealSelect>);

// The header also declares the probe's suite, which belongs to src/platform/isa.

}  // namespace
}  // namespace loadforge::contracts
