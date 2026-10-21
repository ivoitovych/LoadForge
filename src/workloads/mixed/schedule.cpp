// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/mixed/schedule.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

#include "controller/journal/journal.hpp"
#include "core/crc32.hpp"
#include "worker/control.hpp"

namespace loadforge::mixed {
namespace {

constexpr std::size_t kMaxLabelBytes = journal::kMaxLabelBytes;

}  // namespace

bool operator==(const WorkerCommands& lhs, const WorkerCommands& rhs) {
  // The variant compares within the alternative it holds, with the
  // comparisons worker/control.hpp defines for each.
  return std::tie(lhs.slot, lhs.commands) == std::tie(rhs.slot, rhs.commands);
}

bool operator==(const Step& lhs, const Step& rhs) {
  return std::tie(lhs.at_ns, lhs.phase, lhs.workload, lhs.workers) ==
         std::tie(rhs.at_ns, rhs.phase, rhs.workload, rhs.workers);
}

bool operator==(const Start& lhs, const Start& rhs) {
  return std::tie(lhs.workload, lhs.footprint_bytes) ==
         std::tie(rhs.workload, rhs.footprint_bytes);
}

bool operator==(const Schedule& lhs, const Schedule& rhs) {
  return std::tie(lhs.workers, lhs.start, lhs.phase, lhs.workload, lhs.steps) ==
         std::tie(rhs.workers, rhs.start, rhs.phase, rhs.workload, rhs.steps);
}

std::string bounded(std::string label) {
  label.resize(std::min(label.size(), kMaxLabelBytes));
  return label;
}

namespace {

void add(std::string& text, const std::string& key, const std::string& value) {
  text += key;
  text += '=';
  text += value;
  text += ';';
}

void add(std::string& text, const std::string& key, std::uint64_t value) {
  add(text, key, std::to_string(value));
}

void add_command(std::string& text, const worker::Command& command) {
  text += worker::command_name(command);
  text += '(';
  if (const auto* duty = std::get_if<worker::SetDuty>(&command)) {
    add(text, "busy", duty->busy_permille);
    add(text, "period", static_cast<std::uint64_t>(duty->period_ns));
  } else if (const auto* move = std::get_if<worker::Rebind>(&command)) {
    for (const std::uint32_t cpu : move->cpus) {
      add(text, "cpu", cpu);
    }
  } else if (const auto* change = std::get_if<worker::Switch>(&command)) {
    add(text, "workload", change->workload);
    add(text, "seed", change->seed);
    add(text, "footprint", change->footprint_bytes);
    add(text, "decomposition", change->decomposition);
    add(text, "oracle", change->duty.oracle_period);
    add(text, "integrity", change->duty.integrity_period);
  }
  text += ')';
}

}  // namespace

std::optional<std::string> problem(const Schedule& schedule, std::size_t workers) {
  if (schedule.workers != workers) {
    return "the schedule is for " + std::to_string(schedule.workers) + " worker(s) and the run has " +
           std::to_string(workers);
  }
  if (!schedule.start.empty() && schedule.start.size() != workers) {
    return std::string{"the schedule names a start for some workers and not for all"};
  }
  if (schedule.phase.size() > kMaxLabelBytes || schedule.workload.size() > kMaxLabelBytes) {
    return std::string{"the schedule's first phase or workload label is past the journal's limit"};
  }
  std::int64_t before = 0;
  for (const Step& step : schedule.steps) {
    if (step.at_ns < before) {
      return "the step \"" + step.phase + "\" is not after the one before it";
    }
    before = step.at_ns;
    if (step.phase.size() > kMaxLabelBytes || step.workload.size() > kMaxLabelBytes) {
      return "the step \"" + step.phase + "\" has a label past the journal's limit";
    }
    for (const WorkerCommands& each : step.workers) {
      if (each.slot >= workers) {
        return "the step \"" + step.phase + "\" names worker " + std::to_string(each.slot) +
               " of " + std::to_string(workers);
      }
      for (const worker::Command& command : each.commands) {
        if (!worker::is_valid(worker::ControlMessage{0, command})) {
          return "the step \"" + step.phase + "\" sends worker " + std::to_string(each.slot) +
                 " a " + std::string{worker::command_name(command)} +
                 " that breaks the rules of its command";
        }
      }
    }
  }
  return std::nullopt;
}

std::uint32_t digest(const Schedule& schedule) {
  std::string text;
  add(text, "workers", schedule.workers);
  add(text, "phase", schedule.phase);
  add(text, "workload", schedule.workload);
  for (const Start& start : schedule.start) {
    add(text, "start", start.workload);
    add(text, "footprint", start.footprint_bytes);
  }
  for (const Step& step : schedule.steps) {
    add(text, "at", static_cast<std::uint64_t>(step.at_ns));
    add(text, "phase", step.phase);
    add(text, "workload", step.workload);
    for (const WorkerCommands& each : step.workers) {
      add(text, "slot", each.slot);
      for (const worker::Command& command : each.commands) {
        add_command(text, command);
      }
    }
  }
  return core::crc32(std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

}  // namespace loadforge::mixed
