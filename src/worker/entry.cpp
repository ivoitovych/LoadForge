// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/entry.hpp"

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/parse_number.hpp"
#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::worker {
namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// The largest millisecond count that still fits in nanoseconds.
constexpr std::uint64_t kMaxMilliseconds = static_cast<std::uint64_t>(
    std::numeric_limits<std::int64_t>::max() / kNanosecondsPerMillisecond);

constexpr std::string_view kSlot = "--slot";
constexpr std::string_view kWorkers = "--workers";
constexpr std::string_view kHeartbeatFd = "--heartbeat-fd";
constexpr std::string_view kRecordFd = "--record-fd";
constexpr std::string_view kControlFd = "--control-fd";
constexpr std::string_view kSupervisorPid = "--supervisor-pid";
constexpr std::string_view kDurationMs = "--duration-ms";
constexpr std::string_view kIntervalMs = "--interval-ms";
constexpr std::string_view kStaleAfterMs = "--stale-after-ms";
constexpr std::string_view kCpus = "--cpus";

constexpr std::array<std::string_view, 9> kRequired{kSlot,       kWorkers,    kHeartbeatFd,
                                                    kRecordFd,   kControlFd,  kSupervisorPid,
                                                    kDurationMs, kIntervalMs, kStaleAfterMs};

/// "key: what". The second parameter is a string rather than a view so the
/// two cannot be swapped by type, and because every caller builds it anyway.
std::string refusal(std::string_view key, const std::string& what) {
  std::string text{key};
  text += ": ";
  text += what;
  return text;
}

/// A whole non-negative number and nothing else: no suffix, no sign, no space.
core::Result<std::uint64_t, std::string> number_of(std::string_view key, std::string_view text) {
  auto split = core::split_number_and_suffix(text);
  if (!split.has_value() || !split.value().suffix.empty()) {
    return refusal(key, "expected a whole non-negative number, got '" + std::string{text} + "'");
  }
  return split.value().value;
}

core::Result<std::uint64_t, std::string> number_at_most(std::string_view key, std::string_view text,
                                                        std::uint64_t limit) {
  auto number = number_of(key, text);
  if (!number.has_value()) {
    return number.error();
  }
  if (number.value() > limit) {
    return refusal(
        key, std::to_string(number.value()) + " is past the limit of " + std::to_string(limit));
  }
  return number.value();
}

core::Result<std::vector<std::uint32_t>, std::string> cpus_of(std::string_view text) {
  std::vector<std::uint32_t> cpus;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t comma = text.find(',', start);
    if (comma == std::string_view::npos) {
      comma = text.size();
    }
    // Bounded to the type here and no further: which ids a mask can name is
    // mask_of's rule, and which the machine has is the kernel's.
    auto id = number_at_most(kCpus, text.substr(start, comma - start),
                             std::numeric_limits<std::uint32_t>::max());
    if (!id.has_value()) {
      return id.error();
    }
    cpus.push_back(static_cast<std::uint32_t>(id.value()));
    start = comma + 1;
  }
  return cpus;
}

using Values = std::map<std::string_view, std::string_view>;

/// The key/value pairs, each key known and given once, every required key
/// present. The shape of the command line, before any value is read.
core::Result<Values, std::string> collect(std::span<const std::string_view> args) {
  Values values;
  for (std::size_t index = 0; index < args.size(); index += 2) {
    const std::string_view key = args[index];
    const bool known =
        key == kCpus || std::find(kRequired.begin(), kRequired.end(), key) != kRequired.end();
    if (!known) {
      return refusal(key, "not a worker argument");
    }
    if (index + 1 >= args.size()) {
      return refusal(key, "missing its value");
    }
    if (values.contains(key)) {
      return refusal(key, "given twice");
    }
    values[key] = args[index + 1];
  }
  for (const std::string_view required : kRequired) {
    if (!values.contains(required)) {
      return refusal(required, "required");
    }
  }
  return values;
}

/// The three times, each at least a millisecond and small enough to hold in
/// nanoseconds, with the staleness allowance no shorter than the interval:
/// an allowance shorter than the gap between beats would call every
/// controller stale.
core::Result<Pacing, std::string> pacing_of(Values& values) {
  Pacing pacing;
  const std::array<std::pair<std::string_view, std::int64_t*>, 3> times{
      std::pair{kDurationMs, &pacing.duration_ns}, std::pair{kIntervalMs, &pacing.interval_ns},
      std::pair{kStaleAfterMs, &pacing.stale_after_ns}};
  for (const auto& [key, target] : times) {
    auto ms = number_at_most(key, values[key], kMaxMilliseconds);
    if (!ms.has_value()) {
      return ms.error();
    }
    if (ms.value() == 0) {
      return refusal(key, "must be at least 1");
    }
    *target = static_cast<std::int64_t>(ms.value()) * kNanosecondsPerMillisecond;
  }
  if (pacing.stale_after_ns < pacing.interval_ns) {
    return refusal(kStaleAfterMs, "must be at least the interval");
  }
  return pacing;
}

}  // namespace

core::Result<WorkerArgs, std::string> parse_args(std::span<const std::string_view> args) {
  auto collected = collect(args);
  if (!collected.has_value()) {
    return collected.error();
  }
  Values values = collected.value();

  WorkerArgs parsed;
  auto workers = number_at_most(kWorkers, values[kWorkers], ipc::kMaxWorkers);
  if (!workers.has_value()) {
    return workers.error();
  }
  if (workers.value() == 0) {
    return refusal(kWorkers, "must be at least 1");
  }
  parsed.workers = workers.value();
  auto slot = number_at_most(kSlot, values[kSlot], parsed.workers - 1);
  if (!slot.has_value()) {
    return slot.error();
  }
  parsed.slot = slot.value();

  const std::array<std::pair<std::string_view, int*>, 3> descriptors{
      std::pair{kHeartbeatFd, &parsed.heartbeat_fd}, std::pair{kRecordFd, &parsed.record_fd},
      std::pair{kControlFd, &parsed.control_fd}};
  for (const auto& [key, target] : descriptors) {
    auto fd = number_at_most(key, values[key], INT_MAX);
    if (!fd.has_value()) {
      return fd.error();
    }
    *target = static_cast<int>(fd.value());
  }

  auto pid = number_at_most(kSupervisorPid, values[kSupervisorPid], INT_MAX);
  if (!pid.has_value()) {
    return pid.error();
  }
  if (pid.value() == 0) {
    return refusal(kSupervisorPid, "must be at least 1");
  }
  parsed.supervisor_pid = static_cast<pid_t>(pid.value());

  auto pacing = pacing_of(values);
  if (!pacing.has_value()) {
    return pacing.error();
  }
  parsed.pacing = pacing.value();

  if (values.contains(kCpus)) {
    auto cpus = cpus_of(values[kCpus]);
    if (!cpus.has_value()) {
      return cpus.error();
    }
    parsed.cpus = std::move(cpus).value();
  }
  return parsed;
}

std::vector<std::string> argv_for(std::string_view executable, const WorkerArgs& args) {
  // Built by appending rather than from an initializer list: two allocating
  // temporaries in one list leave a cleanup edge no test can take (§1.13).
  std::vector<std::string> argv;
  argv.emplace_back(executable);
  argv.emplace_back(kWorkerSwitch);
  const auto add = [&argv](std::string_view key, std::string value) {
    argv.emplace_back(key);
    argv.push_back(std::move(value));
  };
  add(kSlot, std::to_string(args.slot));
  add(kWorkers, std::to_string(args.workers));
  add(kHeartbeatFd, std::to_string(args.heartbeat_fd));
  add(kRecordFd, std::to_string(args.record_fd));
  add(kControlFd, std::to_string(args.control_fd));
  add(kSupervisorPid, std::to_string(args.supervisor_pid));
  add(kDurationMs, std::to_string(args.pacing.duration_ns / kNanosecondsPerMillisecond));
  add(kIntervalMs, std::to_string(args.pacing.interval_ns / kNanosecondsPerMillisecond));
  add(kStaleAfterMs, std::to_string(args.pacing.stale_after_ns / kNanosecondsPerMillisecond));
  if (!args.cpus.empty()) {
    std::string list;
    for (const std::uint32_t cpu : args.cpus) {
      if (!list.empty()) {
        list += ',';
      }
      list += std::to_string(cpu);
    }
    add(kCpus, std::move(list));
  }
  return argv;
}

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

/// Reports a failure on stderr and, where the channel is up, on the record
/// pipe. A send that fails here is not reported again: the first failure is
/// the one the person needs.
int give_up(std::ostream& err, std::optional<ipc::RecordWriter>& records, const std::string& what,
            WorkerExit code) {
  err << "loadforge --worker: " << what << "\n";
  if (records.has_value()) {
    (void)records->send(ipc::RecordType::kError, bytes_of(what));
  }
  return static_cast<int>(code);
}

}  // namespace

int run(platform::Syscalls& syscalls, std::span<const std::string_view> args, std::ostream& err) {
  std::optional<ipc::RecordWriter> records;
  auto parsed = parse_args(args);
  if (!parsed.has_value()) {
    return give_up(err, records, parsed.error(), WorkerExit::kBadArguments);
  }
  const WorkerArgs& worker = parsed.value();

  // A write to a controller that has gone must be a reported EPIPE, not a
  // death by SIGPIPE that the supervisor cannot tell from a fault.
  if (auto ignored = syscalls.ignore_signal(SIGPIPE); !ignored.has_value()) {
    return give_up(err, records, describe(ignored.error()), WorkerExit::kSetupFailed);
  }
  auto attached = ipc::HeartbeatRegion::attach(syscalls, worker.heartbeat_fd, worker.workers);
  if (!attached.has_value()) {
    return give_up(err, records, "heartbeat region: " + describe(attached.error()),
                   WorkerExit::kSetupFailed);
  }
  ipc::HeartbeatRegion region = std::move(attached).value();

  records.emplace(syscalls, worker.record_fd);
  const std::string started =
      "slot " + std::to_string(worker.slot) + " pid " + std::to_string(syscalls.process_id());
  if (auto sent = records->send(ipc::RecordType::kStarted, bytes_of(started)); !sent.has_value()) {
    // The channel itself is down, so the error record would not arrive either.
    records.reset();
    return give_up(err, records, "record channel: " + describe(sent.error()),
                   WorkerExit::kSetupFailed);
  }

  if (!worker.cpus.empty()) {
    auto mask = platform::mask_of(worker.cpus);
    if (!mask.has_value()) {
      return give_up(err, records, "cpus: " + std::string{mask.error()}, WorkerExit::kSetupFailed);
    }
    platform::Affinity affinity(syscalls);
    if (auto placed = affinity.place(mask.value()); !placed.has_value()) {
      return give_up(err, records, "cpus: " + describe(placed.error()), WorkerExit::kSetupFailed);
    }
  }

  NullWorkload workload(syscalls, Session{&region, worker.slot, worker.control_fd, worker.pacing});
  auto outcome = workload.run();
  if (!outcome.has_value()) {
    return give_up(err, records, describe(outcome.error()), WorkerExit::kSetupFailed);
  }
  const Outcome& result = outcome.value();
  const std::string summary =
      std::string{describe(result.reason)} + " after " + std::to_string(result.progress) +
      " tick(s), " + std::to_string(result.elapsed_ns / kNanosecondsPerMillisecond) + " ms";
  if (controller_lost(result.reason)) {
    // Reported on the pipe as well, in case something is still reading it;
    // a failure to send is expected and not reported twice.
    return give_up(err, records, "controller lost: " + summary, WorkerExit::kControllerLost);
  }
  if (auto sent = records->send(ipc::RecordType::kFinished, bytes_of(summary)); !sent.has_value()) {
    records.reset();
    return give_up(err, records, "record channel: " + describe(sent.error()),
                   WorkerExit::kSetupFailed);
  }
  return static_cast<int>(WorkerExit::kCompleted);
}

}  // namespace loadforge::worker
