// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_ISA_DETECT_HPP
#define LOADFORGE_PLATFORM_ISA_DETECT_HPP

#include <cstdint>

#include "platform/isa/source.hpp"
#include "primitives/simd/dispatch.hpp"

namespace loadforge::isa {

/// The architecture a probe describes. A probe never reports a path of another
/// one, whatever bits its source shows.
enum class Architecture : std::uint8_t { kX86_64, kAarch64, kOther };

[[nodiscard]] constexpr Architecture native_architecture() noexcept {
#if defined(__x86_64__)
  return Architecture::kX86_64;
#elif defined(__aarch64__)
  return Architecture::kAarch64;
#else
  return Architecture::kOther;
#endif
}

// CPUID leaf 1, ECX.
inline constexpr std::uint32_t kEcxFma = 1U << 12;
inline constexpr std::uint32_t kEcxOsxsave = 1U << 27;
inline constexpr std::uint32_t kEcxAvx = 1U << 28;
// CPUID leaf 7 subleaf 0, EBX.
inline constexpr std::uint32_t kEbxAvx2 = 1U << 5;
inline constexpr std::uint32_t kEbxAvx512f = 1U << 16;
// XCR0: x87, SSE, AVX (YMM_Hi128), opmask, ZMM_Hi256, Hi16_ZMM.
inline constexpr std::uint64_t kXcr0Ymm = (1U << 1) | (1U << 2);
inline constexpr std::uint64_t kXcr0Zmm = kXcr0Ymm | (1U << 5) | (1U << 6) | (1U << 7);
// AT_HWCAP on AArch64: HWCAP_ASIMD.
inline constexpr std::uint64_t kHwcapAsimd = 1U << 1;

/// What the machine behind `source` can execute, for `architecture`.
///
///   x86-64   kAvx2 needs AVX2 and FMA3 reported by the CPU, AVX and OSXSAVE,
///            and the OS saving the YMM state; kAvx512 needs AVX512F, OSXSAVE
///            and the OS saving the opmask and ZMM state as well as the YMM.
///            A leaf above the CPU's highest is not read: it would return the
///            highest leaf's data, which is a guess.
///   AArch64  kNeon needs HWCAP_ASIMD.
///
/// kScalar always; nothing is guessed upward.
[[nodiscard]] simd::IsaSet detect_paths(const IsaSource& source, Architecture architecture);

}  // namespace loadforge::isa

#endif
