// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/counters/counters.hpp"

#include <linux/perf_event.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

struct EventSpec {
  std::string_view name;
  std::uint32_t type;
  std::uint64_t config;
  std::string_view provider;
};

constexpr std::string_view kShareMetric = "counters.pmu_share";

constexpr std::array<EventSpec, 5> kEvents{{
    {"counters.cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "perf:hardware"},
    {"counters.instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "perf:hardware"},
    {"counters.cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, "perf:hardware"},
    {"counters.branch_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, "perf:hardware"},
    {"counters.context_switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES,
     "perf:software"},
}};

/// Whether an event is counted by the PMU, which is shared between events and so
/// can be on the counter for only part of the time. Software events are not.
constexpr bool is_hardware(std::uint32_t type) noexcept { return type == PERF_TYPE_HARDWARE; }

/// Whether an event happens in the kernel, so that excluding the kernel excludes
/// the event itself: a context switch is counted by the scheduler. Established
/// on Linux 6.18 (docs/journal/m3-contract-corrections-*.md): with exclude_kernel
/// set the count of PERF_COUNT_SW_CONTEXT_SWITCHES stays 0 for ever.
constexpr bool counts_in_kernel(std::uint32_t type) noexcept { return type == PERF_TYPE_SOFTWARE; }

Metric make_share_metric() {
  Metric metric;
  metric.name = std::string{kShareMetric};
  metric.unit = Unit::kRatio;
  metric.kind = Kind::kGauge;
  metric.scope = Scope::kMachine;
  metric.provider = "perf:hardware";
  return metric;
}

Metric make_metric(const EventSpec& event) {
  Metric metric;
  metric.name = std::string{event.name};
  metric.unit = Unit::kCount;
  metric.kind = Kind::kCounter;
  metric.scope = Scope::kMachine;
  metric.provider = std::string{event.provider};
  return metric;
}

/// Fills a failure where it will live. Built in place, a field at a time, so
/// that no half-built Unavailable exists while the next string is allocated:
/// the cleanup of one is a branch no test can take (journal §1.13).
void fill_failure(Unavailable& why, topology::Availability kind,
                  const platform::SyscallError& error, const std::string& path,
                  std::optional<std::int64_t> paranoid, bool name_paranoid, bool kernel_side) {
  why.kind = kind;
  why.path = path;
  why.detail = platform::describe(error);
  if (name_paranoid && paranoid.has_value()) {
    why.detail += " (kernel.perf_event_paranoid is ";
    why.detail += std::to_string(*paranoid);
    why.detail += ")";
  }
  why.remediation = perf_remediation(kind, paranoid, path, kernel_side);
}

}  // namespace

topology::Availability classify_perf_open(int error) noexcept {
  switch (error) {
    case EACCES:
    case EPERM:
      return topology::Availability::kDenied;
    case ENOENT:
    case ENODEV:
    case EOPNOTSUPP:
    case ENOSYS:
    case EINVAL:
      return topology::Availability::kAbsent;
    default:
      // E2BIG, EBADF, EBUSY, EFAULT, EINTR, EMFILE, ENOSPC, EOVERFLOW, ESRCH:
      // each is this attempt failing, not the machine lacking the counter.
      return topology::Availability::kUnreadable;
  }
}

std::string perf_remediation(topology::Availability kind, std::optional<std::int64_t> paranoid,
                             std::string_view subject, bool kernel_side) {
  std::string text;
  switch (kind) {
    case topology::Availability::kDenied:
      if (kernel_side) {
        text =
            "grant LoadForge the CAP_PERFMON capability, or lower kernel.perf_event_paranoid to 1";
        if (paranoid.has_value()) {
          text += " (it is ";
          text += std::to_string(*paranoid);
          text += ")";
        }
        text +=
            "; this event happens in the kernel, so it can only be counted with the kernel "
            "included, which perf_event_paranoid above 1 refuses an unprivileged process";
        return text;
      }
      text = "grant LoadForge the CAP_PERFMON capability, or lower kernel.perf_event_paranoid";
      if (!paranoid.has_value()) {
        return text;
      }
      if (*paranoid > 2) {
        text += " (it is ";
        text += std::to_string(*paranoid);
        text +=
            ", and above 2 every unprivileged use is refused; 2 or below allows counting "
            "your own processes in user space)";
        return text;
      }
      text = "kernel.perf_event_paranoid is ";
      text += std::to_string(*paranoid);
      text +=
          ", which already allows counting your own processes in user space, so "
          "something else refuses perf_event_open (a container's seccomp profile or a "
          "security module); allow the call there, or grant LoadForge CAP_PERFMON";
      return text;
    case topology::Availability::kAbsent:
      text = "this machine does not offer ";
      text += subject;
      text +=
          " (a virtual machine without a PMU, a CPU without the event, or a kernel built "
          "without perf events); nothing can be changed from the software side";
      return text;
    case topology::Availability::kVanished:
      return "the CPU or process the counter followed went away (a CPU taken offline, or the "
             "process exited); counts before this point belong to a machine that has changed";
    case topology::Availability::kMalformed:
    case topology::Availability::kUnreadable:
    case topology::Availability::kPresent:
      break;
  }
  text = "using ";
  text += subject;
  text += " failed; the detail names the error, and the kernel log (dmesg) may say why";
  return text;
}

CountersSource::CountersSource(platform::Syscalls& syscalls, platform::FileSystem& filesystem,
                               std::string_view proc_root)
    : syscalls_(&syscalls), filesystem_(&filesystem), paranoid_path_(proc_root) {
  paranoid_path_ += "/sys/kernel/perf_event_paranoid";
}

CountersSource::~CountersSource() {
  for (const Channel& channel : channels_) {
    static_cast<void>(syscalls_->close(channel.fd));
  }
}

std::vector<MetricStatus> CountersSource::probe() {
  for (const Channel& channel : channels_) {
    static_cast<void>(syscalls_->close(channel.fd));
  }
  channels_.clear();
  paranoid_.reset();
  share_reported_ = false;
  topology::Source level_file{*filesystem_, paranoid_path_};
  auto level = level_file.integer();
  if (level.has_value()) {
    paranoid_ = level.value();
  }

  std::vector<MetricStatus> statuses;
  statuses.reserve(kEvents.size() + 1);
  Unavailable first_hardware_failure;  // for the share, should no hardware event be held
  const pid_t pid = syscalls_->process_id();
  for (const EventSpec& event : kEvents) {
    platform::PerfEventSpec spec;
    spec.type = event.type;
    spec.config = event.config;
    spec.pid = pid;
    // User space only for what the CPU counts, because that is what an
    // unprivileged process may count at perf_event_paranoid 2. Not for an event
    // that happens in the kernel: with the kernel excluded it reads 0 for ever,
    // a zero standing in for a value. That event needs the kernel included, and
    // so CAP_PERFMON or perf_event_paranoid 1 (perf_event_open(2): EACCES for
    // "not setting exclude_kernel when the paranoid setting requires it").
    const bool kernel_side = counts_in_kernel(event.type);
    spec.user_only = !kernel_side;
    spec.inherit = true;

    // One failure, filled where it lives (journal §1.13).
    const auto fail = [&](topology::Availability kind, const platform::SyscallError& error) {
      statuses.emplace_back();
      statuses.back().metric = make_metric(event);
      statuses.back().unavailable.emplace();
      fill_failure(*statuses.back().unavailable, kind, error, error.subject, paranoid_, true,
                   kernel_side);
      if (is_hardware(event.type) && first_hardware_failure.path.empty()) {
        const Unavailable& failure = *statuses.back().unavailable;
        first_hardware_failure.kind = failure.kind;
        first_hardware_failure.path = failure.path;
        first_hardware_failure.detail = failure.detail;
        first_hardware_failure.remediation = failure.remediation;
      }
    };

    auto opened = syscalls_->open_perf_event(spec);
    if (!opened.has_value()) {
      fail(classify_perf_open(opened.error().number), opened.error());
      continue;
    }
    const int fd = opened.value();
    // Reset then enable, so every count starts from zero at its own enabling.
    bool refused = false;
    for (const platform::PerfAction action :
         {platform::PerfAction::kReset, platform::PerfAction::kEnable}) {
      auto done = syscalls_->control_perf_event(fd, action);
      if (!done.has_value()) {
        fail(topology::Availability::kUnreadable, done.error());
        refused = true;
        break;
      }
    }
    if (refused) {
      static_cast<void>(syscalls_->close(fd));
      continue;
    }
    // Built in place, a field at a time (journal §1.13).
    Channel& channel = channels_.emplace_back();
    channel.fd = fd;
    channel.subject = "perf event type ";
    channel.subject += std::to_string(event.type);
    channel.subject += " config ";
    channel.subject += std::to_string(event.config);
    channel.hardware = is_hardware(event.type);
    channel.kernel_side = kernel_side;
    statuses.emplace_back();
    statuses.back().metric = make_metric(event);
  }
  report_share(statuses, first_hardware_failure);
  return statuses;
}

// The share of the time the hardware events were on the PMU is a metric of the
// source, present when any hardware event is held. When none is, it is absent for
// the same reason the first hardware event is (every one of them was refused or
// absent for the machine's reasons), and carries that event's words.
void CountersSource::report_share(std::vector<MetricStatus>& statuses,
                                  const Unavailable& first_hardware_failure) {
  for (const Channel& channel : channels_) {
    share_reported_ = share_reported_ || channel.hardware;
  }
  statuses.emplace_back();
  statuses.back().metric = make_share_metric();
  if (share_reported_) {
    return;
  }
  Unavailable& why = statuses.back().unavailable.emplace();
  why.kind = first_hardware_failure.kind;
  why.path = first_hardware_failure.path;
  why.detail = first_hardware_failure.detail;
  why.remediation = first_hardware_failure.remediation;
}

void CountersSource::sample(std::vector<Reading>& out) {
  double share = 1.0;  // the smallest share of time on the PMU among the hardware events read
  bool hardware_read = false;
  for (const Channel& channel : channels_) {
    auto count = syscalls_->read_perf_count(channel.fd);
    if (!count.has_value()) {
      const platform::SyscallError& error = count.error();
      Unavailable why;
      fill_failure(why, topology::classify(error, true), error, channel.subject, paranoid_, false,
                   channel.kernel_side);
      out.emplace_back(std::move(why));
      continue;
    }
    const platform::PerfCount& reading = count.value();
    if (reading.time_running == 0 && reading.time_enabled > 0) {
      Unavailable why;
      why.kind = topology::Availability::kUnreadable;
      why.path = channel.subject;
      why.detail = "enabled for ";
      why.detail += std::to_string(reading.time_enabled);
      why.detail += " ns and never scheduled onto a counter";
      why.remediation =
          "another user of the PMU holds every counter (the NMI watchdog, perf, a profiler); "
          "stop it, or set kernel.nmi_watchdog=0 to free one";
      out.emplace_back(std::move(why));
      continue;
    }
    // The raw count, exactly: it only ever increases. An event that shared the PMU
    // counted for part of the time only, and the count is not scaled up to the whole
    // (an estimate goes down when the next, exact, reading arrives, and would be
    // read as a wrap); the share of the time it was counted is the gauge below.
    out.emplace_back(Counter{reading.value});
    if (channel.hardware) {
      hardware_read = true;
      const double time_share = reading.time_enabled == 0
                                    ? 1.0
                                    : static_cast<double>(reading.time_running) /
                                          static_cast<double>(reading.time_enabled);
      share = std::min(share, time_share);
    }
  }
  if (!share_reported_) {
    return;
  }
  if (hardware_read) {
    out.emplace_back(Gauge{share});
    return;
  }
  auto& why = std::get<Unavailable>(out.emplace_back(std::in_place_type<Unavailable>));
  why.kind = topology::Availability::kUnreadable;
  why.path = std::string{kShareMetric};
  why.detail = "no hardware event could be read in this round";
  why.remediation =
      "see the reading of each hardware event in the same round, which says why it could not "
      "be read";
}

}  // namespace loadforge::telemetry
