// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/registry/registry.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/adaptive_mesh/oracle.hpp"
#include "verification/oracle/compression/oracle.hpp"
#include "verification/oracle/dense_linear/dense_linear.hpp"
#include "verification/oracle/fft/fft_oracle.hpp"
#include "verification/oracle/graph/graph_oracle.hpp"
#include "verification/oracle/monte_carlo/monte_carlo_oracle.hpp"
#include "verification/oracle/particles/particles_oracle.hpp"
#include "verification/oracle/raytrace/oracle.hpp"
#include "verification/oracle/sort/sort_oracle.hpp"
#include "verification/oracle/sparse/sparse_oracle.hpp"
#include "verification/oracle/stencil/stencil_oracle.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/fft/spec.hpp"
#include "workloads/graph/spec.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/particles/spec.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/sparse/spec.hpp"
#include "workloads/stencil/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification {
namespace {

using Factory = core::Result<std::unique_ptr<Oracle>, OracleError> (*)(
    const workloads::WorkloadConfig&, const workloads::Plan&);

// Each oracle's factory has its own name and, for dense linear, a defaulted
// third parameter; one function of the registry's shape per oracle adapts
// them without touching their headers.
core::Result<std::unique_ptr<Oracle>, OracleError> compression_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan) {
  return oracle::compression::create(config, plan);
}

core::Result<std::unique_ptr<Oracle>, OracleError> dense_linear_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan) {
  return oracle::dense_linear::make_oracle(config, plan);
}

core::Result<std::unique_ptr<Oracle>, OracleError> monte_carlo_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan) {
  return oracle::make_monte_carlo_oracle(config, plan);
}

core::Result<std::unique_ptr<Oracle>, OracleError> sort_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan) {
  return oracle::make_sort_oracle(config, plan);
}

struct Entry {
  std::string_view name;
  Factory create;
};

// Sorted by name, as the workload registry's table is.
constexpr std::array<Entry, 11> kEntries{{
    {workloads::adaptive_mesh::kName, &oracle::adaptive_mesh::create},
    {workloads::compression::kName, &compression_oracle},
    {workloads::dense_linear::kName, &dense_linear_oracle},
    {workloads::fft::kName, &oracle::fft::create},
    {workloads::graph::kName, &oracle::graph::create},
    {workloads::monte_carlo::kName, &monte_carlo_oracle},
    {workloads::particles::kName, &oracle::particles::create},
    {workloads::raytrace::kName, &oracle::raytrace::create},
    {workloads::sort::kName, &sort_oracle},
    {workloads::sparse::kName, &oracle::sparse::create},
    {workloads::stencil::kName, &oracle::stencil::create},
}};

}  // namespace

BuiltinOracleRegistry::BuiltinOracleRegistry() noexcept = default;

std::vector<std::string> BuiltinOracleRegistry::names() const {
  std::vector<std::string> names;
  names.reserve(kEntries.size());
  for (const Entry& entry : kEntries) {
    names.emplace_back(entry.name);
  }
  return names;
}

core::Result<std::unique_ptr<Oracle>, OracleError> BuiltinOracleRegistry::create(
    std::string_view name, const workloads::WorkloadConfig& config,
    const workloads::Plan& plan) const {
  const auto* const found = std::ranges::find(kEntries, name, &Entry::name);
  if (found == kEntries.end()) {
    OracleError unknown;
    unknown.fault = OracleFault::kUnknownWorkload;
    unknown.detail = "no oracle for a workload named '" + std::string{name} + "'";
    return unknown;
  }
  return found->create(config, plan);
}

}  // namespace loadforge::verification
