// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/registry/registry.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "workloads/adaptive_mesh/adaptive_mesh.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"
#include "workloads/graph/graph.hpp"
#include "workloads/graph/spec.hpp"
#include "workloads/monte_carlo/monte_carlo.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/particles/particles.hpp"
#include "workloads/particles/spec.hpp"
#include "workloads/raytrace/raytrace.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/sort/sort.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/sparse/sparse.hpp"
#include "workloads/sparse/spec.hpp"
#include "workloads/stencil/spec.hpp"
#include "workloads/stencil/stencil.hpp"
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
constexpr std::array<Entry, 11> kEntries{{
    {adaptive_mesh::kName, &adaptive_mesh::create, &adaptive_mesh::workload_descriptor},
    {compression::kName, &compression::create, &compression::workload_descriptor},
    {dense_linear::kName, &dense_linear::create, &dense_linear::workload_descriptor},
    {fft::kName, &fft::create, &fft::workload_descriptor},
    {graph::kName, &graph::create, &graph::workload_descriptor},
    {monte_carlo::kName, &monte_carlo::create, &monte_carlo::workload_descriptor},
    {particles::kName, &particles::create, &particles::workload_descriptor},
    {raytrace::kName, &raytrace::create, &raytrace::workload_descriptor},
    {sort::kName, &sort::create, &sort::workload_descriptor},
    {sparse::kName, &sparse::create, &sparse::workload_descriptor},
    {stencil::kName, &stencil::create, &stencil::workload_descriptor},
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
