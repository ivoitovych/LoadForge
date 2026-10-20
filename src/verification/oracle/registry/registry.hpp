// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_REGISTRY_REGISTRY_HPP
#define LOADFORGE_VERIFICATION_ORACLE_REGISTRY_REGISTRY_HPP

// The provider of verification::OracleRegistry (verification/oracle.hpp):
// the independent oracle of every workload this build contains, by the
// workload's name. It names the same workloads as workloads::BuiltinRegistry
// and a test says so: a workload without an oracle is a stress test with no
// verdict, and the one thing this tool must not be.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification {

class BuiltinOracleRegistry final : public OracleRegistry {
 public:
  // Out of line, every member (journal §1.22).
  BuiltinOracleRegistry() noexcept;

  [[nodiscard]] std::vector<std::string> names() const override;
  [[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
      std::string_view name, const workloads::WorkloadConfig& config,
      const workloads::Plan& plan) const override;
};

}  // namespace loadforge::verification

#endif
