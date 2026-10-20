// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every feature-bit combination the probe reads, one bit at a time: for each
// bit a path requires, a machine with all the others and not that one must not
// have the path; and a machine with everything must.
#include "platform/isa/detect.hpp"

#include <gtest/gtest.h>

#include "fake_source.hpp"

namespace loadforge::isa {
namespace {

using fakes::FakeSource;
using simd::IsaPath;
using simd::IsaSet;

TEST(DetectX86Test, AMachineWithEverythingHasEveryX86Path) {
  FakeSource source(fakes::Machine::kFullX86);
  EXPECT_EQ(detect_paths(source, Architecture::kX86_64),
            (IsaSet{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512}));
  EXPECT_EQ(source.faults, 0);
  EXPECT_EQ(source.hwcap_calls, 0) << "the auxiliary vector says nothing about x86 paths";
}

TEST(DetectX86Test, Avx2NeedsEachOfFmaAvxAndTheCpuBit) {
  for (const std::uint32_t ecx_bit : {kEcxFma, kEcxAvx}) {
    FakeSource source(fakes::Machine::kFullX86);
    source.leaves[{1, 0}].ecx &= ~ecx_bit;
    const IsaSet set = detect_paths(source, Architecture::kX86_64);
    EXPECT_FALSE(set.contains(IsaPath::kAvx2)) << ecx_bit;
    EXPECT_TRUE(set.contains(IsaPath::kAvx512)) << "AVX-512 does not wait on AVX2's bits";
  }
  FakeSource source(fakes::Machine::kFullX86);
  source.leaves[{7, 0}].ebx &= ~kEbxAvx2;
  const IsaSet set = detect_paths(source, Architecture::kX86_64);
  EXPECT_FALSE(set.contains(IsaPath::kAvx2));
  EXPECT_TRUE(set.contains(IsaPath::kAvx512));
}

TEST(DetectX86Test, Avx2WithoutTheOsSavingYmmStateIsNotSupported) {
  for (const std::uint64_t missing : {std::uint64_t{2}, std::uint64_t{4}}) {  // XCR0 bits 1, 2
    FakeSource source(fakes::Machine::kFullX86);
    source.xcr0 &= ~missing;
    const IsaSet set = detect_paths(source, Architecture::kX86_64);
    EXPECT_FALSE(set.contains(IsaPath::kAvx2)) << missing;
    EXPECT_FALSE(set.contains(IsaPath::kAvx512)) << "ZMM state includes YMM state";
  }
}

TEST(DetectX86Test, Avx512WithoutTheOsSavingOpmaskAndZmmStateIsNotSupported) {
  for (const std::uint64_t missing :
       {std::uint64_t{1} << 5, std::uint64_t{1} << 6, std::uint64_t{1} << 7}) {
    FakeSource source(fakes::Machine::kFullX86);
    source.xcr0 &= ~missing;
    const IsaSet set = detect_paths(source, Architecture::kX86_64);
    EXPECT_FALSE(set.contains(IsaPath::kAvx512)) << missing;
    EXPECT_TRUE(set.contains(IsaPath::kAvx2)) << "YMM state is still saved";
  }
}

TEST(DetectX86Test, Avx512NeedsTheCpuBit) {
  FakeSource source(fakes::Machine::kFullX86);
  source.leaves[{7, 0}].ebx &= ~kEbxAvx512f;
  const IsaSet set = detect_paths(source, Architecture::kX86_64);
  EXPECT_FALSE(set.contains(IsaPath::kAvx512));
  EXPECT_TRUE(set.contains(IsaPath::kAvx2));
}

TEST(DetectX86Test, WithoutOsxsaveNothingVectorIsSupportedAndXgetbvIsNeverRead) {
  FakeSource source(fakes::Machine::kFullX86);
  source.leaves[{1, 0}].ecx &= ~kEcxOsxsave;
  EXPECT_EQ(detect_paths(source, Architecture::kX86_64), IsaSet{IsaPath::kScalar});
  EXPECT_EQ(source.xgetbv_calls, 0);
  EXPECT_EQ(source.faults, 0);
}

TEST(DetectX86Test, ACpuWithoutLeafSevenIsNeverAskedForIt) {
  for (const std::uint32_t highest : {0U, 1U, 6U}) {
    FakeSource source(fakes::Machine::kFullX86);
    source.highest_leaf = highest;
    EXPECT_EQ(detect_paths(source, Architecture::kX86_64), IsaSet{IsaPath::kScalar}) << highest;
    EXPECT_EQ(source.faults, 0) << highest;
  }
}

TEST(DetectX86Test, ACpuThatReportsNothingIsScalarOnly) {
  FakeSource source;
  EXPECT_EQ(detect_paths(source, Architecture::kX86_64), IsaSet{IsaPath::kScalar});
}

TEST(DetectX86Test, TheAarch64CapabilityWordIsIgnored) {
  FakeSource source(fakes::Machine::kFullX86);
  source.hwcap_word = kHwcapAsimd;
  EXPECT_FALSE(detect_paths(source, Architecture::kX86_64).contains(IsaPath::kNeon));
}

TEST(DetectAarch64Test, AsimdInTheCapabilityWordIsNeon) {
  FakeSource source(fakes::Machine::kFullAarch64);
  EXPECT_EQ(detect_paths(source, Architecture::kAarch64),
            (IsaSet{IsaPath::kScalar, IsaPath::kNeon}));
  EXPECT_EQ(source.leaves_read.size(), 0U) << "CPUID is x86 and is never read for AArch64";
  EXPECT_EQ(source.xgetbv_calls, 0);
}

TEST(DetectAarch64Test, AMissingOrZeroCapabilityWordIsScalarOnly) {
  FakeSource source(fakes::Machine::kFullAarch64);
  source.hwcap_word = 0;
  EXPECT_EQ(detect_paths(source, Architecture::kAarch64), IsaSet{IsaPath::kScalar});
}

TEST(DetectAarch64Test, ACapabilityWordWithOtherBitsButNotAsimdIsScalarOnly) {
  FakeSource source(fakes::Machine::kFullAarch64);
  source.hwcap_word &= ~kHwcapAsimd;
  EXPECT_NE(source.hwcap_word, 0U);
  EXPECT_EQ(detect_paths(source, Architecture::kAarch64), IsaSet{IsaPath::kScalar});
}

TEST(DetectAarch64Test, TheX86FeatureBitsAreIgnored) {
  FakeSource source(fakes::Machine::kFullX86);
  source.hwcap_word = kHwcapAsimd;
  EXPECT_EQ(detect_paths(source, Architecture::kAarch64),
            (IsaSet{IsaPath::kScalar, IsaPath::kNeon}));
  EXPECT_EQ(source.leaves_read.size(), 0U);
}

TEST(DetectOtherTest, AnUnknownArchitectureIsScalarWhateverTheBitsSay) {
  FakeSource source(fakes::Machine::kFullX86);
  source.hwcap_word = kHwcapAsimd;
  EXPECT_EQ(detect_paths(source, Architecture::kOther), IsaSet{IsaPath::kScalar});
  EXPECT_EQ(source.leaves_read.size(), 0U);
  EXPECT_EQ(source.hwcap_calls, 0);
}

TEST(NativeArchitectureTest, MatchesTheCompiler) {
#if defined(__x86_64__)
  EXPECT_EQ(native_architecture(), Architecture::kX86_64);
#elif defined(__aarch64__)
  EXPECT_EQ(native_architecture(), Architecture::kAarch64);
#else
  EXPECT_EQ(native_architecture(), Architecture::kOther);
#endif
}

}  // namespace
}  // namespace loadforge::isa
