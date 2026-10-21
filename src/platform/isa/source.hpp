// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_ISA_SOURCE_HPP
#define LOADFORGE_PLATFORM_ISA_SOURCE_HPP

// The probe's seam: the three machine facts the ISA decision rests on, behind
// one interface, so that every feature-bit combination a CPU or a kernel can
// present is a unit test rather than something the build machine happens to be.

#include <cstdint>

namespace loadforge::isa {

/// What CPUID returned in its four registers.
struct CpuidRegisters {
  std::uint32_t eax = 0;
  std::uint32_t ebx = 0;
  std::uint32_t ecx = 0;
  std::uint32_t edx = 0;
};

class IsaSource {
 public:
  IsaSource() = default;
  IsaSource(const IsaSource&) = delete;
  IsaSource(IsaSource&&) = delete;
  IsaSource& operator=(const IsaSource&) = delete;
  IsaSource& operator=(IsaSource&&) = delete;
  virtual ~IsaSource() = default;

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  /// CPUID with the given leaf and subleaf (x86-64 only).
  [[nodiscard]] virtual CpuidRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) const = 0;
  /// XGETBV with the given register index (x86-64 only). Valid only when CPUID
  /// leaf 1 reports OSXSAVE; the probe never calls it otherwise.
  [[nodiscard]] virtual std::uint64_t xgetbv(std::uint32_t index) const = 0;
  /// The auxiliary vector's AT_HWCAP. Zero when the kernel did not provide it.
  [[nodiscard]] virtual std::uint64_t hwcap() const = 0;
};

/// The running machine. On an architecture without the instruction, cpuid() and
/// xgetbv() answer zero: they describe a machine that has no such feature.
class RealIsaSource final : public IsaSource {
 public:
  [[nodiscard]] CpuidRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) const override;
  [[nodiscard]] std::uint64_t xgetbv(std::uint32_t index) const override;
  [[nodiscard]] std::uint64_t hwcap() const override;
};

}  // namespace loadforge::isa

#endif
