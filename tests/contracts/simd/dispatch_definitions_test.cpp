// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions src/primitives/simd/dispatch.hpp carries: the path names,
// the set, the build's compiled paths and the kernel table.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "primitives/simd/dispatch.hpp"

namespace loadforge::simd {
namespace {

TEST(IsaPathDefinition, EveryPathHasItsNameAndTheNamesRoundTrip) {
  EXPECT_EQ(describe(IsaPath::kScalar), "scalar");
  EXPECT_EQ(describe(IsaPath::kAvx2), "avx2");
  EXPECT_EQ(describe(IsaPath::kAvx512), "avx512");
  EXPECT_EQ(describe(IsaPath::kNeon), "neon");
  EXPECT_EQ(describe(static_cast<IsaPath>(200)), "unrecognised ISA path");
  for (const IsaPath path : {IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon}) {
    EXPECT_EQ(parse_isa_path(describe(path)), path);
  }
  EXPECT_EQ(parse_isa_path("AVX2"), std::nullopt) << "the names are exact";
  EXPECT_EQ(parse_isa_path(""), std::nullopt);
  EXPECT_EQ(parse_isa_path("sve"), std::nullopt);
}

TEST(IsaPathDefinition, TheEncodingIsFixed) {
  EXPECT_EQ(static_cast<std::uint8_t>(IsaPath::kScalar), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(IsaPath::kAvx2), 1);
  EXPECT_EQ(static_cast<std::uint8_t>(IsaPath::kAvx512), 2);
  EXPECT_EQ(static_cast<std::uint8_t>(IsaPath::kNeon), 3);
}

TEST(IsaSetDefinition, MembershipUnionAndIntersection) {
  const IsaSet none;
  EXPECT_TRUE(none.empty());
  EXPECT_FALSE(none.contains(IsaPath::kScalar));
  const IsaSet two{IsaPath::kScalar, IsaPath::kAvx2};
  EXPECT_FALSE(two.empty());
  EXPECT_TRUE(two.contains(IsaPath::kAvx2));
  EXPECT_FALSE(two.contains(IsaPath::kNeon));
  EXPECT_EQ(two.with(IsaPath::kNeon), (IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kNeon}));
  EXPECT_EQ(two.intersect(IsaSet{IsaPath::kAvx2, IsaPath::kNeon}), IsaSet{IsaPath::kAvx2});
  EXPECT_FALSE(two.contains(static_cast<IsaPath>(200))) << "an unknown value is in no set";
  EXPECT_EQ(two.with(static_cast<IsaPath>(7)), two) << "and adding one changes nothing";
}

TEST(CompiledPathsDefinition, ScalarAlwaysAndOnlyTheNativeArchitecture) {
  const IsaSet compiled = compiled_paths();
  EXPECT_TRUE(compiled.contains(IsaPath::kScalar));
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(compiled, IsaSet{IsaPath::kScalar});
#elif defined(__x86_64__)
  EXPECT_EQ(compiled, (IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512}));
#elif defined(__aarch64__)
  EXPECT_EQ(compiled, (IsaSet{IsaPath::kScalar, IsaPath::kNeon}));
#endif
}

int scalar_kernel(int value) { return value + 1; }
int avx2_kernel(int value) { return value + 2; }
int neon_kernel(int value) { return value + 4; }

TEST(KernelTableDefinition, EachPathAnswersItsKernelOrNothing) {
  const KernelTable<int (*)(int)> table{.scalar = &scalar_kernel, .avx2 = &avx2_kernel};
  EXPECT_EQ(kernel_at(table, IsaPath::kScalar), &scalar_kernel);
  EXPECT_EQ(kernel_at(table, IsaPath::kAvx2), &avx2_kernel);
  EXPECT_EQ(kernel_at(table, IsaPath::kAvx512), nullptr);
  EXPECT_EQ(kernel_at(table, IsaPath::kNeon), nullptr);
  EXPECT_EQ(kernel_at(table, static_cast<IsaPath>(200)), nullptr);
  EXPECT_EQ(kernel_paths(table), (IsaSet{IsaPath::kScalar, IsaPath::kAvx2}));
  EXPECT_EQ(kernel_at(table, IsaPath::kAvx2)(1), 3);

  const KernelTable<int (*)(int)> arm{.scalar = &scalar_kernel, .neon = &neon_kernel};
  EXPECT_EQ(kernel_at(arm, IsaPath::kNeon), &neon_kernel);
  EXPECT_EQ(kernel_paths(arm), (IsaSet{IsaPath::kScalar, IsaPath::kNeon}));
}

TEST(KernelTableDefinition, ATableWithoutItsScalarKernelSaysSo) {
  const KernelTable<int (*)(int)> broken{.avx2 = &avx2_kernel};
  EXPECT_FALSE(kernel_paths(broken).contains(IsaPath::kScalar));
}

}  // namespace
}  // namespace loadforge::simd
