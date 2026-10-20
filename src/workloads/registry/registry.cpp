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

struct Entry {
  std::string_view name;
  Factory create;
};

// Sorted by name: names() is sorted by the contract, and a table that is
// sorted already needs no sort whose result a test would have to take on
// faith. The tests check the order.
constexpr std::array<Entry, 4> kEntries{{
    {compression::kName, &compression::create},
    {dense_linear::kName, &dense_linear::create},
    {monte_carlo::kName, &monte_carlo::create},
    {sort::kName, &sort::create},
}};

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
    WorkloadError unknown;
    unknown.fault = WorkloadFault::kUnknownWorkload;
    unknown.detail = "no workload named '" + std::string{name} + "'";
    return unknown;
  }
  return found->create(config);
}

}  // namespace loadforge::workloads
