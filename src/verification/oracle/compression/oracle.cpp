// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/compression/oracle.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/compression/decoder.hpp"
#include "verification/record.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::compression {
namespace {

namespace spec = workloads::compression;

constexpr std::size_t kOutputBytes = spec::output_bytes_for(spec::kInputBytes);
constexpr std::size_t kBlockBound = spec::compress_bound(spec::kInputBytes);
constexpr std::string_view kWhat = "the compressed output, decoded, against the input";

/// An ExactMismatch with no difference: the output is wrong and has no bits
/// to compare. `at` is the byte of the unit's output.
ExactMismatch malformed(std::size_t at, std::string_view rule) {
  std::string check = "compressed output malformed at byte ";
  check += std::to_string(at);
  check += ": ";
  check += rule;
  return ExactMismatch{std::move(check), std::nullopt, 1};
}

/// The difference between what the block decoded to and the input, or none.
std::optional<Difference> derived_difference(std::span<const std::byte> expected,
                                             std::span<const std::byte> actual) {
  const std::size_t size = expected.size();
  std::size_t first = size;
  std::size_t last = 0;
  std::uint64_t popcount = 0;
  for (std::size_t at = 0; at < size; ++at) {
    const auto delta = std::to_integer<unsigned>(expected[at] ^ actual[at]);
    if (delta != 0) {
      first = std::min(first, at);
      last = at;
      popcount += static_cast<std::uint64_t>(std::popcount(delta));
    }
  }
  if (popcount == 0) {
    return std::nullopt;
  }
  Difference difference;
  difference.domain = Domain::kDerived;
  difference.first_offset = first;
  difference.last_offset = last;
  difference.popcount = popcount;
  difference.window_offset = first / kWordBytes * kWordBytes;
  const auto from = static_cast<std::ptrdiff_t>(difference.window_offset);
  const auto to = from + static_cast<std::ptrdiff_t>(
                             std::min(kMaxWindowBytes, size - difference.window_offset));
  difference.expected.assign(expected.begin() + from, expected.begin() + to);
  difference.actual.assign(actual.begin() + from, actual.begin() + to);
  return difference;
}

/// The first byte of the padding after the block that is not zero.
std::optional<std::size_t> nonzero_padding(std::span<const std::byte> output, std::size_t from) {
  const auto found = std::find_if(output.begin() + static_cast<std::ptrdiff_t>(from), output.end(),
                                  [](std::byte value) { return value != std::byte{0}; });
  if (found == output.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - output.begin());
}

OracleError refused(std::string detail) {
  return OracleError{OracleFault::kConfiguration, std::move(detail)};
}

}  // namespace

std::unique_ptr<std::byte[]> heap_scratch(std::size_t bytes) noexcept {
  return std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[bytes]);
}

CompressionOracle::CompressionOracle(ScratchSource scratch) noexcept : scratch_(scratch) {}

std::string_view CompressionOracle::workload() const noexcept { return kWorkload; }

Contract CompressionOracle::contract() const noexcept { return Contract::kExact; }

Verdict CompressionOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != kOutputBytes) {
    std::string why = "the evidence is ";
    why += std::to_string(evidence.input.size());
    why += " bytes of input and ";
    why += std::to_string(evidence.output.size());
    why += " of output; a compression unit is ";
    why += std::to_string(spec::kInputBytes);
    why += " and ";
    why += std::to_string(kOutputBytes);
    return NotChecked{std::string(kWhat), std::move(why), 0};
  }
  const std::span<const std::byte> output = evidence.output;
  if (spec::load_le32(output, 0) != spec::kMagic) {
    return malformed(0, "the magic is not \"LFZ1\"");
  }
  const std::size_t length = spec::load_le32(output, 4);
  if (length > kBlockBound) {
    return malformed(4, "the block length exceeds compress_bound(65536) = 65809");
  }
  const std::size_t block_end = spec::kHeaderBytes + length;
  if (const auto padding = nonzero_padding(output, block_end)) {
    return malformed(*padding, "the padding after the block is not zero");
  }
  const std::unique_ptr<std::byte[]> scratch = scratch_(spec::kInputBytes);
  if (scratch == nullptr) {
    return NotChecked{std::string(kWhat), "no scratch for the decoded bytes could be had", 0};
  }
  const std::span<std::byte> decoded(scratch.get(), spec::kInputBytes);
  const auto summary = decode_block(output.subspan(spec::kHeaderBytes, length), decoded);
  if (!summary.has_value()) {
    const Malformation& broken = summary.error();
    std::string rule = std::string(describe(broken.rule));
    rule += " (";
    rule += std::to_string(broken.decoded);
    rule += " bytes decoded)";
    return malformed(spec::kHeaderBytes + broken.at, rule);
  }
  if (auto difference = derived_difference(evidence.input, decoded)) {
    return ExactMismatch{std::string(kWhat), std::move(difference), 1};
  }
  return Agreement{1};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(const workloads::WorkloadConfig& config,
                                                          const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != kOutputBytes) {
    return refused("a compression unit is " + std::to_string(spec::kInputBytes) + " bytes in and " +
                   std::to_string(kOutputBytes) + " out; the plan says " +
                   std::to_string(plan.input_bytes) + " and " + std::to_string(plan.output_bytes));
  }
  if (plan.decomposition != 1 || config.decomposition > 1) {
    return refused(
        "the compression workload has no decomposition: the plan's must be 1 and the "
        "configured one 0 or 1, not " +
        std::to_string(plan.decomposition) + " and " + std::to_string(config.decomposition));
  }
  return std::unique_ptr<Oracle>(std::make_unique<CompressionOracle>());
}

}  // namespace loadforge::verification::oracle::compression
