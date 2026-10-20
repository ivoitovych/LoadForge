// SPDX-License-Identifier: GPL-3.0-or-later
//
// The probe against IsaProbeContract -- over the real machine and over
// described ones -- and the real source's reads.
#include "platform/isa/probe.hpp"

#include <gtest/gtest.h>

#include <memory>

#include "contracts/simd/dispatch_contract.hpp"
#include "fake_source.hpp"

namespace loadforge::isa {
namespace {

using fakes::FakeSource;
using simd::IsaPath;
using simd::IsaSet;

TEST(SnapshotProbeTest, TheAnswerIsReadOnceAndDoesNotFollowTheSource) {
  FakeSource source(fakes::Machine::kFullX86);
  const SnapshotProbe probe(source, Architecture::kX86_64);
  const IsaSet first = probe.supported();
  source.xcr0 = 0;
  source.leaves.clear();
  EXPECT_EQ(probe.supported(), first);
  EXPECT_TRUE(first.contains(IsaPath::kAvx512));
}

TEST(RealSourceTest, CpuidAndXgetbvAreTheMachinesOnX86AndZeroElsewhere) {
  const RealIsaSource source;
#if defined(__x86_64__)
  EXPECT_GE(source.cpuid(0, 0).eax, 1U);
  const std::uint32_t leaf1_ecx = source.cpuid(1, 0).ecx;
  if ((leaf1_ecx & kEcxOsxsave) != 0U) {
    EXPECT_EQ(source.xgetbv(0) & 1U, 1U) << "x87 state is always saved";
  }
#else
  EXPECT_EQ(source.cpuid(0, 0).eax, 0U);
  EXPECT_EQ(source.xgetbv(0), 0U);
#endif
}

TEST(RealSourceTest, HwcapIsTheAuxiliaryVectorsWord) {
  const RealIsaSource source;
#if defined(__aarch64__)
  EXPECT_NE(source.hwcap() & kHwcapAsimd, 0U) << "every AArch64 Linux machine has Advanced SIMD";
#else
  EXPECT_NO_FATAL_FAILURE((void)source.hwcap());
#endif
}

}  // namespace
}  // namespace loadforge::isa

namespace loadforge::contracts {
namespace {

struct NativeSubject {
#if defined(__x86_64__)
  static constexpr const char* kArchitecture = "x86_64";
#elif defined(__aarch64__)
  static constexpr const char* kArchitecture = "aarch64";
#else
  static constexpr const char* kArchitecture = "other";
#endif
  static std::unique_ptr<simd::IsaProbe> make() { return isa::make_native_probe(); }
};

struct DescribedX86 {
  static constexpr const char* kArchitecture = "x86_64";
  static std::unique_ptr<simd::IsaProbe> make() {
    const isa::fakes::FakeSource source(isa::fakes::Machine::kFullX86);
    return std::make_unique<isa::SnapshotProbe>(source, isa::Architecture::kX86_64);
  }
};

struct DescribedAarch64 {
  static constexpr const char* kArchitecture = "aarch64";
  static std::unique_ptr<simd::IsaProbe> make() {
    const isa::fakes::FakeSource source(isa::fakes::Machine::kFullAarch64);
    return std::make_unique<isa::SnapshotProbe>(source, isa::Architecture::kAarch64);
  }
};

struct DescribedOther {
  static constexpr const char* kArchitecture = "other";
  static std::unique_ptr<simd::IsaProbe> make() {
    isa::fakes::FakeSource source(isa::fakes::Machine::kFullX86);
    source.hwcap_word = isa::kHwcapAsimd;
    return std::make_unique<isa::SnapshotProbe>(source, isa::Architecture::kOther);
  }
};

using Probes = ::testing::Types<NativeSubject, DescribedX86, DescribedAarch64, DescribedOther>;
INSTANTIATE_TYPED_TEST_SUITE_P(Probe, IsaProbeContract, Probes);

// The header also declares select_path()'s suite, which belongs to src/primitives/simd.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(SelectPathContract);

}  // namespace
}  // namespace loadforge::contracts
