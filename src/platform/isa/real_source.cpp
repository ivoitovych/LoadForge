// SPDX-License-Identifier: GPL-3.0-or-later
#include <sys/auxv.h>

#include <cstdint>

#include "platform/isa/source.hpp"

namespace loadforge::isa {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
CpuidRegisters RealIsaSource::cpuid([[maybe_unused]] std::uint32_t leaf,
                                    [[maybe_unused]] std::uint32_t subleaf) const {
  CpuidRegisters out;
#if defined(__x86_64__)
  asm volatile("cpuid"
               : "=a"(out.eax), "=b"(out.ebx), "=c"(out.ecx), "=d"(out.edx)
               : "a"(leaf), "c"(subleaf));
#endif
  return out;
}

std::uint64_t RealIsaSource::xgetbv([[maybe_unused]] std::uint32_t index) const {
  std::uint64_t value = 0;
#if defined(__x86_64__)
  std::uint32_t low = 0;   // NOLINT(misc-const-correctness): written by the instruction
  std::uint32_t high = 0;  // NOLINT(misc-const-correctness): written by the instruction
  asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(index));
  value = (std::uint64_t{high} << 32) | low;
#endif
  return value;
}

std::uint64_t RealIsaSource::hwcap() const { return getauxval(AT_HWCAP); }

}  // namespace loadforge::isa
