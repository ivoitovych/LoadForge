// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_MIXED_LISTING_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_MIXED_LISTING_HPP

// A schedule in the listing format of tests/fixtures/workloads/mixed/generate.py,
// so that the C++ and the Python are compared as text, line for line.

#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>

#include "workloads/mixed/schedule.hpp"

namespace loadforge::mixed_testing {

inline std::string text_of(const worker::Command& command) {
  if (const auto* change = std::get_if<worker::Switch>(&command)) {
    return "switch " + change->workload + " " + std::to_string(change->seed) + " " +
           std::to_string(change->footprint_bytes) + " " + std::to_string(change->decomposition) +
           " " + std::to_string(change->duty.oracle_period) + " " +
           std::to_string(change->duty.integrity_period);
  }
  if (const auto* move = std::get_if<worker::Rebind>(&command)) {
    std::string cpus;
    for (const std::uint32_t cpu : move->cpus) {
      cpus += cpus.empty() ? "" : ",";
      cpus += std::to_string(cpu);
    }
    return "rebind " + cpus;
  }
  if (const auto* duty = std::get_if<worker::SetDuty>(&command)) {
    return "set duty " + std::to_string(duty->busy_permille) + " " +
           std::to_string(duty->period_ns);
  }
  return std::string{worker::command_name(command)};
}

inline std::string listing(const mixed::Schedule& schedule) {
  std::string text = "workers " + std::to_string(schedule.workers) + "\n";
  text += "phase " + schedule.phase + "\n";
  text += "workload " + schedule.workload + "\n";
  for (const mixed::Start& start : schedule.start) {
    text += "start " + start.workload + " " + std::to_string(start.footprint_bytes) + "\n";
  }
  for (const mixed::Step& step : schedule.steps) {
    text +=
        "step " + std::to_string(step.at_ns) + " | " + step.phase + " | " + step.workload + "\n";
    for (const mixed::WorkerCommands& each : step.workers) {
      for (const worker::Command& command : each.commands) {
        text += "  worker " + std::to_string(each.slot) + ": " + text_of(command) + "\n";
      }
    }
  }
  char digest[16];
  std::snprintf(digest, sizeof digest, "%08x", mixed::digest(schedule));
  text += std::string{"digest "} + digest + "\n";
  return text;
}

}  // namespace loadforge::mixed_testing

#endif
