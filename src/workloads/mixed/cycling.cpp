// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/mixed/cycling.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "worker/control.hpp"
#include "workloads/mixed/mixed.hpp"
#include "workloads/mixed/schedule.hpp"

namespace loadforge::mixed {
namespace {

/// One level of the load cycle: how much of every period is busy, in
/// thousandths, with 0 meaning paused.
struct LoadLevel {
  std::uint16_t busy_permille;
  const char* name;
};

constexpr std::array<LoadLevel, 4> kLoadLevels{
    {{0, "idle"}, {1000, "heavy"}, {500, "moderate"}, {1000, "heavy"}}};

/// One level of the core cycle: the share of the workers that work, in
/// thousandths: an eighth, half, all, a quarter, all.
constexpr std::array<std::uint16_t, 5> kCoreLevels{125, 500, 1000, 250, 1000};

/// What a worker is doing, as the schedule has left it.
struct State {
  bool paused = false;
  std::uint16_t busy_permille = 1000;
};

/// The commands that bring `state` to a level of load, none if it is there.
std::vector<worker::Command> towards_load(State& state, std::uint16_t busy_permille,
                                          std::int64_t period_ns) {
  std::vector<worker::Command> commands;
  if (busy_permille == 0) {
    if (!state.paused) {
      commands.emplace_back(worker::Pause{});
    }
    state.paused = true;
    return commands;
  }
  if (state.paused) {
    commands.emplace_back(worker::Resume{});
  }
  if (state.busy_permille != busy_permille) {
    commands.emplace_back(worker::SetDuty{busy_permille, period_ns});
  }
  state.paused = false;
  state.busy_permille = busy_permille;
  return commands;
}

}  // namespace

core::Result<Schedule, std::string> make_cycling(const CyclingOptions& options) {
  if (options.workers == 0) {
    return std::string{"a cycling schedule needs at least one worker"};
  }
  if (options.duration_ns <= 0 || options.dwell_ns <= 0 || options.period_ns <= 0) {
    return std::string{"the duration, the dwell of a level and the period must be positive"};
  }
  const auto levels = static_cast<std::uint64_t>((options.duration_ns - 1) / options.dwell_ns) + 1;
  if (levels > kMaxSteps) {
    return "a level every " + std::to_string(options.dwell_ns / 1'000'000) + " ms for " +
           std::to_string(options.duration_ns / 1'000'000) + " ms is more than " +
           std::to_string(kMaxSteps) + " steps";
  }
  if (options.workers > kMaxCommands / levels) {
    return std::to_string(options.workers) + " worker(s) over " + std::to_string(levels) +
           " levels is more than " + std::to_string(kMaxCommands) + " commands";
  }
  Schedule schedule;
  schedule.workers = options.workers;
  schedule.phase = "run";
  schedule.workload = std::string{"cycling "} + describe(options.kind);
  std::vector<State> states(options.workers);
  for (std::uint64_t level = 0; level < levels; ++level) {
    Step step;
    step.at_ns = static_cast<std::int64_t>(level) * options.dwell_ns;
    step.workload = schedule.workload;
    std::string name;
    std::size_t working = options.workers;
    std::uint16_t busy_permille = 1000;
    if (options.kind == CycleKind::kLoad) {
      const LoadLevel& each = kLoadLevels.at(level % kLoadLevels.size());
      busy_permille = each.busy_permille;
      name = each.name;
    } else {
      const std::uint16_t share = kCoreLevels.at(level % kCoreLevels.size());
      working = std::max<std::size_t>(1, options.workers * share / 1000);
      name = std::to_string(working) + " of " + std::to_string(options.workers);
    }
    for (std::size_t slot = 0; slot < options.workers; ++slot) {
      // The cores cycle keeps the first `working` workers at full load and
      // pauses the rest; the load cycle moves every worker alike.
      std::uint16_t wanted = busy_permille;
      if (options.kind == CycleKind::kCores) {
        wanted = slot < working ? 1000 : 0;
      }
      WorkerCommands commands;
      commands.slot = slot;
      commands.commands = towards_load(states[slot], wanted, options.period_ns);
      if (!commands.commands.empty()) {
        step.workers.push_back(std::move(commands));
      }
    }
    step.phase = bounded("c" + std::to_string(level) + " " + name);
    schedule.steps.push_back(std::move(step));
  }
  return schedule;
}

}  // namespace loadforge::mixed
