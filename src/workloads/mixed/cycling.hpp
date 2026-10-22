// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_MIXED_CYCLING_HPP
#define LOADFORGE_WORKLOADS_MIXED_CYCLING_HPP

// Thermal and power cycling (docs/DESIGN-DRAFT.md §35): the load rises and
// falls so that package power, temperature, boost state, voltage and fan speed
// change over and over, which a steady load never makes them do. Two shapes:
//
//   load   every worker, together: idle, heavy, moderate, heavy, and again.
//          Idle is a pause (the worker beats and runs nothing), heavy is
//          continuous load, moderate is a square wave -- half of every period
//          busy.
//   cores  the number of workers that work changes: an eighth of them, half,
//          all, a quarter, all, and again.
//
// A level lasts `dwell_ns`. A command is sent only to a worker whose state it
// changes, so what a schedule says is what changes. The workers start running
// continuously, and the first level is applied at the first instant.

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/result.hpp"
#include "workloads/mixed/schedule.hpp"

namespace loadforge::mixed {

enum class CycleKind : std::uint8_t { kLoad, kCores };

[[nodiscard]] constexpr const char* describe(CycleKind kind) noexcept {
  return kind == CycleKind::kLoad ? "load" : "cores";
}

struct CyclingOptions {
  CycleKind kind = CycleKind::kLoad;
  std::size_t workers = 0;
  std::int64_t duration_ns = 0;
  std::int64_t dwell_ns = 0;
  /// The period of the moderate level's square wave.
  std::int64_t period_ns = 1'000'000'000;
};

/// The schedule, or why the options make none: no workers, a duration, dwell or
/// period that is not positive, or too many steps (kMaxSteps, mixed.hpp).
[[nodiscard]] core::Result<Schedule, std::string> make_cycling(const CyclingOptions& options);

}  // namespace loadforge::mixed

#endif
