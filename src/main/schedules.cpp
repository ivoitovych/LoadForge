// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/schedules.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "controller/plan.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/result.hpp"
#include "workloads/mixed/cycling.hpp"
#include "workloads/mixed/mixed.hpp"
#include "workloads/mixed/schedule.hpp"

namespace loadforge::cli {
namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;

/// The most a duration of milliseconds may be for its nanoseconds to be
/// counted: an eighth of what a signed 64-bit count holds, as the command line
/// bounds a run.
constexpr std::int64_t kLongestMilliseconds = INT64_MAX / kNanosecondsPerMillisecond / 8;

}  // namespace

std::int64_t default_rotation_ms(std::int64_t duration_ms) noexcept {
  return std::clamp<std::int64_t>(duration_ms / 6, 1'000, 300'000);
}

std::int64_t default_dwell_ms(std::int64_t duration_ms) noexcept {
  return std::clamp<std::int64_t>(duration_ms / 8, 1'000, 60'000);
}

core::Result<ScheduledRun, std::string> plan_mixed(const MixedRequest& request) {
  if (request.duration_ms <= 0 || request.duration_ms > kLongestMilliseconds) {
    return std::string{"the duration is not one a schedule can be made for"};
  }
  const std::vector<mixed::RosterEntry> roster =
      mixed::roster_of(*request.registry, request.footprint_bytes);
  mixed::MixedOptions options;
  options.workers = request.workers;
  options.seed = request.seed;
  options.duration_ns = request.duration_ms * kNanosecondsPerMillisecond;
  options.rotation_ns = request.rotation_ms.value_or(default_rotation_ms(request.duration_ms)) *
                        kNanosecondsPerMillisecond;
  options.footprint_bytes = request.footprint_bytes;
  options.duty = request.duty;
  // The workers are placed one to a CPU (controller::assign_cpus, as `stress
  // --test` places them); rotating across CPUs is each worker taking another's.
  if (request.workers >= 2 && request.cpus.size() >= 2) {
    for (std::size_t slot = 0; slot < request.workers; ++slot) {
      options.placements.push_back(controller::assign_cpus(slot, 1, request.cpus));
    }
  }
  auto made = mixed::make_mixed(roster, options);
  if (!made.has_value()) {
    return made.error();
  }
  ScheduledRun run;
  run.schedule = std::move(made).value();
  run.config.run.mode = config::Mode::kStress;
  run.config.run.duration = core::Duration{request.duration_ms};
  for (const mixed::Start& start : run.schedule.start) {
    run.config.workloads.emplace_back(start.workload, config::Threads::count(1), true,
                                      core::ByteSize::bytes(start.footprint_bytes));
  }
  return run;
}

core::Result<ScheduledRun, std::string> plan_cycling(const CyclingRequest& request) {
  if (request.duration_ms <= 0 || request.duration_ms > kLongestMilliseconds) {
    return std::string{"the duration is not one a schedule can be made for"};
  }
  mixed::CyclingOptions options;
  options.kind = request.kind;
  options.workers = request.workers;
  options.duration_ns = request.duration_ms * kNanosecondsPerMillisecond;
  options.dwell_ns =
      request.dwell_ms.value_or(default_dwell_ms(request.duration_ms)) * kNanosecondsPerMillisecond;
  auto made = mixed::make_cycling(options);
  if (!made.has_value()) {
    return made.error();
  }
  ScheduledRun run;
  run.schedule = std::move(made).value();
  run.config.run.mode = config::Mode::kStress;
  run.config.run.duration = core::Duration{request.duration_ms};
  for (std::size_t worker = 0; worker < request.workers; ++worker) {
    run.config.workloads.emplace_back(request.workload, config::Threads::count(1), true,
                                      request.footprint);
  }
  return run;
}

}  // namespace loadforge::cli
