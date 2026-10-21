// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/mixed/mixed.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "worker/control.hpp"
#include "workloads/mixed/schedule.hpp"

namespace loadforge::mixed {
namespace {

/// A 64-bit mixing function (the finaliser of SplitMix64, Steele, Lea and
/// Flood 2014): a bijection with good avalanche, in unsigned arithmetic, with
/// no state and nothing a standard library may change.
std::uint64_t mix(std::uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

/// What a choice is for, so that two choices about one worker in one rotation
/// are independent.
enum class Purpose : std::uint8_t { kPick = 1, kSeed = 2, kSize = 3 };

std::uint64_t draw(std::uint64_t seed, std::uint64_t rotation, std::uint64_t slot,
                   Purpose purpose) {
  return mix(seed + mix(rotation + mix(slot + mix(static_cast<std::uint64_t>(purpose)))));
}

enum class Kind : std::uint8_t { kCompute, kMemory, kBalanced, kMixed, kJoin, kMove };

std::string_view name_of(Kind kind) {
  switch (kind) {
    case Kind::kCompute:
      return "compute-heavy";
    case Kind::kMemory:
      return "memory-heavy";
    case Kind::kBalanced:
      return "balanced";
    case Kind::kMixed:
      return "mixed";
    case Kind::kJoin:
      return "all-join";
    case Kind::kMove:
      return "move";
  }
  return "unrecognised kind";
}

/// The order phases come in; a move replaces every third.
constexpr std::array<Kind, 5> kPlan{Kind::kMixed, Kind::kCompute, Kind::kMemory, Kind::kBalanced,
                                    Kind::kJoin};
constexpr std::uint64_t kMoveEvery = 3;

using Pool = std::vector<const RosterEntry*>;

/// The roster's entries of one class, or the whole roster when it has none.
Pool pool_of(const Pool& usable, Intensity wanted) {
  Pool pool;
  for (const RosterEntry* entry : usable) {
    if (classify(entry->name) == wanted) {
      pool.push_back(entry);
    }
  }
  return pool.empty() ? usable : pool;
}

/// The workload `slot` runs in rotation `rotation` of a phase of `kind`.
const RosterEntry& pick(Kind kind, const Pool& usable, std::uint64_t seed, std::uint64_t rotation,
                        std::size_t slot) {
  Pool pool = usable;
  if (kind == Kind::kCompute) {
    pool = pool_of(usable, Intensity::kCompute);
  } else if (kind == Kind::kMemory) {
    pool = pool_of(usable, Intensity::kMemory);
  } else if (kind == Kind::kBalanced) {
    pool = pool_of(usable, slot % 2 == 0 ? Intensity::kCompute : Intensity::kMemory);
  }
  // Consecutive workers take consecutive entries from a seeded start, so a mix
  // gives neighbours different workloads; a join takes the start for all.
  const std::uint64_t spread = kind == Kind::kJoin ? 0 : slot;
  return *pool[(draw(seed, rotation, 0, Purpose::kPick) + spread) % pool.size()];
}

worker::Switch assignment(const RosterEntry& entry, const MixedOptions& options,
                          std::uint64_t rotation, std::size_t slot) {
  worker::Switch change;
  change.workload = entry.name;
  change.seed = draw(options.seed, rotation, slot, Purpose::kSeed);
  // Datasets change size: the whole footprint or half of it, never below what
  // the workload accepts.
  const bool half = (draw(options.seed, rotation, slot, Purpose::kSize) & 1U) != 0U;
  const std::uint64_t size = half ? options.footprint_bytes / 2 : options.footprint_bytes;
  change.footprint_bytes = size < entry.min_footprint_bytes ? options.footprint_bytes : size;
  change.decomposition = 0;
  change.duty = options.duty;
  return change;
}

/// The distinct names, sorted, joined by '+', in a label.
std::string label_of(const std::vector<worker::Switch>& changes) {
  std::vector<std::string> names;
  for (const worker::Switch& change : changes) {
    names.push_back(change.workload);
  }
  std::ranges::sort(names);
  names.erase(std::ranges::unique(names).begin(), names.end());
  std::string label;
  for (const std::string& name : names) {
    label += label.empty() ? "" : "+";
    label += name;
  }
  return bounded(std::move(label));
}

std::string phase_of(std::uint64_t rotation, Kind kind) {
  std::string label = "r";
  label += std::to_string(rotation);
  label += ' ';
  label += name_of(kind);
  return bounded(std::move(label));
}

}  // namespace

Intensity classify(const std::string& workload) {
  // The usual reading of each algorithm's arithmetic intensity: what its time
  // goes to. A hypothesis for hardware validation to test, not a measurement.
  constexpr std::array<std::string_view, 5> kCompute{"dense_linear", "fft", "monte_carlo",
                                                     "particles", "raytrace"};
  constexpr std::array<std::string_view, 5> kMemory{"adaptive_mesh", "graph", "sort", "sparse",
                                                    "stencil"};
  if (std::ranges::find(kCompute, workload) != kCompute.end()) {
    return Intensity::kCompute;
  }
  if (std::ranges::find(kMemory, workload) != kMemory.end()) {
    return Intensity::kMemory;
  }
  return Intensity::kBalanced;
}

core::Result<Schedule, std::string> make_mixed(const std::vector<RosterEntry>& roster,
                                               const MixedOptions& options) {
  if (options.workers == 0) {
    return std::string{"a mixed schedule needs at least one worker"};
  }
  if (options.duration_ns <= 0 || options.rotation_ns <= 0) {
    return std::string{"the duration and the rotation interval must be positive"};
  }
  if (options.footprint_bytes == 0) {
    return std::string{"a mixed schedule needs a footprint"};
  }
  const auto rotations = static_cast<std::uint64_t>((options.duration_ns - 1) / options.rotation_ns);
  if (rotations > kMaxSteps) {
    return "a rotation every " + std::to_string(options.rotation_ns / 1'000'000) + " ms for " +
           std::to_string(options.duration_ns / 1'000'000) + " ms is more than " +
           std::to_string(kMaxSteps) + " steps";
  }
  if (options.placements.size() == 1) {
    return std::string{"workers move across at least two sets of CPUs, not one"};
  }
  for (const std::vector<std::uint32_t>& cpus : options.placements) {
    if (!worker::is_valid(worker::ControlMessage{0, worker::Rebind{cpus}})) {
      return std::string{"a set of CPUs to move across is empty, unordered or past CPU 1023"};
    }
  }
  Pool usable;
  for (const RosterEntry& entry : roster) {
    if (entry.min_footprint_bytes <= options.footprint_bytes) {
      usable.push_back(&entry);
    }
  }
  if (usable.empty()) {
    return std::string{"no workload of the roster accepts a footprint of " +
                       std::to_string(options.footprint_bytes) + " bytes"};
  }

  Schedule schedule;
  schedule.workers = options.workers;
  // What the workers start as: the first rotation, made as any other.
  std::vector<worker::Switch> first;
  for (std::size_t slot = 0; slot < options.workers; ++slot) {
    const worker::Switch change =
        assignment(pick(kPlan[0], usable, options.seed, 0, slot), options, 0, slot);
    schedule.start.push_back(Start{change.workload, change.footprint_bytes});
    first.push_back(change);
  }
  schedule.phase = phase_of(0, kPlan[0]);
  schedule.workload = label_of(first);

  std::string current = schedule.workload;
  for (std::uint64_t rotation = 1; rotation <= rotations; ++rotation) {
    Step step;
    step.at_ns = static_cast<std::int64_t>(rotation) * options.rotation_ns;
    const bool moves = !options.placements.empty() && rotation % kMoveEvery == 0;
    const Kind kind = moves ? Kind::kMove : kPlan[rotation % kPlan.size()];
    std::vector<worker::Switch> changes;
    for (std::size_t slot = 0; slot < options.workers; ++slot) {
      WorkerCommands commands;
      commands.slot = slot;
      if (moves) {
        const std::size_t where = (slot + rotation / kMoveEvery) % options.placements.size();
        commands.commands.emplace_back(worker::Rebind{options.placements[where]});
      } else {
        changes.push_back(assignment(pick(kind, usable, options.seed, rotation, slot), options,
                                     rotation, slot));
        commands.commands.emplace_back(changes.back());
      }
      step.workers.push_back(std::move(commands));
    }
    if (!moves) {
      current = label_of(changes);
    }
    step.phase = phase_of(rotation, kind);
    step.workload = current;
    schedule.steps.push_back(std::move(step));
  }
  return schedule;
}

}  // namespace loadforge::mixed
