// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_SCHEDULES_HPP
#define LOADFORGE_MAIN_SCHEDULES_HPP

// `stress --test mixed` and `stress --test cycling`: a run made of a schedule
// (workloads/mixed/schedule.hpp) and the configuration of the workers it starts
// as. The schedule is made here, before anything is started, from the request
// alone, so the run can be read before it is run and a refusal is a refusal
// before the first fork.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "core/byte_size.hpp"
#include "core/result.hpp"
#include "verification/record.hpp"
#include "workloads/mixed/cycling.hpp"
#include "workloads/mixed/schedule.hpp"
#include "workloads/workload.hpp"

namespace loadforge::cli {

/// A run made of a schedule: the workers' configuration and what they are told.
struct ScheduledRun {
  config::Config config;
  mixed::Schedule schedule;
};

/// How long a mixed run goes between rotations when it is not told: a sixth of
/// the run, never under a second and never over five minutes (the rotation is
/// a pause in every worker's verification, and a run of hours should not
/// rotate every few seconds).
[[nodiscard]] std::int64_t default_rotation_ms(std::int64_t duration_ms) noexcept;

/// How long a level of a cycle lasts when it is not told: an eighth of the run,
/// at least a second and at most a minute.
[[nodiscard]] std::int64_t default_dwell_ms(std::int64_t duration_ms) noexcept;

struct MixedRequest {
  const workloads::Registry* registry = nullptr;
  std::size_t workers = 0;
  /// The CPUs workers are placed on, ascending: worker i on cpus[i mod n], and
  /// across them, if there are two, the workers rotate.
  std::vector<std::uint32_t> cpus;
  std::uint64_t seed = 1;
  std::int64_t duration_ms = 0;
  std::optional<std::int64_t> rotation_ms;
  std::uint64_t footprint_bytes = 0;
  verification::DutyCycle duty;
};

[[nodiscard]] core::Result<ScheduledRun, std::string> plan_mixed(const MixedRequest& request);

struct CyclingRequest {
  std::string workload;
  mixed::CycleKind kind = mixed::CycleKind::kLoad;
  std::size_t workers = 0;
  std::int64_t duration_ms = 0;
  std::optional<std::int64_t> dwell_ms;
  std::optional<core::ByteSize> footprint;
};

[[nodiscard]] core::Result<ScheduledRun, std::string> plan_cycling(const CyclingRequest& request);

}  // namespace loadforge::cli

#endif
