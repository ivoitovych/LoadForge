// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/frames.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "verification/runner.hpp"

namespace loadforge::worker {
namespace {

constexpr std::size_t kCounters = 7;

std::array<std::uint64_t, kCounters> counters_of(const verification::Tally& tally) noexcept {
  return {tally.units_computed,  tally.oracle_checked,       tally.oracle_blocks,
          tally.oracle_failed,   tally.oracle_not_performed, tally.integrity_checked,
          tally.integrity_failed};
}

}  // namespace

std::vector<std::byte> encode_tally(const verification::Tally& tally) {
  std::vector<std::byte> bytes;
  bytes.reserve(kTallyFrameBytes);
  bytes.push_back(static_cast<std::byte>(kTallyFormatVersion));
  for (const std::uint64_t counter : counters_of(tally)) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      bytes.push_back(static_cast<std::byte>((counter >> shift) & 0xFFU));
    }
  }
  return bytes;
}

core::Result<verification::Tally, std::string> decode_tally(std::span<const std::byte> bytes) {
  if (bytes.size() != kTallyFrameBytes) {
    return "tally frame: " + std::to_string(bytes.size()) + " byte(s), expected " +
           std::to_string(kTallyFrameBytes);
  }
  if (bytes.front() != static_cast<std::byte>(kTallyFormatVersion)) {
    return "tally frame: unknown format version " +
           std::to_string(std::to_integer<unsigned>(bytes.front()));
  }
  std::array<std::uint64_t, kCounters> counters{};
  for (std::size_t index = 0; index < kCounters; ++index) {
    for (std::size_t byte = 0; byte < 8; ++byte) {
      const auto value = std::to_integer<std::uint64_t>(bytes[1 + index * 8 + byte]);
      counters[index] |= value << (8 * byte);
    }
  }
  verification::Tally tally;
  tally.units_computed = counters[0];
  tally.oracle_checked = counters[1];
  tally.oracle_blocks = counters[2];
  tally.oracle_failed = counters[3];
  tally.oracle_not_performed = counters[4];
  tally.integrity_checked = counters[5];
  tally.integrity_failed = counters[6];
  return tally;
}

}  // namespace loadforge::worker
