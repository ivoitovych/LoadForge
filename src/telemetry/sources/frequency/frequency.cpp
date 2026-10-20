// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/frequency/frequency.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "resources/decimal.hpp"
#include "topology/cpu_list.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

// kHz in the file; 100 GHz is past any clock and below anything a bad read
// of a 32-bit field looks like.
constexpr std::int64_t kMaxKilohertz = 100'000'000;
constexpr double kHertzPerKilohertz = 1000.0;

// /proc/stat: user nice system idle iowait irq softirq steal guest guest_nice.
// Later fields appear on later kernels, so only the first four are required.
constexpr std::size_t kRequiredFields = 4;
constexpr std::size_t kCountedFields = 8;
constexpr std::size_t kIdleField = 3;
constexpr std::size_t kIowaitField = 4;

const char* const kStatPath = "/proc/stat";

Remedies frequency_remedies() {
  return make_remedies(
      "this machine does not offer cpufreq for this CPU: a virtual machine, a kernel without a "
      "cpufreq driver, or a CPU that is offline; nothing LoadForge can change here",
      "this CPU's cpufreq directory disappeared during the run (the CPU was taken offline or "
      "the driver unloaded); readings before and after belong to a different machine, so "
      "re-run once it is stable",
      "read access to this cpufreq attribute is refused; scaling_cur_freq is normally "
      "world-readable, so check the file's mode, owner and any udev rule rather than running "
      "privileged",
      "the cpufreq driver returned something that is not a frequency, and LoadForge does not "
      "guess; update the kernel or driver",
      "the read of this cpufreq attribute failed; check the kernel log (dmesg) for the "
      "driver's complaint");
}

Remedies stat_remedies() {
  return make_remedies(
      "this kernel does not provide /proc/stat (procfs is not mounted, as in some "
      "containers and chroots); mount procfs at /proc to get CPU utilisation",
      "/proc/stat disappeared during the run (procfs was unmounted); re-run with procfs "
      "mounted throughout",
      "read access to /proc/stat is refused, which is unusual (it is world-readable by "
      "default); a hidepid or sandbox policy may be in force, so check the mount options of "
      "/proc and any security module",
      "the first line of /proc/stat is not CPU tick counters, and LoadForge does not guess; "
      "this is a kernel or sandbox fault, so report the kernel version",
      "the read of /proc/stat failed; check the kernel log (dmesg)");
}

Remedies online_remedies() {
  return make_remedies(
      "this kernel does not publish the online CPU list, so the CPUs cpufreq could be read "
      "for cannot be named; nothing LoadForge can change here",
      "the online CPU list disappeared during the run; re-run once the machine is stable",
      "read access to the online CPU list is refused, which is unusual (it is world-readable "
      "by default); check the file's mode and any security module",
      "the online CPU list is not a CPU list, and LoadForge does not guess; this is a kernel "
      "or sandbox fault, so report the kernel version",
      "the read of the online CPU list failed; check the kernel log (dmesg)");
}

Metric frequency_metric(std::uint32_t cpu) {
  return Metric{"frequency.cpu", Unit::kHertz,      Kind::kGauge, Scope::kCpu,
                cpu,             std::nullopt,      "cpufreq"};
}

Metric tick_metric(std::string name) {
  return Metric{std::move(name), Unit::kCount, Kind::kCounter,    Scope::kMachine,
                0,               std::nullopt, "procfs:stat"};
}

/// The aggregate line's counters, or why the line is not one.
core::Result<CpuTicks, std::string> parse_stat_line(std::string_view line) {
  if (!line.starts_with("cpu ")) {
    return "the first line does not start with \"cpu \": \"" + std::string{line} + "\"";
  }
  line.remove_prefix(std::string_view{"cpu"}.size());
  CpuTicks ticks;
  std::uint64_t idle = 0;
  std::size_t field = 0;
  for (;;) {
    while (line.starts_with(' ')) {
      line.remove_prefix(1);
    }
    if (line.empty()) {
      break;
    }
    const std::size_t end = line.find(' ');
    const std::string_view token = line.substr(0, end);
    line.remove_prefix(token.size());
    const std::optional<std::uint64_t> value = resources::parse_decimal(token);
    if (!value.has_value()) {
      return "field " + std::to_string(field + 1) + " is not a tick count: \"" +
             std::string{token} + "\"";
    }
    // Sums wrap modulo 2^64, which is the arithmetic counter_delta uses.
    if (field == kIdleField || field == kIowaitField) {
      idle += *value;
    } else if (field < kCountedFields) {
      ticks.busy += *value;
    }
    ++field;
  }
  if (field < kRequiredFields) {
    return "only " + std::to_string(field) + " fields; the aggregate line has at least " +
           std::to_string(kRequiredFields);
  }
  ticks.total = ticks.busy + idle;
  return ticks;
}

/// One read of the aggregate line: the counters, or why there are none.
core::Result<CpuTicks, topology::Unavailable> read_ticks(topology::Source& stat) {
  auto line = stat.text();
  if (!line.has_value()) {
    return line.error();
  }
  auto ticks = parse_stat_line(line.value());
  if (!ticks.has_value()) {
    return topology::Unavailable{topology::Availability::kMalformed, stat.path(), ticks.error()};
  }
  return ticks.value();
}

}  // namespace

std::optional<double> utilisation(CpuTicks previous, CpuTicks current) noexcept {
  constexpr std::uint64_t kBackwards = std::uint64_t{1} << 63U;
  const std::uint64_t busy = counter_delta(previous.busy, current.busy, std::nullopt);
  const std::uint64_t total = counter_delta(previous.total, current.total, std::nullopt);
  if (busy >= kBackwards || total >= kBackwards || total == 0 || busy > total) {
    return std::nullopt;
  }
  return static_cast<double>(busy) / static_cast<double>(total);
}

FrequencySource::FrequencySource(platform::FileSystem& filesystem, std::string root)
    : filesystem_(&filesystem), root_(std::move(root)) {}

std::vector<MetricStatus> FrequencySource::probe() {
  channels_.clear();
  stat_.reset();
  std::vector<MetricStatus> statuses;

  const std::string cpu_directory = root_ + "/sys/devices/system/cpu";
  topology::Source online(*filesystem_, cpu_directory + "/online");
  auto cpus = online.cpu_list();
  if (!cpus.has_value()) {
    statuses.push_back(MetricStatus{frequency_metric(0),
                                    make_unavailable(cpus.error(), online_remedies())});
  } else {
    for (const std::uint32_t cpu : cpus.value().ids()) {
      GaugeSpec spec;
      spec.metric = frequency_metric(cpu);
      spec.path = cpu_directory + "/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
      spec.minimum = 0;
      spec.maximum = kMaxKilohertz;
      spec.scale = kHertzPerKilohertz;
      spec.remedies = frequency_remedies();
      Channel channel(*filesystem_, std::move(spec));
      MetricStatus status = channel.probe();
      if (!status.unavailable.has_value()) {
        channels_.push_back(std::move(channel));
      }
      statuses.push_back(std::move(status));
    }
  }

  stat_.emplace(*filesystem_, root_ + kStatPath);
  auto ticks = read_ticks(*stat_);
  for (const char* const name : {"frequency.busy_ticks", "frequency.total_ticks"}) {
    std::optional<Unavailable> unavailable;
    if (!ticks.has_value()) {
      unavailable = make_unavailable(ticks.error(), stat_remedies());
    }
    statuses.push_back(MetricStatus{tick_metric(name), std::move(unavailable)});
  }
  if (!ticks.has_value()) {
    stat_.reset();
  }
  return statuses;
}

void FrequencySource::sample(std::vector<Reading>& out) {
  for (Channel& channel : channels_) {
    out.push_back(channel.read());
  }
  if (!stat_.has_value()) {
    return;
  }
  auto ticks = read_ticks(*stat_);
  if (ticks.has_value()) {
    out.emplace_back(Counter{ticks.value().busy});
    out.emplace_back(Counter{ticks.value().total});
    return;
  }
  out.emplace_back(make_unavailable(ticks.error(), stat_remedies()));
  out.emplace_back(make_unavailable(ticks.error(), stat_remedies()));
}

}  // namespace loadforge::telemetry
