// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_MIXED_STUB_REGISTRY_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_MIXED_STUB_REGISTRY_HPP

// This module's own reading of workloads::Registry (workload.hpp), as a
// schedule uses it: the names a build has, and whether a workload can be made
// at a footprint. Stub workloads that compute nothing; the real registry's own
// suite is the registry's. ADR 0003: the provider's suite is run against THIS
// (roster_test.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::mixed_testing {

class StubWorkload final : public workloads::Workload {
 public:
  StubWorkload(workloads::Descriptor descriptor, workloads::Plan plan)
      : descriptor_(std::move(descriptor)), plan_(plan) {}
  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return plan_; }
  void generate(std::uint64_t /*index*/, std::span<std::byte> input) const noexcept override {
    std::fill(input.begin(), input.end(), std::byte{1});
  }
  void compute(std::uint64_t /*index*/, std::span<const std::byte> /*input*/,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& /*executor*/) const noexcept override {
    std::fill(output.begin(), output.end(), std::byte{2});
  }

 private:
  workloads::Descriptor descriptor_;
  workloads::Plan plan_;
};

/// What a stub workload is: its name, how many bytes one unit takes, and
/// whether it can be made at all.
struct StubEntry {
  std::string name;
  std::uint64_t unit_bytes = 1;
  bool makeable = true;
};

class StubRegistry final : public workloads::Registry {
 public:
  explicit StubRegistry(std::vector<StubEntry> entries) : entries_(std::move(entries)) {
    std::ranges::sort(entries_, {}, &StubEntry::name);
  }

  [[nodiscard]] std::vector<std::string> names() const override {
    std::vector<std::string> out;
    for (const StubEntry& entry : entries_) {
      out.push_back(entry.name);
    }
    return out;
  }

  [[nodiscard]] core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      std::string_view name, const workloads::WorkloadConfig& config) const override {
    ++creations;
    const auto found = std::ranges::find(entries_, name, &StubEntry::name);
    if (found == entries_.end()) {
      return unknown(name);
    }
    if (!found->makeable) {
      return workloads::WorkloadError{workloads::WorkloadFault::kConfiguration, "never"};
    }
    if (config.footprint_bytes < found->unit_bytes) {
      return workloads::WorkloadError{workloads::WorkloadFault::kFootprintTooSmall, "too small"};
    }
    workloads::Plan plan;
    plan.input_bytes = found->unit_bytes / 2 + 1;
    plan.output_bytes = found->unit_bytes - plan.input_bytes + 1;
    plan.units = config.footprint_bytes / found->unit_bytes;
    plan.decomposition = 1;
    std::unique_ptr<workloads::Workload> made =
        std::make_unique<StubWorkload>(descriptor_of(*found), plan);
    return made;
  }

  [[nodiscard]] core::Result<workloads::Descriptor, workloads::WorkloadError> descriptor(
      std::string_view name) const override {
    const auto found = std::ranges::find(entries_, name, &StubEntry::name);
    if (found == entries_.end()) {
      return unknown(name);
    }
    return descriptor_of(*found);
  }

  mutable int creations = 0;

 private:
  static workloads::WorkloadError unknown(std::string_view name) {
    workloads::WorkloadError error;
    error.fault = workloads::WorkloadFault::kUnknownWorkload;
    error.detail = "no workload named '" + std::string{name} + "'";
    return error;
  }
  static workloads::Descriptor descriptor_of(const StubEntry& entry) {
    workloads::Descriptor descriptor;
    descriptor.name = entry.name;
    return descriptor;
  }

  std::vector<StubEntry> entries_;
};

}  // namespace loadforge::mixed_testing

#endif
