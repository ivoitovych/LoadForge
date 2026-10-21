// SPDX-License-Identifier: GPL-3.0-or-later
//
// WorkloadContract against the sparse workload, at every chunk size the
// specification has.

#include "contracts/workloads/workload_contract.hpp"

#include <gtest/gtest.h>

#include "workloads/sparse/sparse.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::contracts {
namespace {

template <std::uint32_t kChunkRows>
struct Sparse {
  static constexpr const char* kName = "sparse";
  static workloads::WorkloadConfig config() {
    return workloads::WorkloadConfig{
        0x5EED, 3 * (workloads::sparse::kInputBytes + workloads::sparse::kOutputBytes), kChunkRows,
        simd::IsaPath::kScalar};
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return workloads::sparse::create(config);
  }
};

using ChunkSizes =
    ::testing::Types<Sparse<0>, Sparse<256>, Sparse<512>, Sparse<1024>, Sparse<2048>>;
INSTANTIATE_TYPED_TEST_SUITE_P(Sparse, WorkloadContract, ChunkSizes);

}  // namespace
}  // namespace loadforge::contracts
