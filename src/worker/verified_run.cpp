// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/verified_run.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/clock.hpp"
#include "platform/fpenv/environment.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "worker/entry.hpp"
#include "worker/executor.hpp"
#include "worker/executor/thread_pool.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"
#include "worker/services.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker {
namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// A tick's target is this fraction of the heartbeat interval: half.
constexpr std::int64_t kTargetDivisor = 2;
/// Never aim for less than a millisecond a slice.
constexpr std::int64_t kMinTargetNs = kNanosecondsPerMillisecond;

WorkerError error_at(std::string_view stage, std::string detail) {
  return WorkerError{std::string{stage}, std::move(detail)};
}

/// What sending a frame came to: sent, the controller gone (EPIPE says the
/// read end has no holder), or a failure that is the worker's own.
struct Sent {
  bool controller_gone = false;
  std::optional<WorkerError> failure;
};

Sent send_frame(ipc::RecordWriter& records, std::uint16_t type,
                const std::vector<std::byte>& bytes) {
  Sent sent;
  auto written = records.send(type, bytes);
  if (written.has_value()) {
    return sent;
  }
  if (written.error().cause.number == EPIPE) {
    sent.controller_gone = true;
    return sent;
  }
  sent.failure = error_at("record channel", ipc::describe(written.error()));
  return sent;
}

/// Every record of a report, then the running tally. Stops at the first frame
/// the channel will not take.
Sent send_report(ipc::RecordWriter& records, const verification::SliceReport& report,
                 const verification::Tally& total) {
  Sent sent;
  for (const verification::ErrorRecord& record : report.records) {
    sent = send_frame(records, verification::kErrorRecordFrameType, verification::encode(record));
    if (sent.controller_gone || sent.failure.has_value()) {
      return sent;
    }
  }
  return send_frame(records, kTallyFrameType, encode_tally(total));
}

}  // namespace

std::uint64_t next_slice_units(std::uint64_t last_units, std::int64_t took_ns,
                               std::int64_t target_ns) noexcept {
  const std::uint64_t doubled = std::min(last_units * 2, kMaxSliceUnits);
  if (took_ns <= 0) {
    return doubled;
  }
  const auto took = static_cast<std::uint64_t>(took_ns);
  const auto target = static_cast<std::uint64_t>(std::max<std::int64_t>(target_ns, 1));
  // last_units is at most kMaxSliceUnits (2^20) and a target is a heartbeat
  // interval in nanoseconds, so the product stays far inside 64 bits.
  const std::uint64_t wanted = last_units * target / took;
  return std::clamp<std::uint64_t>(wanted, 1, doubled);
}

core::Result<std::vector<ThreadPlacement>, std::string> placements_for(
    std::uint32_t threads, const std::vector<std::uint32_t>& cpus) {
  std::vector<ThreadPlacement> placements;
  placements.reserve(threads);
  for (std::uint32_t thread = 0; thread < threads; ++thread) {
    if (cpus.empty()) {
      placements.emplace_back(std::nullopt);
      continue;
    }
    auto mask = platform::mask_of({cpus[thread % cpus.size()]});
    if (!mask.has_value()) {
      return std::string{mask.error()};
    }
    placements.emplace_back(mask.value());
  }
  return placements;
}

core::Result<Prepared, WorkerError> prepare(platform::Syscalls& syscalls, const WorkloadArgs& args,
                                            const std::vector<std::uint32_t>& cpus,
                                            std::uint64_t first_index, const Services& services) {
  // 1. Before any thread: the threads inherit it.
  if (auto established = fpenv::establish_environment(*services.fp); !established.has_value()) {
    return error_at("floating-point environment", fpenv::describe(established.error()));
  }
  // 2. The path was chosen once, by the controller. It is used or refused.
  auto path = simd::select_path(args.isa, services.isa->supported(), simd::compiled_paths());
  if (!path.has_value()) {
    return error_at("isa path", path.error().detail);
  }
  // 3. The workload, its oracle for the plan it resolved, the executor.
  const workloads::WorkloadConfig config{args.seed, args.footprint_bytes, args.decomposition,
                                         args.isa};
  auto workload = services.workloads->create(args.workload, config);
  if (!workload.has_value()) {
    return error_at("workload", std::string{workloads::describe(workload.error().fault)} + ": " +
                                    workload.error().detail);
  }
  Prepared prepared;
  prepared.workload = std::move(workload).value();
  auto oracle = services.oracles->create(args.workload, config, prepared.workload->plan());
  if (!oracle.has_value()) {
    return error_at("oracle", std::string{verification::describe(oracle.error().fault)} + ": " +
                                  oracle.error().detail);
  }
  prepared.oracle = std::move(oracle).value();
  auto placements = placements_for(args.threads, cpus);
  if (!placements.has_value()) {
    return error_at("executor", "cpus: " + placements.error());
  }
  auto pool = make_thread_pool(placements.value(), *services.threads, syscalls, *services.fp);
  if (!pool.has_value()) {
    return error_at("executor", describe(pool.error()));
  }
  prepared.executor = std::move(pool).value();
  // 4. After everything that could have loaded a library.
  if (auto asserted = fpenv::assert_environment(*services.fp); !asserted.has_value()) {
    return error_at("floating-point environment",
                    "changed during start-up: " + fpenv::describe(asserted.error()));
  }

  verification::RunnerSetup setup;
  setup.workload = prepared.workload.get();
  setup.oracle = prepared.oracle.get();
  setup.executor = prepared.executor.get();
  setup.syscalls = &syscalls;
  setup.duty = args.duty;
  setup.line_bytes = args.line_bytes;
  setup.base.workload = args.workload;
  setup.base.workload_version = prepared.workload->descriptor().version;
  setup.base.seed = args.seed;
  setup.base.isa = args.isa;
  setup.base.physical_core = args.physical_core;
  setup.base.numa_node = args.numa_node;
  if (cpus.size() == 1) {
    setup.base.logical_cpu = cpus.front();
  }
  setup.first_index = first_index;
  auto runner = services.make_runner(setup);
  if (!runner.has_value()) {
    return error_at("runner", std::string{verification::describe(runner.error().fault)} + ": " +
                                  runner.error().detail);
  }
  prepared.runner = std::move(runner).value();
  return prepared;
}

core::Result<VerifiedOutcome, WorkerError> VerifiedRun::run() {
  const WorkloadArgs& workload = *args_->workload;
  auto prepared = prepare(*syscalls_, workload, args_->cpus, 0, *services_);
  if (!prepared.has_value()) {
    return prepared.error();
  }
  verification::SliceRunner& runner = *prepared.value().runner;

  platform::Clock clock(*syscalls_);
  auto started = clock.now();
  if (!started.has_value()) {
    return error_at("clock", describe(started.error()));
  }
  const auto pid = static_cast<std::uint32_t>(syscalls_->process_id());
  const std::size_t slot = ipc::worker_slot(session_.slot);
  const std::int64_t target_ns =
      std::max(session_.pacing.interval_ns / kTargetDivisor, kMinTargetNs);
  ControllerWatch watch(*syscalls_, session_, started.value().running_ns);
  VerifiedOutcome result;
  std::uint64_t units = 1;

  while (true) {
    auto now = clock.now();
    if (!now.has_value()) {
      return error_at("clock", describe(now.error()));
    }
    auto elapsed = platform::Clock::between(started.value(), now.value());
    if (!elapsed.has_value()) {
      return error_at("clock", describe(elapsed.error()));
    }
    const std::int64_t elapsed_ns = elapsed.value().raw_ns;
    result.outcome.elapsed_ns = elapsed_ns;
    result.outcome.progress = runner.total().units_computed;

    const ipc::Heartbeat beat{now.value().running_ns, result.outcome.progress,
                              NullWorkload::kPhaseRunning, pid};
    if (auto published = session_.region->publish(slot, beat); !published.has_value()) {
      return error_at("heartbeat", describe(published.error()));
    }
    if (elapsed_ns >= session_.pacing.duration_ns) {
      result.outcome.reason = StopReason::kDurationElapsed;
      break;
    }
    if (const auto gone = watch.gone(now.value(), elapsed_ns); gone.has_value()) {
      result.outcome.reason = *gone;
      return result;
    }
    auto looked = watch.wait(0);
    if (!looked.has_value()) {
      return looked.error();
    }
    if (looked.value().has_value()) {
      result.outcome.reason = *looked.value();
      if (controller_lost(result.outcome.reason)) {
        return result;
      }
      break;
    }

    auto slice = runner.run_slice(verification::SliceRequest{units});
    if (!slice.has_value()) {
      // kFinished is the only error a slice has: the runner ended the run
      // itself (a slice before this one said why, in a record).
      result.outcome.reason = StopReason::kRunnerStopped;
      result.note = slice.error().detail;
      break;
    }
    const Sent sent = send_report(*records_, slice.value(), runner.total());
    if (sent.failure.has_value()) {
      return *sent.failure;
    }
    if (sent.controller_gone) {
      result.outcome.reason = StopReason::kControllerHungUp;
      return result;
    }
    auto after = clock.now();
    if (!after.has_value()) {
      return error_at("clock", describe(after.error()));
    }
    units = next_slice_units(units, after.value().running_ns - now.value().running_ns, target_ns);
  }

  // Ended by time, by request, or by the runner: its last word, the records
  // that state what the whole run did not check, goes out before the worker
  // says it finished.
  auto last = runner.finish();
  if (!last.has_value()) {
    // finish() answers kFinished only to a second call, and this is the first.
    return error_at("runner", "finish: " + last.error().detail);
  }
  result.tally = runner.total();
  result.outcome.progress = result.tally.units_computed;
  const Sent sent = send_report(*records_, last.value(), result.tally);
  if (sent.failure.has_value()) {
    return *sent.failure;
  }
  if (sent.controller_gone) {
    result.outcome.reason = StopReason::kControllerHungUp;
  }
  return result;
}

}  // namespace loadforge::worker
