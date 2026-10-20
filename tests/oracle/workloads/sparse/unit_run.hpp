// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_SPARSE_UNIT_RUN_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_SPARSE_UNIT_RUN_HPP

// Making and solving one unit, for the tiers above T1.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "worker/executor.hpp"
#include "workloads/sparse/sparse.hpp"
#include "workloads/sparse/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sparse::tests {

using Bytes = std::vector<std::byte>;

inline std::unique_ptr<Workload> make_workload(std::uint64_t seed, std::uint32_t chunk_rows) {
  auto created = create(WorkloadConfig{seed, 3 * (kInputBytes + kOutputBytes), chunk_rows,
                                       simd::IsaPath::kScalar});
  EXPECT_TRUE(created.has_value());
  return created.has_value() ? std::move(created).value() : nullptr;
}

inline Bytes generate_input(const Workload& workload, std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

inline Bytes solve(const Workload& workload, std::uint64_t index, const Bytes& input,
                   worker::Executor& executor, std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

}  // namespace loadforge::workloads::sparse::tests

#endif
