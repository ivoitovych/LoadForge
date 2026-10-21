// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_SORT_CHECK_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_SORT_CHECK_HPP

// The sort's output checker, written from workloads/sort/spec.hpp and from
// nothing the sort implements (verification.md §3). It never sorts: it proves
// the indices a permutation, compares each key with the input key its index
// names, and compares neighbours. Three steps in the order the specification
// gives, the first to fail giving the verdict.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "workloads/sort/spec.hpp"

namespace loadforge::workloads::sort::checking {

enum class Verdict : std::uint8_t {
  kAgreement,
  kIndexOutOfRange,
  kIndexRepeated,
  kKeyDiffers,
  kOrderBroken,
  kNotChecked,
};

struct Outcome {
  Verdict verdict = Verdict::kNotChecked;
  std::size_t position = 0;  ///< the output position named, for every verdict but the two ends
  std::size_t blocks_compared = 0;
};

[[nodiscard]] inline Outcome check(std::span<const std::byte> input,
                                   std::span<const std::byte> output) {
  if (input.size() != kInputBytes || output.size() != kOutputBytes) {
    return Outcome{Verdict::kNotChecked, 0, 0};
  }
  std::vector<bool> seen(kKeys, false);
  for (std::size_t position = 0; position < kKeys; ++position) {
    const std::uint32_t index = load_le32(output, index_offset(position));
    if (index >= kKeys) {
      return Outcome{Verdict::kIndexOutOfRange, position, position + 1};
    }
    if (seen[index]) {
      return Outcome{Verdict::kIndexRepeated, position, position + 1};
    }
    seen[index] = true;
  }
  for (std::size_t position = 0; position < kKeys; ++position) {
    const std::uint32_t index = load_le32(output, index_offset(position));
    if (load_le64(output, key_offset(position)) != load_le64(input, key_offset(index))) {
      return Outcome{Verdict::kKeyDiffers, position, kKeys};
    }
  }
  for (std::size_t position = 1; position < kKeys; ++position) {
    const Entry before{load_le64(output, key_offset(position - 1)),
                       load_le32(output, index_offset(position - 1))};
    const Entry here{load_le64(output, key_offset(position)),
                     load_le32(output, index_offset(position))};
    if (!precedes(before, here)) {
      return Outcome{Verdict::kOrderBroken, position, kKeys};
    }
  }
  return Outcome{Verdict::kAgreement, 0, kKeys};
}

}  // namespace loadforge::workloads::sort::checking

#endif
