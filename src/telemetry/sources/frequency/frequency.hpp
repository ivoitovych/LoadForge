// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_SOURCES_FREQUENCY_FREQUENCY_HPP
#define LOADFORGE_TELEMETRY_SOURCES_FREQUENCY_FREQUENCY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/sources/thermal/channel.hpp"

namespace loadforge::telemetry {

/// The busy and total ticks of /proc/stat's aggregate line, as the two
/// counters this source reports. Ticks are USER_HZ jiffies.
struct CpuTicks {
  std::uint64_t busy = 0;
  std::uint64_t total = 0;
};

/// The share of the interval between two readings that the CPUs were busy,
/// 0 to 1, or nothing when the interval cannot be told.
///
/// Both deltas go through counter_delta, so a counter that wrapped between
/// the readings is advanced, not reported as a huge negative step. A reading
/// pair that moved backwards (the kernel's idle and iowait ticks occasionally
/// do, by a few ticks, when a CPU's idle accounting is reconciled) is not a
/// wrap: a 64-bit tick counter that wrapped advanced by a small amount, and
/// an advance past 2^63 is a step back. That, a total that did not move, and a
/// busy share larger than the total are all "cannot be told", never a number.
[[nodiscard]] std::optional<double> utilisation(CpuTicks previous, CpuTicks current) noexcept;

/// CPU frequency and utilisation.
///
///   frequency.cpu          gauge, hertz, one per online CPU: cpufreq's
///                          scaling_cur_freq (kHz in the file).
///   frequency.busy_ticks   counter, count: the aggregate /proc/stat line's
///                          user, nice, system, irq, softirq and steal.
///   frequency.total_ticks  counter, count: those plus idle and iowait.
///
/// Guest ticks are already inside user and nice and are not added twice. The
/// two counters wrap at 2^64 (modulus absent), which is what an unsigned long
/// does on a 64-bit kernel; a 32-bit kernel's narrower wrap would read as a
/// step back, and utilisation() reports that as "cannot be told".
///
/// A CPU with no cpufreq directory (a virtual machine, no driver, or offline)
/// is absent with that said. The set of CPUs is the `online` list read at
/// probe(); a CPU that goes offline mid-run vanishes.
class FrequencySource final : public Source {
 public:
  explicit FrequencySource(platform::FileSystem& filesystem, std::string root = {});

  [[nodiscard]] std::string_view name() const noexcept override { return "frequency"; }
  [[nodiscard]] std::vector<MetricStatus> probe() override;
  void sample(std::vector<Reading>& out) override;

 private:
  platform::FileSystem* filesystem_;
  std::string root_;
  std::vector<Channel> channels_;
  /// Set when /proc/stat was readable at probe(); the two counters follow
  /// the frequency channels in sample().
  std::optional<topology::Source> stat_;
};

}  // namespace loadforge::telemetry

#endif
