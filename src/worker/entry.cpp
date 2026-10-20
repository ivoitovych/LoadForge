// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/entry.hpp"

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
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
#include <tuple>
#include <utility>
#include <vector>

#include "core/parse_number.hpp"
#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "verification/record/render.hpp"
#include "verification/runner.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"
#include "worker/services.hpp"
#include "worker/verified_run.hpp"
#include "workloads/workload.hpp"

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

constexpr std::string_view kWorkload = "--workload";
constexpr std::string_view kSeed = "--seed";
constexpr std::string_view kFootprintBytes = "--footprint-bytes";
constexpr std::string_view kThreads = "--threads";
constexpr std::string_view kDecomposition = "--decomposition";
constexpr std::string_view kIsa = "--isa";
constexpr std::string_view kOraclePeriod = "--oracle-period";
constexpr std::string_view kIntegrityPeriod = "--integrity-period";
constexpr std::string_view kLineBytes = "--line-bytes";
constexpr std::string_view kPhysicalCore = "--physical-core";
constexpr std::string_view kNumaNode = "--numa-node";
constexpr std::string_view kVersion = "--version";
constexpr std::string_view kIteration = "--iteration";
constexpr std::string_view kCpu = "--cpu";

constexpr std::array<std::string_view, 9> kRequired{kSlot,       kWorkers,    kHeartbeatFd,
                                                    kRecordFd,   kControlFd,  kSupervisorPid,
                                                    kDurationMs, kIntervalMs, kStaleAfterMs};

/// The keys that describe the work, required together once --workload is
/// given and refused without it.
constexpr std::array<std::string_view, 8> kWorkloadRequired{
    kSeed, kFootprintBytes, kThreads,         kDecomposition,
    kIsa,  kOraclePeriod,   kIntegrityPeriod, kLineBytes};
constexpr std::array<std::string_view, 2> kWorkloadOptional{kPhysicalCore, kNumaNode};

/// Every key a worker accepts beyond the required nine: --cpus, --workload and
/// the keys that only mean something with it.
constexpr std::array<std::string_view, 12> kWorkerOptional{
    kCpus,         kWorkload,        kSeed,      kFootprintBytes, kThreads, kDecomposition, kIsa,
    kOraclePeriod, kIntegrityPeriod, kLineBytes, kPhysicalCore,   kNumaNode};

/// A re-run's required keys: the unit and where it runs. There is no duty
/// (a re-run checks everything) and no placement facts beyond the CPU.
constexpr std::array<std::string_view, 10> kRerunRequired{
    kWorkload, kSeed,      kFootprintBytes, kThreads,   kDecomposition,
    kIsa,      kLineBytes, kVersion,        kIteration, kCpu};

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
core::Result<Values, std::string> collect(std::span<const std::string_view> args,
                                          std::span<const std::string_view> required,
                                          std::span<const std::string_view> optional) {
  Values values;
  for (std::size_t index = 0; index < args.size(); index += 2) {
    const std::string_view key = args[index];
    const bool known = std::find(required.begin(), required.end(), key) != required.end() ||
                       std::find(optional.begin(), optional.end(), key) != optional.end();
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
  for (const std::string_view key : required) {
    if (!values.contains(key)) {
      return refusal(key, "required");
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

constexpr std::uint64_t kMaxLineBytes = 4096;
constexpr std::uint64_t kMinLineBytes = 8;

/// The keys that describe a unit of work, read into WorkloadArgs. `with_duty`
/// says whether the duty cycle's two periods are among them (a worker's) or
/// not (a re-run's, which checks every unit). The values are the controller's,
/// written from the same structure, so a refusal is a bug report and names the
/// key.
core::Result<WorkloadArgs, std::string> workload_of(Values& values, bool with_duty) {
  WorkloadArgs args;
  args.workload = std::string{values[kWorkload]};
  if (!workloads::is_valid_name(args.workload)) {
    return refusal(kWorkload, "'" + args.workload + "' is not a workload name");
  }
  auto seed = number_of(kSeed, values[kSeed]);
  if (!seed.has_value()) {
    return seed.error();
  }
  args.seed = seed.value();
  auto footprint = number_of(kFootprintBytes, values[kFootprintBytes]);
  if (!footprint.has_value()) {
    return footprint.error();
  }
  if (footprint.value() == 0) {
    return refusal(kFootprintBytes, "must be at least 1");
  }
  args.footprint_bytes = footprint.value();
  auto threads = number_at_most(kThreads, values[kThreads], ipc::kMaxWorkers);
  if (!threads.has_value()) {
    return threads.error();
  }
  if (threads.value() == 0) {
    return refusal(kThreads, "must be at least 1");
  }
  args.threads = static_cast<std::uint32_t>(threads.value());
  auto decomposition = number_at_most(kDecomposition, values[kDecomposition],
                                      std::numeric_limits<std::uint32_t>::max());
  if (!decomposition.has_value()) {
    return decomposition.error();
  }
  args.decomposition = static_cast<std::uint32_t>(decomposition.value());
  const std::optional<simd::IsaPath> path = simd::parse_isa_path(values[kIsa]);
  if (!path.has_value()) {
    return refusal(kIsa,
                   "'" + std::string{values[kIsa]} + "' is none of scalar, avx2, avx512, neon");
  }
  args.isa = *path;
  auto line = number_at_most(kLineBytes, values[kLineBytes], kMaxLineBytes);
  if (!line.has_value()) {
    return line.error();
  }
  if (line.value() < kMinLineBytes || !std::has_single_bit(line.value())) {
    return refusal(kLineBytes, "must be a power of two of at least 8");
  }
  args.line_bytes = static_cast<std::uint32_t>(line.value());
  if (with_duty) {
    const std::array<std::pair<std::string_view, std::uint32_t*>, 2> periods{
        std::pair{kOraclePeriod, &args.duty.oracle_period},
        std::pair{kIntegrityPeriod, &args.duty.integrity_period}};
    for (const auto& [key, target] : periods) {
      auto period = number_at_most(key, values[key], std::numeric_limits<std::uint32_t>::max());
      if (!period.has_value()) {
        return period.error();
      }
      *target = static_cast<std::uint32_t>(period.value());
    }
  }
  const std::array<std::pair<std::string_view, std::optional<std::uint32_t>*>, 2> where{
      std::pair{kPhysicalCore, &args.physical_core}, std::pair{kNumaNode, &args.numa_node}};
  for (const auto& [key, target] : where) {
    if (!values.contains(key)) {
      continue;
    }
    auto id = number_at_most(key, values[key], std::numeric_limits<std::uint32_t>::max());
    if (!id.has_value()) {
      return id.error();
    }
    *target = static_cast<std::uint32_t>(id.value());
  }
  return args;
}

}  // namespace

core::Result<WorkerArgs, std::string> parse_args(std::span<const std::string_view> args) {
  auto collected = collect(args, kRequired, kWorkerOptional);
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

  // The work: all of its keys with --workload, none without it.
  if (!values.contains(kWorkload)) {
    for (const std::string_view key : kWorkloadRequired) {
      if (values.contains(key)) {
        return refusal(key, "needs --workload");
      }
    }
    for (const std::string_view key : kWorkloadOptional) {
      if (values.contains(key)) {
        return refusal(key, "needs --workload");
      }
    }
    return parsed;
  }
  for (const std::string_view key : kWorkloadRequired) {
    if (!values.contains(key)) {
      return refusal(key, "required with --workload");
    }
  }
  auto workload = workload_of(values, true);
  if (!workload.has_value()) {
    return workload.error();
  }
  parsed.workload = std::move(workload).value();
  return parsed;
}

core::Result<RerunArgs, std::string> parse_rerun_args(std::span<const std::string_view> args) {
  auto collected = collect(args, kRerunRequired, {});
  if (!collected.has_value()) {
    return collected.error();
  }
  Values values = collected.value();
  RerunArgs parsed;
  auto unit = workload_of(values, false);
  if (!unit.has_value()) {
    return unit.error();
  }
  parsed.unit = std::move(unit).value();
  auto version =
      number_at_most(kVersion, values[kVersion], std::numeric_limits<std::uint32_t>::max());
  if (!version.has_value()) {
    return version.error();
  }
  parsed.workload_version = static_cast<std::uint32_t>(version.value());
  auto iteration = number_of(kIteration, values[kIteration]);
  if (!iteration.has_value()) {
    return iteration.error();
  }
  parsed.iteration = iteration.value();
  auto cpu = number_at_most(kCpu, values[kCpu], std::numeric_limits<std::uint32_t>::max());
  if (!cpu.has_value()) {
    return cpu.error();
  }
  parsed.cpu = static_cast<std::uint32_t>(cpu.value());
  return parsed;
}

namespace {

/// Appends a unit of work's keys. The inverse of workload_of, and a test says
/// so for every field (the config module's round-trip rule).
void add_workload(std::vector<std::string>& argv, const WorkloadArgs& args, bool with_duty) {
  const auto add = [&argv](std::string_view key, std::string value) {
    argv.emplace_back(key);
    argv.push_back(std::move(value));
  };
  add(kWorkload, args.workload);
  add(kSeed, std::to_string(args.seed));
  add(kFootprintBytes, std::to_string(args.footprint_bytes));
  add(kThreads, std::to_string(args.threads));
  add(kDecomposition, std::to_string(args.decomposition));
  add(kIsa, std::string{simd::describe(args.isa)});
  add(kLineBytes, std::to_string(args.line_bytes));
  if (with_duty) {
    add(kOraclePeriod, std::to_string(args.duty.oracle_period));
    add(kIntegrityPeriod, std::to_string(args.duty.integrity_period));
  }
  if (args.physical_core.has_value()) {
    add(kPhysicalCore, std::to_string(*args.physical_core));
  }
  if (args.numa_node.has_value()) {
    add(kNumaNode, std::to_string(*args.numa_node));
  }
}

}  // namespace

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
  if (args.workload.has_value()) {
    add_workload(argv, *args.workload, true);
  }
  return argv;
}

std::vector<std::string> rerun_argv_for(std::string_view executable, const RerunArgs& args) {
  std::vector<std::string> argv;
  argv.emplace_back(executable);
  argv.emplace_back(kRerunSwitch);
  add_workload(argv, args.unit, false);
  const auto add = [&argv](std::string_view key, std::string value) {
    argv.emplace_back(key);
    argv.push_back(std::move(value));
  };
  add(kVersion, std::to_string(args.workload_version));
  add(kIteration, std::to_string(args.iteration));
  add(kCpu, std::to_string(args.cpu));
  return argv;
}

bool operator==(const WorkloadArgs& lhs, const WorkloadArgs& rhs) noexcept {
  return std::tie(lhs.workload, lhs.seed, lhs.footprint_bytes, lhs.threads, lhs.decomposition,
                  lhs.isa, lhs.duty, lhs.line_bytes, lhs.physical_core, lhs.numa_node) ==
         std::tie(rhs.workload, rhs.seed, rhs.footprint_bytes, rhs.threads, rhs.decomposition,
                  rhs.isa, rhs.duty, rhs.line_bytes, rhs.physical_core, rhs.numa_node);
}

bool operator==(const WorkerArgs& lhs, const WorkerArgs& rhs) noexcept {
  return std::tie(lhs.slot, lhs.workers, lhs.heartbeat_fd, lhs.record_fd, lhs.control_fd,
                  lhs.supervisor_pid, lhs.pacing, lhs.cpus, lhs.workload) ==
         std::tie(rhs.slot, rhs.workers, rhs.heartbeat_fd, rhs.record_fd, rhs.control_fd,
                  rhs.supervisor_pid, rhs.pacing, rhs.cpus, rhs.workload);
}

bool operator==(const RerunArgs& lhs, const RerunArgs& rhs) noexcept {
  return std::tie(lhs.unit, lhs.workload_version, lhs.iteration, lhs.cpu) ==
         std::tie(rhs.unit, rhs.workload_version, rhs.iteration, rhs.cpu);
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

/// How a worker leaves when its record channel will not take a frame. EPIPE
/// says who is at fault: the read end has no holder left, so the controller
/// is gone, which is a controller lost as the exit-code table describes a
/// hang-up -- the controller-killed test met it when the controller died
/// before a worker's started record. Any other refusal is the worker's own
/// failure to report. A frame the writer would not encode carries no errno
/// (SyscallError's number is 0) and is the worker's failure too.
WorkerExit exit_for_unsent(const ipc::ChannelError& error) noexcept {
  return error.cause.number == EPIPE ? WorkerExit::kControllerLost : WorkerExit::kSetupFailed;
}

}  // namespace

namespace {

/// The sentence a verifying worker's finished record ends with: what it did,
/// in the words the contracts use (a unit is "checked" by the oracle over some
/// sampled blocks; nothing is called correct).
std::string summarise(const verification::Tally& tally) {
  std::string text = std::to_string(tally.units_computed) + " unit(s) computed; oracle checked " +
                     std::to_string(tally.oracle_checked) + " unit(s) over " +
                     std::to_string(tally.oracle_blocks) + " sampled block(s), " +
                     std::to_string(tally.oracle_failed) + " wrong, " +
                     std::to_string(tally.oracle_not_performed) +
                     " not performed; bracket checked " + std::to_string(tally.integrity_checked) +
                     " unit(s), " + std::to_string(tally.integrity_failed) + " failed";
  return text;
}

}  // namespace

int run(platform::Syscalls& syscalls, std::span<const std::string_view> args, std::ostream& err) {
  const ProductionServices production;
  return run(syscalls, args, err, production.services());
}

int run(platform::Syscalls& syscalls, std::span<const std::string_view> args, std::ostream& err,
        const Services& services) {
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
                   exit_for_unsent(sent.error()));
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

  const Session session{&region, worker.slot, worker.control_fd, worker.pacing};
  Outcome result;
  std::string detail;
  if (worker.workload.has_value()) {
    VerifiedRun verified(syscalls, *records, session, worker, services);
    auto done = verified.run();
    if (!done.has_value()) {
      return give_up(err, records, describe(done.error()), WorkerExit::kSetupFailed);
    }
    result = done.value().outcome;
    detail = "; " + summarise(done.value().tally);
    if (!done.value().note.empty()) {
      detail += "; runner: " + done.value().note;
    }
  } else {
    NullWorkload workload(syscalls, session);
    auto outcome = workload.run();
    if (!outcome.has_value()) {
      return give_up(err, records, describe(outcome.error()), WorkerExit::kSetupFailed);
    }
    result = outcome.value();
  }
  const std::string summary =
      std::string{describe(result.reason)} + " after " + std::to_string(result.progress) +
      (worker.workload.has_value() ? " unit(s), " : " tick(s), ") +
      std::to_string(result.elapsed_ns / kNanosecondsPerMillisecond) + " ms" + detail;
  if (controller_lost(result.reason)) {
    // Reported on the pipe as well, in case something is still reading it;
    // a failure to send is expected and not reported twice.
    return give_up(err, records, "controller lost: " + summary, WorkerExit::kControllerLost);
  }
  if (auto sent = records->send(ipc::RecordType::kFinished, bytes_of(summary)); !sent.has_value()) {
    records.reset();
    return give_up(err, records, "record channel: " + describe(sent.error()),
                   exit_for_unsent(sent.error()));
  }
  return static_cast<int>(WorkerExit::kCompleted);
}

namespace {

/// Whether a record states a wrong result.
bool any_wrong(const std::vector<verification::ErrorRecord>& records) {
  return std::ranges::any_of(records, [](const verification::ErrorRecord& record) {
    return verification::is_wrong_result(record);
  });
}

}  // namespace

int run_rerun(platform::Syscalls& syscalls, std::span<const std::string_view> args,
              std::ostream& err, const Services& services) {
  auto parsed = parse_rerun_args(args);
  if (!parsed.has_value()) {
    err << "loadforge --rerun: " << parsed.error() << "\n";
    return static_cast<int>(RerunExit::kBadArguments);
  }
  const RerunArgs& unit = parsed.value();
  const std::vector<std::uint32_t> cpus{unit.cpu};
  // Placed first and by the same rule as a worker: the unit is run where it
  // was asked to be run or not at all.
  auto mask = platform::mask_of(cpus);
  if (!mask.has_value()) {
    err << "loadforge --rerun: cpu: " << mask.error() << "\n";
    return static_cast<int>(RerunExit::kNotPerformed);
  }
  platform::Affinity affinity(syscalls);
  if (auto placed = affinity.place(mask.value()); !placed.has_value()) {
    err << "loadforge --rerun: cpu: " << describe(placed.error()) << "\n";
    return static_cast<int>(RerunExit::kNotPerformed);
  }
  auto prepared = prepare(syscalls, unit.unit, cpus, unit.iteration, services);
  if (!prepared.has_value()) {
    err << "loadforge --rerun: " << describe(prepared.error()) << "\n";
    return static_cast<int>(RerunExit::kNotPerformed);
  }
  // Bits that differ by version are not the bits the finding is about.
  const std::uint32_t version = prepared.value().workload->descriptor().version;
  if (version != unit.workload_version) {
    err << "loadforge --rerun: the finding is of version " << unit.workload_version << " of "
        << unit.unit.workload << ", this build has version " << version << "\n";
    return static_cast<int>(RerunExit::kNotPerformed);
  }
  verification::SliceRunner& runner = *prepared.value().runner;
  auto slice = runner.run_slice(verification::SliceRequest{1});
  if (!slice.has_value()) {
    err << "loadforge --rerun: " << slice.error().detail << "\n";
    return static_cast<int>(RerunExit::kNotPerformed);
  }
  auto last = runner.finish();
  if (!last.has_value()) {
    err << "loadforge --rerun: " << last.error().detail << "\n";
    return static_cast<int>(RerunExit::kNotPerformed);
  }
  std::vector<verification::ErrorRecord> records = slice.value().records;
  records.insert(records.end(), last.value().records.begin(), last.value().records.end());
  for (const verification::ErrorRecord& record : records) {
    err << verification::render(record);
  }
  if (any_wrong(records)) {
    return static_cast<int>(RerunExit::kReproduced);
  }
  return static_cast<int>(records.empty() ? RerunExit::kClean : RerunExit::kNotPerformed);
}

}  // namespace loadforge::worker
