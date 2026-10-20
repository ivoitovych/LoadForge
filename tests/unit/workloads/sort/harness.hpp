// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_SORT_HARNESS_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_SORT_HARNESS_HPP

// Shared by every tier of the sort's tests: a configured workload, and units run
// through it.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "worker/executor.hpp"
#include "workloads/sort/sort.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sort::testing {

using Bytes = std::vector<std::byte>;

inline constexpr std::uint64_t kUnitBytes = kInputBytes + kOutputBytes;

[[nodiscard]] inline WorkloadConfig config_with(std::uint32_t chunks, std::uint64_t seed = 1) {
  return WorkloadConfig{seed, 3 * kUnitBytes, chunks, simd::IsaPath::kScalar};
}

[[nodiscard]] inline std::unique_ptr<Workload> make(std::uint32_t chunks = 0,
                                                    std::uint64_t seed = 1) {
  auto created = create(config_with(chunks, seed));
  EXPECT_TRUE(created.has_value());
  return created.has_value() ? std::move(created).value() : nullptr;
}

[[nodiscard]] inline Bytes generate(const Workload& workload, std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

/// compute() with output and scratch prefilled with `fill`.
[[nodiscard]] inline Bytes sorted(const Workload& workload, std::uint64_t index, const Bytes& input,
                                  worker::Executor& executor, std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

inline void put_key(Bytes& input, std::size_t position, std::uint64_t key) {
  for (std::size_t byte = 0; byte < kKeyBytes; ++byte) {
    input[position * kKeyBytes + byte] = static_cast<std::byte>((key >> (8U * byte)) & 0xFFU);
  }
}

[[nodiscard]] inline std::uint64_t key_of(const Bytes& bytes, std::size_t position) {
  return load_le64(bytes, position * kKeyBytes);
}

[[nodiscard]] inline std::uint32_t index_of(const Bytes& output, std::size_t position) {
  return load_le32(output, index_offset(position));
}

/// An input of kKeys keys, key e being `key(e)`.
template <typename KeyOf>
[[nodiscard]] Bytes input_of(KeyOf key) {
  Bytes input(kInputBytes);
  for (std::size_t position = 0; position < kKeys; ++position) {
    put_key(input, position, key(position));
  }
  return input;
}

}  // namespace loadforge::workloads::sort::testing

#endif
