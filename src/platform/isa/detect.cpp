// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/isa/detect.hpp"

namespace loadforge::isa {
namespace {

[[nodiscard]] constexpr bool has_all(std::uint64_t value, std::uint64_t mask) noexcept {
  return (value & mask) == mask;
}

simd::IsaSet detect_x86(const IsaSource& source) {
  simd::IsaSet set{simd::IsaPath::kScalar};
  const std::uint32_t highest_leaf = source.cpuid(0, 0).eax;
  if (highest_leaf < 7) {
    return set;
  }
  const CpuidRegisters leaf1 = source.cpuid(1, 0);
  const CpuidRegisters leaf7 = source.cpuid(7, 0);
  if (!has_all(leaf1.ecx, kEcxOsxsave)) {
    return set;
  }
  const std::uint64_t xcr0 = source.xgetbv(0);
  if (has_all(leaf1.ecx, kEcxAvx | kEcxFma) && has_all(leaf7.ebx, kEbxAvx2) &&
      has_all(xcr0, kXcr0Ymm)) {
    set = set.with(simd::IsaPath::kAvx2);
  }
  if (has_all(leaf7.ebx, kEbxAvx512f) && has_all(xcr0, kXcr0Zmm)) {
    set = set.with(simd::IsaPath::kAvx512);
  }
  return set;
}

simd::IsaSet detect_aarch64(const IsaSource& source) {
  simd::IsaSet set{simd::IsaPath::kScalar};
  if (has_all(source.hwcap(), kHwcapAsimd)) {
    set = set.with(simd::IsaPath::kNeon);
  }
  return set;
}

}  // namespace

simd::IsaSet detect_paths(const IsaSource& source, Architecture architecture) {
  if (architecture == Architecture::kX86_64) {
    return detect_x86(source);
  }
  if (architecture == Architecture::kAarch64) {
    return detect_aarch64(source);
  }
  return simd::IsaSet{simd::IsaPath::kScalar};
}

}  // namespace loadforge::isa
