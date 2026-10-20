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
#include "verification/oracle/compression/oracle.hpp"
#include "verification/oracle/dense_linear/dense_linear.hpp"
#include "verification/oracle/monte_carlo/monte_carlo_oracle.hpp"
#include "verification/oracle/sort/sort_oracle.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/sort/spec.hpp"
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
constexpr std::array<Entry, 4> kEntries{{
    {workloads::compression::kName, &compression_oracle},
    {workloads::dense_linear::kName, &dense_linear_oracle},
    {workloads::monte_carlo::kName, &monte_carlo_oracle},
    {workloads::sort::kName, &sort_oracle},
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
  const auto found = std::ranges::find(kEntries, name, &Entry::name);
  if (found == kEntries.end()) {
    OracleError unknown;
    unknown.fault = OracleFault::kUnknownWorkload;
    unknown.detail = "no oracle for a workload named '" + std::string{name} + "'";
    return unknown;
  }
  return found->create(config, plan);
}

}  // namespace loadforge::verification
