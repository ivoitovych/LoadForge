// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_FPENV_REGISTERS_HPP
#define LOADFORGE_PLATFORM_FPENV_REGISTERS_HPP

// The two control registers' layouts, as pure functions of the register's
// value. They are compiled on both architectures and used on the one that has
// the register, so that each layout is unit-tested wherever the tests run;
// only the instruction that moves the value is architecture-specific
// (real_control.cpp).
//
// Both encode functions change only the bits this module owns -- rounding,
// flush-to-zero, denormals-are-zero, default-NaN -- and carry every other bit
// through: the exception masks and the sticky exception flags are not ours to
// disturb.

#include <cstdint>

#include "platform/fpenv/fpenv.hpp"

namespace loadforge::fpenv {

/// x86-64 MXCSR (Intel SDM vol. 1, §10.2.3).
///   bit 6 DAZ, bits 13-14 RC (00 nearest, 01 down, 10 up, 11 toward zero),
///   bit 15 FTZ. Has no default-NaN control.
inline constexpr std::uint32_t kMxcsrDaz = 1U << 6;
inline constexpr std::uint32_t kMxcsrFtz = 1U << 15;
inline constexpr unsigned kMxcsrRoundingShift = 13;

[[nodiscard]] FpState decode_mxcsr(std::uint32_t mxcsr) noexcept;
[[nodiscard]] std::uint32_t encode_mxcsr(std::uint32_t mxcsr, const FpState& state) noexcept;

/// AArch64 FPCR (Arm ARM, D24.2.67).
///   bits 22-23 RMode (00 nearest, 01 up, 10 down, 11 toward zero),
///   bit 24 FZ, bit 25 DN. FZ covers inputs and outputs, so there is no
///   separate denormals-are-zero.
inline constexpr std::uint64_t kFpcrFz = std::uint64_t{1} << 24;
inline constexpr std::uint64_t kFpcrDn = std::uint64_t{1} << 25;
inline constexpr unsigned kFpcrRoundingShift = 22;

[[nodiscard]] FpState decode_fpcr(std::uint64_t fpcr) noexcept;
[[nodiscard]] std::uint64_t encode_fpcr(std::uint64_t fpcr, const FpState& state) noexcept;

}  // namespace loadforge::fpenv

#endif
