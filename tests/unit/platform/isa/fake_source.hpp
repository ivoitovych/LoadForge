// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_PLATFORM_ISA_FAKE_SOURCE_HPP
#define LOADFORGE_TESTS_UNIT_PLATFORM_ISA_FAKE_SOURCE_HPP

// LOADFORGE P7 FIXTURE BUILDER: this file builds the machine states the ISA
// probe must survive -- a CPU that reports a feature the OS does not save state
// for, a CPU with fewer leaves than the probe wants, an auxiliary vector with
// no capability word -- as an IsaSource a test can aim at one bit at a time.

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "platform/isa/detect.hpp"
#include "platform/isa/source.hpp"

namespace loadforge::isa::fakes {

/// A machine described by the registers it presents. It refuses the reads real
/// hardware refuses: XGETBV without OSXSAVE faults (#UD) and a CPUID leaf above
/// the highest returns the highest leaf's data, so either read is recorded as
/// a fault the test then asserts did not happen.
enum class Machine : std::uint8_t { kBlank, kFullX86, kFullAarch64 };

class FakeSource final : public IsaSource {
 public:
  explicit FakeSource(Machine machine = Machine::kBlank) {
    if (machine == Machine::kFullX86) {
      // A Haswell-or-later x86-64 machine with AVX-512: every feature the probe
      // reads, and an OS that saves all the state.
      highest_leaf = 0x16;
      leaves[{1, 0}] = {0, 0, kEcxFma | kEcxOsxsave | kEcxAvx, 0};
      leaves[{7, 0}] = {0, kEbxAvx2 | kEbxAvx512f, 0, 0};
      xcr0 = kXcr0Zmm | 1U;
    }
    if (machine == Machine::kFullAarch64) {
      hwcap_word = kHwcapAsimd | 0x3ffU;  // asimd plus fp, evtstrm, aes ... as a kernel sets them
    }
  }

  [[nodiscard]] CpuidRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) const override {
    leaves_read.emplace_back(leaf, subleaf);
    if (leaf > highest_leaf) {
      ++faults;
      return {};
    }
    if (leaf == 0) {
      return {highest_leaf, 0, 0, 0};
    }
    const auto found = leaves.find({leaf, subleaf});
    return found == leaves.end() ? CpuidRegisters{} : found->second;
  }

  [[nodiscard]] std::uint64_t xgetbv(std::uint32_t index) const override {
    ++xgetbv_calls;
    const CpuidRegisters leaf1 = leaves.count({1, 0}) != 0 ? leaves.at({1, 0}) : CpuidRegisters{};
    if ((leaf1.ecx & kEcxOsxsave) == 0 || index != 0) {
      ++faults;
      return 0;
    }
    return xcr0;
  }

  [[nodiscard]] std::uint64_t hwcap() const override {
    ++hwcap_calls;
    return hwcap_word;
  }

  std::uint32_t highest_leaf = 7;
  std::map<std::pair<std::uint32_t, std::uint32_t>, CpuidRegisters> leaves;
  std::uint64_t xcr0 = 0;
  std::uint64_t hwcap_word = 0;

  mutable std::vector<std::pair<std::uint32_t, std::uint32_t>> leaves_read;
  mutable int xgetbv_calls = 0;
  mutable int hwcap_calls = 0;
  mutable int faults = 0;
};

}  // namespace loadforge::isa::fakes

#endif
