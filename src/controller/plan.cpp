// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "core/byte_size.hpp"
#include "core/result.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "primitives/simd/dispatch.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/numa.hpp"
#include "topology/snapshot.hpp"
#include "worker/entry.hpp"
#include "workloads/workload.hpp"

namespace loadforge::controller {
namespace {

std::string cpu_list(const std::vector<std::uint32_t>& cpus) {
  if (cpus.empty()) {
    return "unpinned";
  }
  std::string text = cpus.size() == 1 ? "cpu " : "cpus ";
  for (std::size_t index = 0; index < cpus.size(); ++index) {
    if (index != 0) {
      text += ',';
    }
    text += std::to_string(cpus[index]);
  }
  return text;
}

/// The physical core the CPU belongs to in the snapshot, if the snapshot has it.
std::optional<std::uint32_t> core_of(const topology::Snapshot& machine, std::uint32_t cpu) {
  for (const topology::LogicalCpu& each : machine.cpus.cpus()) {
    if (each.id == cpu) {
      return each.core.core;
    }
  }
  return std::nullopt;
}

std::string refused(std::string_view name, const workloads::WorkloadError& error) {
  return std::string{name} + ": " + std::string{workloads::describe(error.fault)} + ": " +
         error.detail;
}

}  // namespace

std::vector<std::uint32_t> assign_cpus(std::size_t slot, std::size_t threads,
                                       const std::vector<std::uint32_t>& all) {
  std::vector<std::uint32_t> assigned;
  if (all.empty()) {
    return assigned;
  }
  const std::size_t count = std::min(threads, all.size());
  const std::size_t start = (slot * threads) % all.size();
  assigned.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    assigned.push_back(all[(start + index) % all.size()]);
  }
  return assigned;
}

core::Result<simd::IsaPath, std::string> choose_isa(std::optional<simd::IsaPath> requested,
                                                    simd::IsaSet supported, simd::IsaSet compiled,
                                                    simd::IsaSet workload_paths) {
  if (!requested.has_value()) {
    // select_path answers an unrequested call with a path always (dispatch.hpp:
    // "the unrequested call never fails"), so value() is its answer and not a
    // branch of this function.
    return simd::select_path(std::nullopt, supported, compiled.intersect(workload_paths)).value();
  }
  if (!workload_paths.contains(*requested)) {
    return "the workload has no " + std::string{simd::describe(*requested)} + " kernel";
  }
  auto selected = simd::select_path(requested, supported, compiled);
  if (!selected.has_value()) {
    return selected.error().detail;
  }
  return selected.value();
}

std::string describe_state(const fpenv::FpState& state) {
  const auto flag = [](bool on) { return on ? "on" : "off"; };
  std::string text = "rounding " + std::string{fpenv::describe(state.rounding)} +
                     ", flush-to-zero " + flag(state.flush_to_zero);
  if (state.denormals_are_zero.has_value()) {
    text += std::string{", denormals-are-zero "} + flag(*state.denormals_are_zero);
  }
  if (state.default_nan.has_value()) {
    text += std::string{", default-NaN "} + flag(*state.default_nan);
  }
  return text;
}

core::Result<WorkerPlan, std::string> plan_worker(const config::Workload& entry, std::size_t slot,
                                                  const std::optional<config::MemoryBudget>& budget,
                                                  const VerificationOptions& options,
                                                  const topology::Snapshot* machine) {
  WorkerPlan plan;
  std::vector<std::string> known;
  if (options.registry != nullptr) {
    known = options.registry->names();
  }
  if (std::ranges::find(known, entry.name) == known.end()) {
    plan.description = entry.name + ": null workload";
    return plan;
  }
  if (options.line_bytes == 0) {
    return entry.name +
           ": the machine's cache-line size is not known, and an error record's classification "
           "depends on it; refusing to assume one";
  }

  if (entry.threads.is_all()) {
    plan.cpus = options.cpus;
    plan.threads = static_cast<std::uint32_t>(std::max<std::size_t>(options.cpus.size(), 1));
  } else {
    plan.threads = entry.threads.value();
    plan.cpus = assign_cpus(slot, plan.threads, options.cpus);
  }

  std::uint64_t footprint = options.footprint_bytes;
  if (entry.memory.has_value() && (entry.memory->is_absolute() || budget.has_value())) {
    auto resolved = config::resolve_memory(*entry.memory, budget.value_or(config::MemoryBudget{}),
                                           "workload." + entry.name);
    if (!resolved.has_value()) {
      return config::describe(resolved.error());
    }
    footprint = resolved.value();
  }

  worker::WorkloadArgs args;
  args.workload = entry.name;
  args.seed = options.seed + slot * kSeedStride;
  args.footprint_bytes = footprint;
  args.threads = plan.threads;
  args.decomposition = options.decomposition;
  args.duty = options.duty;
  args.line_bytes = options.line_bytes;

  // The workload's kernel set is only known from a workload, and a workload
  // is only made for a path: made once for scalar, which every one has, to
  // ask; made again for the path chosen, to have its refusal now and not in a
  // worker after the fork.
  workloads::WorkloadConfig config{args.seed, footprint, args.decomposition,
                                   simd::IsaPath::kScalar};
  auto asked = options.registry->create(entry.name, config);
  if (!asked.has_value()) {
    return refused(entry.name, asked.error());
  }
  const simd::IsaSet supported = options.isa_probe != nullptr
                                     ? options.isa_probe->supported()
                                     : simd::IsaSet{simd::IsaPath::kScalar};
  auto path = choose_isa(options.requested_isa, supported, simd::compiled_paths(),
                         asked.value()->descriptor().paths);
  if (!path.has_value()) {
    return entry.name + ": " + path.error();
  }
  args.isa = path.value();
  config.isa = args.isa;
  auto created = options.registry->create(entry.name, config);
  if (!created.has_value()) {
    return refused(entry.name, created.error());
  }

  if (plan.cpus.size() == 1 && machine != nullptr) {
    args.physical_core = core_of(*machine, plan.cpus.front());
    const topology::NumaNode* node = machine->numa.node_of(plan.cpus.front());
    if (node != nullptr) {
      args.numa_node = node->id;
    }
  }
  WorkerWork work;
  work.descriptor = created.value()->descriptor();
  work.isa = args.isa;
  work.decomposition = created.value()->plan().decomposition;
  work.duty = args.duty;
  work.logical_bytes = created.value()->plan().logical_bytes;
  work.threads = plan.threads;
  if (plan.cpus.size() == 1) {
    work.cpu = plan.cpus.front();
  }
  work.seed = args.seed;
  work.footprint_bytes = footprint;
  work.line_bytes = args.line_bytes;
  plan.work = std::move(work);
  plan.description = entry.name + ": " + std::to_string(plan.threads) + " thread(s) on " +
                     cpu_list(plan.cpus) + ", footprint " +
                     core::to_string(core::ByteSize::bytes(footprint)) + ", " +
                     std::to_string(created.value()->plan().units) + " unit(s) resident, " +
                     std::string{simd::describe(args.isa)} + " path";
  plan.args = std::move(args);
  return plan;
}

}  // namespace loadforge::controller
