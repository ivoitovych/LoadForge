// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: the real machine's answer, compared with what the kernel itself says in
// /proc/cpuinfo -- read here as text, independently of the probe (F21). Linux
// clears a feature's flag there when the state it needs is not enabled in
// XCR0, so the flag list is the kernel's verdict on the same question.
#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "platform/isa/probe.hpp"

namespace loadforge::isa {
namespace {

using simd::IsaPath;

/// The feature words of the first processor block in /proc/cpuinfo.
std::set<std::string> kernel_flags() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
      key.pop_back();
    }
    if (key == "flags" || key == "Features") {
      std::istringstream words(line.substr(colon + 1));
      std::set<std::string> flags;
      for (std::string word; words >> word;) {
        flags.insert(word);
      }
      return flags;
    }
  }
  return {};
}

TEST(MachineTest, TheProbeAgreesWithTheKernelsFeatureFlags) {
  const std::set<std::string> flags = kernel_flags();
  ASSERT_FALSE(flags.empty()) << "/proc/cpuinfo has no feature line to compare with";
  const simd::IsaSet set = make_native_probe()->supported();
  const auto has = [&flags](const char* flag) { return flags.count(flag) != 0U; };
#if defined(__x86_64__)
  EXPECT_EQ(set.contains(IsaPath::kAvx2), has("avx2") && has("fma"));
  EXPECT_EQ(set.contains(IsaPath::kAvx512), has("avx512f"));
  EXPECT_FALSE(set.contains(IsaPath::kNeon));
#elif defined(__aarch64__)
  EXPECT_EQ(set.contains(IsaPath::kNeon), has("asimd"));
  EXPECT_FALSE(set.contains(IsaPath::kAvx2));
  EXPECT_FALSE(set.contains(IsaPath::kAvx512));
#endif
  EXPECT_TRUE(set.contains(IsaPath::kScalar));
}

#if defined(__x86_64__)
TEST(MachineTest, TheProbeAgreesWithTheCompilersOwnRuntimeCheck) {
  // __builtin_cpu_supports reads CPUID and XGETBV through the compiler runtime:
  // a second implementation of the same question, written by someone else.
  __builtin_cpu_init();
  const simd::IsaSet set = make_native_probe()->supported();
  EXPECT_EQ(set.contains(IsaPath::kAvx2),
            __builtin_cpu_supports("avx2") != 0 && __builtin_cpu_supports("fma") != 0);
  EXPECT_EQ(set.contains(IsaPath::kAvx512), __builtin_cpu_supports("avx512f") != 0);
}
#endif

}  // namespace
}  // namespace loadforge::isa
