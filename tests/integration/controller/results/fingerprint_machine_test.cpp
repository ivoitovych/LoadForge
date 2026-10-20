// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: the fingerprint read from the machine this test runs on, through the real
// syscalls (M3 exit criterion 5: reproducible across two runs on one machine).
//
// What the machine has differs -- a virtual machine has no battery, a server no
// power supply -- so this asserts what must hold on every machine: two readings
// agree in everything but the fields that describe the moment, a reading's gaps
// are exactly its absent fields, and what the kernel itself says through
// uname(2) is what the fingerprint says.

#include <gtest/gtest.h>
#include <sys/utsname.h>

#include <string>
#include <string_view>

#include "controller/results/fingerprint.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::results {
namespace {

FingerprintReading read_machine() {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  FingerprintContext context;
  context.build = this_build();
  return read_fingerprint(filesystem, "", context);
}

TEST(FingerprintMachineTest, TwoReadingsOnOneMachineAgreeBeyondTheMoment) {
  const FingerprintReading first = read_machine();
  const FingerprintReading second = read_machine();
  for (const std::string_view field : differing_fields(first.fingerprint, second.fingerprint)) {
    EXPECT_TRUE(is_moment_field(field)) << field << " differs between two readings";
  }
  EXPECT_EQ(first.fingerprint.loadforge_version, second.fingerprint.loadforge_version);
  ASSERT_EQ(first.gaps.size(), second.gaps.size());
  for (std::size_t i = 0; i < first.gaps.size(); ++i) {
    EXPECT_EQ(first.gaps[i].field, second.gaps[i].field);
    EXPECT_EQ(first.gaps[i].why.kind, second.gaps[i].why.kind) << first.gaps[i].field;
  }
}

TEST(FingerprintMachineTest, TheKernelIsWhatTheKernelSaysItIs) {
  utsname name{};
  ASSERT_EQ(uname(&name), 0);
  const FingerprintReading reading = read_machine();
  ASSERT_TRUE(reading.fingerprint.kernel.has_value());
  EXPECT_EQ(*reading.fingerprint.kernel, std::string(name.release));
}

TEST(FingerprintMachineTest, EveryAbsentFileFieldHasItsGapAndEveryGapItsField) {
  const FingerprintReading reading = read_machine();
  const Fingerprint& f = reading.fingerprint;
  const auto has_gap = [&reading](std::string_view field) {
    for (const FingerprintGap& gap : reading.gaps) {
      if (gap.field == field) {
        return true;
      }
    }
    return false;
  };
  EXPECT_EQ(!f.cpu_model.has_value(), has_gap("cpu_model"));
  EXPECT_EQ(!f.microcode.has_value(), has_gap("microcode"));
  EXPECT_EQ(!f.smt_enabled.has_value(), has_gap("smt_enabled"));
  EXPECT_EQ(!f.power_source.has_value(), has_gap("power_source"));
  EXPECT_EQ(!f.battery_state.has_value(), has_gap("battery_state"));
  EXPECT_EQ(!f.cpufreq_governor.has_value(), has_gap("cpufreq_governor"));
  EXPECT_EQ(!f.platform_profile.has_value(), has_gap("platform_profile"));
  EXPECT_EQ(!f.turbo_enabled.has_value(), has_gap("turbo_enabled"));
  EXPECT_EQ(!f.huge_page_policy.has_value(), has_gap("huge_page_policy"));
  EXPECT_EQ(!f.temperature_at_start_c.has_value(), has_gap("temperature_at_start_c"));
  for (const FingerprintGap& gap : reading.gaps) {
    EXPECT_FALSE(gap.why.path.empty()) << gap.field;
    EXPECT_FALSE(gap.why.detail.empty()) << gap.field;
  }
}

}  // namespace
}  // namespace loadforge::results
