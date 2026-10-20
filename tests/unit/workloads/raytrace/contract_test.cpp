// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ray tracing workload against WorkloadContract, which it provides, and
// the workload's own executors against ExecutorContract, which it consumes
// (ADR 0003): the suite of each contract, run against the real thing and
// against the mock.

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include "contracts/worker/executor_contract.hpp"
#include "contracts/workloads/workload_contract.hpp"
#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/raytrace.hpp"

namespace loadforge::contracts {

// workload_contract.hpp also registers RegistryContract, which only the
// integration stream's registry instantiates.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);

namespace {

namespace rtt = loadforge::tests::raytrace;

struct ScalarDefault {
  static constexpr std::string_view kName = "raytrace";
  static workloads::WorkloadConfig config() { return rtt::config_for(simd::IsaPath::kScalar); }
  static auto create(const workloads::WorkloadConfig& config) {
    return workloads::raytrace::create(config);
  }
};

struct ScalarEdgeEight : ScalarDefault {
  static workloads::WorkloadConfig config() { return rtt::config_for(simd::IsaPath::kScalar, 8); }
};

struct ScalarEdgeThirtyTwo : ScalarDefault {
  static workloads::WorkloadConfig config() { return rtt::config_for(simd::IsaPath::kScalar, 32); }
};

/// The wide path of this build where this machine can run it; the scalar one
/// elsewhere, so the suite still has a subject.
struct WidestRunnable : ScalarDefault {
  static workloads::WorkloadConfig config() {
    return rtt::config_for(rtt::runnable_paths().back(), 16, 99);
  }
};

using WorkloadSubjects =
    ::testing::Types<ScalarDefault, ScalarEdgeEight, ScalarEdgeThirtyTwo, WidestRunnable>;
INSTANTIATE_TYPED_TEST_SUITE_P(Raytrace, WorkloadContract, WorkloadSubjects);

struct SerialMock {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<rtt::SerialExecutor>();
  }
};

struct PoolMock {
  static std::unique_ptr<worker::Executor> make() {
    return std::make_unique<rtt::PoolExecutor>(3);
  }
};

using ExecutorMocks = ::testing::Types<SerialMock, PoolMock>;
INSTANTIATE_TYPED_TEST_SUITE_P(RaytraceMocks, ExecutorContract, ExecutorMocks);

}  // namespace
}  // namespace loadforge::contracts
