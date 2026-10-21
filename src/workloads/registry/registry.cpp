// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/registry/registry.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/monte_carlo/monte_carlo.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/sort/sort.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

using Factory = core::Result<std::unique_ptr<Workload>, WorkloadError> (*)(const WorkloadConfig&);

using Describer = Descriptor (*)();

struct Entry {
  std::string_view name;
  Factory create;
  Describer describe;
};

// Sorted by name: names() is sorted by the contract, and a table that is
// sorted already needs no sort whose result a test would have to take on
// faith. The tests check the order.
constexpr std::array<Entry, 4> kEntries{{
    {compression::kName, &compression::create, &compression::workload_descriptor},
    {dense_linear::kName, &dense_linear::create, &dense_linear::workload_descriptor},
    {monte_carlo::kName, &monte_carlo::create, &monte_carlo::workload_descriptor},
    {sort::kName, &sort::create, &sort::workload_descriptor},
}};

WorkloadError unknown_workload(std::string_view name) {
  WorkloadError unknown;
  unknown.fault = WorkloadFault::kUnknownWorkload;
  unknown.detail = "no workload named '" + std::string{name} + "'";
  return unknown;
}

}  // namespace

BuiltinRegistry::BuiltinRegistry() noexcept = default;

std::vector<std::string> BuiltinRegistry::names() const {
  std::vector<std::string> names;
  names.reserve(kEntries.size());
  for (const Entry& entry : kEntries) {
    names.emplace_back(entry.name);
  }
  return names;
}

core::Result<std::unique_ptr<Workload>, WorkloadError> BuiltinRegistry::create(
    std::string_view name, const WorkloadConfig& config) const {
  const auto* const found = std::ranges::find(kEntries, name, &Entry::name);
  if (found == kEntries.end()) {
    return unknown_workload(name);
  }
  return found->create(config);
}

core::Result<Descriptor, WorkloadError> BuiltinRegistry::descriptor(std::string_view name) const {
  const auto* const found = std::ranges::find(kEntries, name, &Entry::name);
  if (found == kEntries.end()) {
    return unknown_workload(name);
  }
  return found->describe();
}

}  // namespace loadforge::workloads
