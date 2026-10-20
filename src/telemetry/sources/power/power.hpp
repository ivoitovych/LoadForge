// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_POWER_POWER_HPP
#define LOADFORGE_TELEMETRY_SOURCES_POWER_POWER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {

/// What the user can do about a sysfs attribute of the power providers that
/// could not be read, in the state it is in. The mechanism here is file
/// permission, so the advice is about files: CAP_PERFMON governs
/// perf_event_open and does nothing for a `0400` node (PLAN F3 -- revision 5
/// got this wrong). The perf provider's own remedy is perf_remediation().
[[nodiscard]] std::string powercap_remediation(topology::Availability kind, std::string_view path);

/// Package power (PLAN F3), as `power.package`: a counter of energy in
/// microjoules per package, by whichever provider offers it.
///
/// TWO PROVIDERS, DIFFERENT FAILURES, DIFFERENT FIXES.
///   powercap  /sys/class/powercap/intel-rapl:<N>/energy_uj, `0400 root` since
///             CVE-2020-8694; the counter wraps at max_energy_range_uj, which
///             is the metric's modulus. Fix: privilege, or read access to the file.
///   perf      the `power` PMU's energy-pkg event through perf_event_open,
///             system-wide on a CPU of the package. Fix: CAP_PERFMON, or a lower
///             kernel.perf_event_paranoid. Its counter is 64 bits wide, scaled
///             by the PMU's own energy-pkg.scale into microjoules.
/// Powercap is tried first (a read is cheaper than a counter held open); a
/// package it cannot serve is tried through perf. A package neither serves is
/// reported once, with the kind of the failure that can be acted on and the
/// remedy of EACH provider that failed, labelled -- and with no advice at all
/// when both simply lack the interface, because there is nothing to do.
///
/// Packages are numbered in the order powercap lists them (zones named
/// "package..."), or, with no powercap, in the order of the power PMU's cpumask,
/// which names one CPU per package.
///
/// Thread-safety: none. The descriptors are closed with the source.
class PowerSource final : public Source {
 public:
  static constexpr std::string_view kDefaultSysfsRoot = "/sys";

  PowerSource(platform::Syscalls& syscalls, platform::FileSystem& filesystem,
              std::string_view sysfs_root = kDefaultSysfsRoot);
  ~PowerSource() override;
  PowerSource(const PowerSource&) = delete;
  PowerSource(PowerSource&&) = delete;
  PowerSource& operator=(const PowerSource&) = delete;
  PowerSource& operator=(PowerSource&&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "power"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  /// One package served: by a powercap file (`fd` is -1) or a held counter.
  struct Channel {
    topology::Source energy;
    std::uint64_t modulus = 0;
    int fd = -1;
    double microjoules_per_count = 0.0;
    std::string subject;
  };

  void close_all();

  platform::Syscalls* syscalls_;
  platform::FileSystem* filesystem_;
  std::string root_;
  std::vector<Channel> channels_;
};

}  // namespace loadforge::telemetry

#endif
