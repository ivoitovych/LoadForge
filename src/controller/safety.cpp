// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/safety.hpp"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/result.hpp"

namespace loadforge::controller {

std::string_view describe(StopStep step) noexcept {
  switch (step) {
    case StopStep::kNone:
      return "running";
    case StopStep::kRequested:
      return "stop requested";
    case StopStep::kTerminated:
      return "SIGTERM sent";
    case StopStep::kKilled:
      return "SIGKILL sent";
  }
  return "unrecognised stop step";
}

std::optional<int> escalation_due(StopStep step, std::int64_t since_ns, std::int64_t now_ns,
                                  const Grace& grace) noexcept {
  const std::int64_t waited_ns = now_ns - since_ns;
  if (step == StopStep::kRequested && waited_ns >= grace.stop_ns) {
    return SIGTERM;
  }
  if (step == StopStep::kTerminated && waited_ns >= grace.kill_ns) {
    return SIGKILL;
  }
  return std::nullopt;
}

StopStep step_after(int signal) noexcept {
  return signal == SIGKILL ? StopStep::kKilled : StopStep::kTerminated;
}

namespace {

bool any_memory_configured(const config::Config& config) {
  if (config.limits.memory.has_value()) {
    return true;
  }
  return std::ranges::any_of(config.workloads, [](const config::Workload& workload) {
    return workload.memory.has_value();
  });
}

}  // namespace

std::vector<LimitStatus> limit_statuses(const config::Config& config,
                                        const std::optional<config::MemoryBudget>& budget) {
  // Each status is built in a named local and moved in: a temporary with two
  // allocating members inside push_back leaves a cleanup edge no test can
  // take (journal §1.13).
  // One allocating expression per statement, and the note built before the
  // status that carries it; the status is then two moves and a flag.
  std::vector<LimitStatus> statuses;
  std::string duration_note = core::to_string(config.run.duration);
  LimitStatus duration{"duration", true, std::move(duration_note)};
  statuses.push_back(std::move(duration));
  if (any_memory_configured(config)) {
    std::string memory_note = "no memory budget could be read";
    if (budget.has_value()) {
      memory_note = "checked at start against a ceiling of ";
      memory_note += core::to_string(core::ByteSize::bytes(config::ceiling_of(*budget)));
    }
    LimitStatus memory{"memory", budget.has_value(), std::move(memory_note)};
    statuses.push_back(std::move(memory));
  }
  if (config.limits.max_temperature_c.has_value()) {
    std::string temperature_note = "not enforced until telemetry lands (M3); configured ";
    temperature_note += std::to_string(*config.limits.max_temperature_c);
    temperature_note += " C";
    LimitStatus temperature{"temperature", false, std::move(temperature_note)};
    statuses.push_back(std::move(temperature));
  }
  return statuses;
}

core::Result<core::Ok, std::string> check_memory(
    const config::Config& config, const std::optional<config::MemoryBudget>& budget) {
  if (!budget.has_value()) {
    return core::Ok{};
  }
  std::uint64_t total = 0;
  for (const config::Workload& workload : config.workloads) {
    if (!workload.enabled || !workload.memory.has_value()) {
      continue;
    }
    const std::string key = "workload." + workload.name;
    auto resolved = config::resolve_memory(*workload.memory, *budget, key);
    if (!resolved.has_value()) {
      std::string refusal = config::describe(resolved.error());
      return refusal;
    }
    total += resolved.value();
  }
  const std::uint64_t ceiling = config::ceiling_of(*budget);
  if (total > ceiling) {
    return "the workloads' memory adds up to " + core::to_string(core::ByteSize::bytes(total)) +
           ", past the ceiling of " + core::to_string(core::ByteSize::bytes(ceiling));
  }
  if (config.limits.memory.has_value()) {
    auto limit = config::resolve_memory(*config.limits.memory, *budget, "limits.memory");
    if (!limit.has_value()) {
      std::string refusal = config::describe(limit.error());
      return refusal;
    }
    if (total > limit.value()) {
      return "the workloads' memory adds up to " + core::to_string(core::ByteSize::bytes(total)) +
             ", past limits.memory of " + core::to_string(core::ByteSize::bytes(limit.value()));
    }
  }
  return core::Ok{};
}

}  // namespace loadforge::controller
