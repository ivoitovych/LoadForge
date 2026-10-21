// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_FPENV_REAL_CONTROL_HPP
#define LOADFORGE_PLATFORM_FPENV_REAL_CONTROL_HPP

#include "platform/fpenv/fpenv.hpp"

namespace loadforge::fpenv {

/// The calling thread's real floating-point control register: MXCSR on
/// x86-64, FPCR on AArch64.
///
/// The register is read and written with the instruction itself, in inline
/// assembly: the portability workflow refuses the x86 intrinsics' names outside
/// src/primitives/simd/x86/, and the compilers' own builtins differ in what they
/// promise about ordering. A write is a read-modify-write that changes only the
/// bits FpState names (registers.hpp).
class RealFpControl final : public FpControl {
 public:
#if defined(__x86_64__)
  static constexpr bool kHasDenormalsAreZero = true;
  static constexpr bool kHasDefaultNan = false;
#else
  static constexpr bool kHasDenormalsAreZero = false;
  static constexpr bool kHasDefaultNan = true;
#endif

  [[nodiscard]] FpState read() const override;
  void write(const FpState& state) override;
};

}  // namespace loadforge::fpenv

#endif
