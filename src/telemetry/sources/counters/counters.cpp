// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/counters/counters.hpp"

#include <linux/perf_event.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

// GCC's -Wpedantic refuses the type unless it is spelled as an extension.
__extension__ using Wide = unsigned __int128;

struct EventSpec {
  std::string_view name;
  std::uint32_t type;
  std::uint64_t config;
  std::string_view provider;
};

constexpr std::array<EventSpec, 5> kEvents{{
    {"counters.cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "perf:hardware"},
    {"counters.instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "perf:hardware"},
    {"counters.cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, "perf:hardware"},
    {"counters.branch_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, "perf:hardware"},
    {"counters.context_switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES,
     "perf:software"},
}};

Metric make_metric(const EventSpec& event) {
  Metric metric;
  metric.name = std::string{event.name};
  metric.unit = Unit::kCount;
  metric.kind = Kind::kCounter;
  metric.scope = Scope::kMachine;
  metric.provider = std::string{event.provider};
  return metric;
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
                             std::string_view subject) {
  std::string text;
  switch (kind) {
    case topology::Availability::kDenied:
      text = "grant LoadForge the CAP_PERFMON capability, or lower kernel.perf_event_paranoid";
      if (!paranoid.has_value()) {
        return text;
      }
      if (*paranoid > 2) {
        text += " (it is ";
        text += std::to_string(*paranoid);
        text += ", and above 2 every unprivileged use is refused; 2 or below allows counting "
                "your own processes in user space)";
        return text;
      }
      text = "kernel.perf_event_paranoid is ";
      text += std::to_string(*paranoid);
      text += ", which already allows counting your own processes in user space, so "
              "something else refuses perf_event_open (a container's seccomp profile or a "
              "security module); allow the call there, or grant LoadForge CAP_PERFMON";
      return text;
    case topology::Availability::kAbsent:
      text = "this machine does not offer ";
      text += subject;
      text += " (a virtual machine without a PMU, a CPU without the event, or a kernel built "
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
  topology::Source level_file{*filesystem_, paranoid_path_};
  auto level = level_file.integer();
  if (level.has_value()) {
    paranoid_ = level.value();
  }

  std::vector<MetricStatus> statuses;
  const pid_t pid = syscalls_->process_id();
  for (const EventSpec& event : kEvents) {
    platform::PerfEventSpec spec;
    spec.type = event.type;
    spec.config = event.config;
    spec.pid = pid;
    spec.user_only = true;
    spec.inherit = true;

    // One failure's report: kind chosen by `kind`, the error's own words, and
    // the remedy for the provider that failed. Built a step at a time (journal
    // §1.13).
    const auto failure = [&](topology::Availability kind, const platform::SyscallError& error) {
      std::string detail = platform::describe(error);
      if (paranoid_.has_value()) {
        detail += " (kernel.perf_event_paranoid is ";
        detail += std::to_string(*paranoid_);
        detail += ")";
      }
      std::string remediation = perf_remediation(kind, paranoid_, error.subject);
      return MetricStatus{make_metric(event),
                          Unavailable{kind, error.subject, std::move(detail),
                                      std::move(remediation)}};
    };

    auto opened = syscalls_->open_perf_event(spec);
    if (!opened.has_value()) {
      statuses.push_back(failure(classify_perf_open(opened.error().number), opened.error()));
      continue;
    }
    const int fd = opened.value();
    // Reset then enable, so every count starts from zero at its own enabling.
    std::optional<MetricStatus> refused;
    for (const platform::PerfAction action :
         {platform::PerfAction::kReset, platform::PerfAction::kEnable}) {
      auto done = syscalls_->control_perf_event(fd, action);
      if (!done.has_value()) {
        refused = failure(topology::Availability::kUnreadable, done.error());
        break;
      }
    }
    if (refused.has_value()) {
      static_cast<void>(syscalls_->close(fd));
      statuses.push_back(std::move(*refused));
      continue;
    }
    std::string subject = "perf event type ";
    subject += std::to_string(event.type);
    subject += " config ";
    subject += std::to_string(event.config);
    channels_.push_back(Channel{fd, std::move(subject)});
    statuses.push_back(MetricStatus{make_metric(event), std::nullopt});
  }
  return statuses;
}

void CountersSource::sample(std::vector<Reading>& out) {
  for (const Channel& channel : channels_) {
    auto count = syscalls_->read_perf_count(channel.fd);
    if (!count.has_value()) {
      const platform::SyscallError& error = count.error();
      const topology::Availability kind = topology::classify(error, true);
      std::string detail = platform::describe(error);
      std::string remediation = perf_remediation(kind, paranoid_, channel.subject);
      out.emplace_back(Unavailable{kind, channel.subject, std::move(detail),
                                   std::move(remediation)});
      continue;
    }
    const platform::PerfCount& reading = count.value();
    if (reading.time_running == 0 && reading.time_enabled > 0) {
      std::string detail = "enabled for ";
      detail += std::to_string(reading.time_enabled);
      detail += " ns and never scheduled onto a counter";
      std::string remediation =
          "another user of the PMU holds every counter (the NMI watchdog, perf, a profiler); "
          "stop it, or set kernel.nmi_watchdog=0 to free one";
      out.emplace_back(Unavailable{topology::Availability::kUnreadable, channel.subject,
                                   std::move(detail), std::move(remediation)});
      continue;
    }
    std::uint64_t value = reading.value;
    if (reading.time_running < reading.time_enabled) {
      // The event shared the PMU: scale to the time it was enabled. Widened,
      // because the product of two 64-bit values does not fit one.
      Wide scaled = reading.value;
      scaled *= reading.time_enabled;
      scaled /= reading.time_running;
      value = static_cast<std::uint64_t>(
          std::min(scaled, static_cast<Wide>(std::numeric_limits<std::uint64_t>::max())));
    }
    out.emplace_back(Counter{value});
  }
}

}  // namespace loadforge::telemetry
