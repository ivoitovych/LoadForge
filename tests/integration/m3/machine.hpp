// SPDX-License-Identifier: GPL-3.0-or-later
//
// A machine for the integration tier: the kernel's files, served through the
// substitutable seam, in the layout the kernel documents. Nothing here reads a
// real sensor, so a full Load Signature built on it is "from fixtures alone"
// (M3 exit criterion 1).
//
// WHAT IS REAL AND WHAT IS MODELLED (F21)
//   /proc/stat and /proc/meminfo     captured: tests/fixtures/procfs.
//   hwmon, thermal zone, fan, cpufreq    modelled on Documentation/hwmon/
//                                        sysfs-interface.rst and the cpufreq
//                                        ABI; the development machine has none.
//   powercap and EDAC                modelled on Documentation/power/powercap/
//                                    powercap.rst and Documentation/ABI/testing/
//                                    sysfs-devices-edac.
// The real-kernel half of the tier (real_kernel_test.cpp) runs the same stack
// on the real /sys and /proc.

#ifndef LOADFORGE_TESTS_INTEGRATION_M3_MACHINE_HPP
#define LOADFORGE_TESTS_INTEGRATION_M3_MACHINE_HPP

#include <cstdint>
#include <filesystem>
#include <string>

#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::m3 {

constexpr std::uint32_t kPerfHardware = 0;  // PERF_TYPE_HARDWARE
constexpr std::uint32_t kPerfSoftware = 1;  // PERF_TYPE_SOFTWARE

inline std::filesystem::path fixtures() {
  return std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures";
}

/// The files of a machine that offers everything: two temperature sensors on a
/// package chip, a thermal zone, a fan, two CPUs with cpufreq, RAPL energy for
/// two packages, two EDAC controllers, and the performance counters.
class Machine {
 public:
  testing::FakeSyscalls& syscalls() { return fake_; }

  void install_everything() {
    install_hwmon();
    install_thermal_zone();
    install_fan();
    install_cpufreq();
    fake_.add_tree(fixtures() / "procfs" / "stat" / "present" / "proc", "/proc");
    fake_.add_tree(fixtures() / "procfs" / "meminfo" / "present" / "proc", "/proc");
    fake_.add_tree(fixtures() / "sysfs" / "powercap" / "intel-two-packages" / "class",
                   "/sys/class");
    fake_.add_tree(fixtures() / "sysfs" / "edac" / "two-controllers" / "devices", "/sys/devices");
    fake_.add_file("/proc/sys/kernel/perf_event_paranoid", "1\n");
  }

  void install_hwmon() {
    fake_.add_file("/sys/class/hwmon/hwmon0/name", "coretemp\n");
    fake_.add_file("/sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
    fake_.add_file("/sys/class/hwmon/hwmon0/temp1_input", "45000\n");
    fake_.add_file("/sys/class/hwmon/hwmon0/temp2_label", "Core 0\n");
    fake_.add_file("/sys/class/hwmon/hwmon0/temp2_input", "43000\n");
  }
  void install_thermal_zone() {
    fake_.add_file("/sys/class/thermal/thermal_zone0/type", "x86_pkg_temp\n");
    fake_.add_file("/sys/class/thermal/thermal_zone0/temp", "46000\n");
  }
  void install_fan() {
    fake_.add_file("/sys/class/hwmon/hwmon1/name", "nct6775\n");
    fake_.add_file("/sys/class/hwmon/hwmon1/fan1_input", "1200\n");
  }
  void install_cpufreq() {
    fake_.add_file("/sys/devices/system/cpu/online", "0-1\n");
    fake_.add_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "2400000\n");
    fake_.add_file("/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq", "2600000\n");
  }

  void set(const std::string& path, const std::string& content) {
    fake_.add_file(path, content + "\n");
  }
  void remove(const std::string& path) { fake_.remove_file(path); }

 private:
  testing::FakeSyscalls fake_;
};

}  // namespace loadforge::m3

#endif
