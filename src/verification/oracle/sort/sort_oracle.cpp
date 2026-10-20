// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/sort/sort_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "workloads/sort/spec.hpp"

namespace loadforge::verification::oracle {
namespace {

namespace spec = workloads::sort;

constexpr std::size_t kWordBits = 64;
constexpr std::size_t kSeenWords = spec::kKeys / kWordBits;

ExactMismatch indices_mismatch(const char* what, std::size_t position) {
  return ExactMismatch{std::string(what) + std::to_string(position), std::nullopt, position + 1};
}

class SortOracle final : public Oracle {
 public:
  explicit SortOracle(ScratchAllocator allocator) : allocator_(std::move(allocator)) {}

  [[nodiscard]] std::string_view workload() const noexcept override { return spec::kName; }
  [[nodiscard]] Contract contract() const noexcept override { return Contract::kExact; }

  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override {
    if (evidence.input.size() != spec::kInputBytes ||
        evidence.output.size() != spec::kOutputBytes) {
      return NotChecked{"sort unit", "evidence is not a sort unit's input and output size", 0};
    }
    const std::unique_ptr<std::uint64_t[]> seen = allocator_(kSeenWords);
    if (seen == nullptr) {
      return NotChecked{"sort indices", "no memory for the permutation check's scratch", 0};
    }
    if (auto bad = check_indices(evidence.output, seen.get())) {
      return *std::move(bad);
    }
    if (auto bad = check_keys(evidence)) {
      return *std::move(bad);
    }
    if (auto bad = check_order(evidence.output)) {
      return *std::move(bad);
    }
    return Agreement{spec::kKeys};
  }

 private:
  // Step 1: the indices are a permutation of 0 .. kKeys - 1. The smallest
  // position at which one is out of range, or equal to an earlier one.
  static std::optional<ExactMismatch> check_indices(std::span<const std::byte> output,
                                                    std::uint64_t* seen) {
    for (std::size_t position = 0; position < spec::kKeys; ++position) {
      const std::uint32_t index = spec::load_le32(output, spec::index_offset(position));
      if (index >= spec::kKeys) {
        return indices_mismatch("index out of range at position ", position);
      }
      std::uint64_t& word = seen[index / kWordBits];
      const std::uint64_t bit = std::uint64_t{1} << (index % kWordBits);
      if ((word & bit) != 0) {
        return indices_mismatch("index repeated at position ", position);
      }
      word |= bit;
    }
    return std::nullopt;
  }

  // Step 2: every key is the input key its index names. The difference spans
  // the keys' region: its first and last differing byte, the popcount over all
  // of it, and the window the record model defines.
  static std::optional<ExactMismatch> check_keys(const UnitEvidence& evidence) {
    std::optional<std::size_t> first_position;
    std::uint64_t first_offset = 0;
    std::uint64_t last_offset = 0;
    std::uint64_t popcount = 0;
    for (std::size_t position = 0; position < spec::kKeys; ++position) {
      const std::uint32_t index = spec::load_le32(evidence.output, spec::index_offset(position));
      const std::uint64_t delta = spec::load_le64(evidence.output, spec::key_offset(position)) ^
                                  spec::load_le64(evidence.input, std::size_t{index} * 8);
      if (delta == 0) {
        continue;
      }
      if (!first_position.has_value()) {
        first_position = position;
        first_offset =
            spec::key_offset(position) + static_cast<std::size_t>(std::countr_zero(delta)) / 8U;
      }
      last_offset = spec::key_offset(position) +
                    (kWordBits - 1U - static_cast<std::size_t>(std::countl_zero(delta))) / 8U;
      popcount += static_cast<std::uint64_t>(std::popcount(delta));
    }
    if (!first_position.has_value()) {
      return std::nullopt;
    }
    Difference difference;
    difference.domain = Domain::kStored;
    difference.first_offset = first_offset;
    difference.last_offset = last_offset;
    difference.popcount = popcount;
    difference.window_offset = first_offset / kWordBytes * kWordBytes;
    const std::size_t end =
        std::min<std::size_t>(spec::kIndexRegionOffset, difference.window_offset + kMaxWindowBytes);
    for (std::size_t at = difference.window_offset; at < end; ++at) {
      const std::uint32_t index =
          spec::load_le32(evidence.output, spec::index_offset(at / spec::kKeyBytes));
      difference.expected.push_back(evidence.input[std::size_t{index} * 8 + at % 8]);
      difference.actual.push_back(evidence.output[at]);
    }
    const std::uint32_t named =
        spec::load_le32(evidence.output, spec::index_offset(*first_position));
    return ExactMismatch{"key differs from input key " + std::to_string(named) + " at position " +
                             std::to_string(*first_position),
                         std::move(difference), spec::kKeys};
  }

  // Step 3: the pairs strictly increase. The smallest position where one does
  // not follow the one before it.
  static std::optional<ExactMismatch> check_order(std::span<const std::byte> output) {
    spec::Entry before{spec::load_le64(output, spec::key_offset(0)),
                       spec::load_le32(output, spec::index_offset(0))};
    for (std::size_t position = 1; position < spec::kKeys; ++position) {
      const spec::Entry entry{spec::load_le64(output, spec::key_offset(position)),
                              spec::load_le32(output, spec::index_offset(position))};
      if (!spec::precedes(before, entry)) {
        return ExactMismatch{"order broken at position " + std::to_string(position), std::nullopt,
                             spec::kKeys};
      }
      before = entry;
    }
    return std::nullopt;
  }

  ScratchAllocator allocator_;
};

}  // namespace

std::unique_ptr<std::uint64_t[]> allocate_scratch(std::size_t words) {
  return std::unique_ptr<std::uint64_t[]>(new (std::nothrow) std::uint64_t[words]());
}

core::Result<std::unique_ptr<Oracle>, OracleError> make_sort_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan) {
  return make_sort_oracle(config, plan, allocate_scratch);
}

core::Result<std::unique_ptr<Oracle>, OracleError> make_sort_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan,
    ScratchAllocator allocator) {
  if (config.decomposition != 0 && !spec::is_chunk_count(config.decomposition)) {
    return OracleError{OracleFault::kConfiguration, "chunk count " +
                                                        std::to_string(config.decomposition) +
                                                        " is not one of 1, 2, 4, 8, 16, 32, 64"};
  }
  if (!spec::is_chunk_count(plan.decomposition)) {
    return OracleError{OracleFault::kConfiguration, "plan chunk count " +
                                                        std::to_string(plan.decomposition) +
                                                        " is not one of 1, 2, 4, 8, 16, 32, 64"};
  }
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    return OracleError{OracleFault::kConfiguration,
                       "plan sizes " + std::to_string(plan.input_bytes) + " and " +
                           std::to_string(plan.output_bytes) + " are not a sort unit's " +
                           std::to_string(spec::kInputBytes) + " and " +
                           std::to_string(spec::kOutputBytes)};
  }
  return std::unique_ptr<Oracle>(std::make_unique<SortOracle>(std::move(allocator)));
}

}  // namespace loadforge::verification::oracle
