// SPDX-License-Identifier: GPL-3.0-or-later
//
// WorkloadContract against the graph workload, one subject per decomposition
// and per ISA path this build has a kernel for.

#include <gtest/gtest.h>

#include <memory>

#include "contracts/workloads/workload_contract.hpp"
#include "graph_support.hpp"

namespace loadforge::contracts {
namespace {

using namespace graph_test;

// The contract header registers the registry suite too; this binary has no
// registry, which is the integration stream's.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);

template <std::uint32_t kDecomposition, simd::IsaPath kPath>
struct Subject {
  static constexpr std::string_view kName = graph_test::spec::kName;
  static workloads::WorkloadConfig config() {
    // A path this machine cannot run is replaced by the scalar one: the
    // suite then runs the same arithmetic, which the determinism tests prove
    // is the same bits.
    return config_for(kDecomposition, can_run(kPath) ? kPath : simd::IsaPath::kScalar);
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return graph_test::spec::create(config);
  }
};

using Scalar = ::testing::Types<
    Subject<0, simd::IsaPath::kScalar>, Subject<1, simd::IsaPath::kScalar>,
    Subject<2, simd::IsaPath::kScalar>, Subject<4, simd::IsaPath::kScalar>,
    Subject<8, simd::IsaPath::kScalar>>;
INSTANTIATE_TYPED_TEST_SUITE_P(GraphScalar, WorkloadContract, Scalar);

#if defined(__x86_64__) && !defined(LOADFORGE_SIMD_SCALAR)
using Vector = ::testing::Types<Subject<0, simd::IsaPath::kAvx2>, Subject<2, simd::IsaPath::kAvx2>,
                                Subject<4, simd::IsaPath::kAvx2>, Subject<8, simd::IsaPath::kAvx2>>;
INSTANTIATE_TYPED_TEST_SUITE_P(GraphVector, WorkloadContract, Vector);
#elif defined(__aarch64__) && !defined(LOADFORGE_SIMD_SCALAR)
using Vector = ::testing::Types<Subject<0, simd::IsaPath::kNeon>, Subject<2, simd::IsaPath::kNeon>,
                                Subject<4, simd::IsaPath::kNeon>, Subject<8, simd::IsaPath::kNeon>>;
INSTANTIATE_TYPED_TEST_SUITE_P(GraphVector, WorkloadContract, Vector);
#endif

}  // namespace
}  // namespace loadforge::contracts
