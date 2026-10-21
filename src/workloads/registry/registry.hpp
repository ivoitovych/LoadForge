// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_REGISTRY_REGISTRY_HPP
#define LOADFORGE_WORKLOADS_REGISTRY_REGISTRY_HPP

// The provider of workloads::Registry (workloads/workload.hpp): every workload
// this build contains, by name, and nothing else. The worker creates a
// workload through it and the controller lists the names it accepts from it;
// neither names a workload's factory.
//
// A workload joins the build by one line in registry.cpp, and the tests of
// this module fail until its oracle joins the oracle registry as well, so a
// workload that could run without an independent check cannot be added.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {

class BuiltinRegistry final : public Registry {
 public:
  // Out of line, every member: an inline function used from several
  // translation units reads as uncovered in all but one (journal §1.22).
  BuiltinRegistry() noexcept;

  [[nodiscard]] std::vector<std::string> names() const override;
  [[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
      std::string_view name, const WorkloadConfig& config) const override;
  [[nodiscard]] core::Result<Descriptor, WorkloadError> descriptor(
      std::string_view name) const override;
};

}  // namespace loadforge::workloads

#endif
