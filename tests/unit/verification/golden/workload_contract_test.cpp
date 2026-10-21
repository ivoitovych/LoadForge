// SPDX-License-Identifier: GPL-3.0-or-later
//
// WorkloadContract against this module's own mock (ADR 0003): the golden
// tests run on the mock, so the mock must be a workload by the provider's
// suite, not only by this module's reading.

#include "contracts/workloads/workload_contract.hpp"

#include <memory>

#include "core/result.hpp"
#include "unit/verification/golden/mock_workload.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {
namespace {

struct GoldenMock {
  static constexpr const char* kName = "golden_mock";
  static workloads::WorkloadConfig config() { return verification::golden_test::fixture_config(); }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return verification::golden_test::MockWorkload::create(config);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(GoldenMock, WorkloadContract, ::testing::Types<GoldenMock>);

}  // namespace
}  // namespace loadforge::contracts
