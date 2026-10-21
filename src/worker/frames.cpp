// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/frames.hpp"

#include <algorithm>
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

std::vector<std::byte> encode_memory(const verification::MemoryLock& lock) {
  std::vector<std::byte> bytes;
  bytes.push_back(static_cast<std::byte>(kMemoryFormatVersion));
  bytes.push_back(static_cast<std::byte>(lock.policy));
  bytes.push_back(static_cast<std::byte>(lock.outcome));
  const std::size_t kept = std::min(lock.detail.size(), kMaxMemoryDetailBytes);
  for (std::size_t at = 0; at < kept; ++at) {
    bytes.push_back(static_cast<std::byte>(lock.detail[at]));
  }
  return bytes;
}

core::Result<verification::MemoryLock, std::string> decode_memory(
    std::span<const std::byte> bytes) {
  constexpr std::size_t kHeader = 3;
  if (bytes.size() < kHeader) {
    return "memory frame: " + std::to_string(bytes.size()) + " byte(s), expected at least " +
           std::to_string(kHeader);
  }
  if (bytes[0] != static_cast<std::byte>(kMemoryFormatVersion)) {
    return "memory frame: unknown format version " +
           std::to_string(std::to_integer<unsigned>(bytes[0]));
  }
  const auto policy = std::to_integer<unsigned>(bytes[1]);
  if (policy > static_cast<unsigned>(verification::LockPolicy::kNone)) {
    return "memory frame: unknown lock policy " + std::to_string(policy);
  }
  const auto outcome = std::to_integer<unsigned>(bytes[2]);
  if (outcome > static_cast<unsigned>(verification::LockOutcome::kFellBack)) {
    return "memory frame: unknown lock outcome " + std::to_string(outcome);
  }
  verification::MemoryLock lock;
  lock.policy = static_cast<verification::LockPolicy>(policy);
  lock.outcome = static_cast<verification::LockOutcome>(outcome);
  const std::size_t detail = bytes.size() - kHeader;
  if (detail > kMaxMemoryDetailBytes) {
    return "memory frame: a detail of " + std::to_string(detail) + " bytes, at most " +
           std::to_string(kMaxMemoryDetailBytes);
  }
  if (detail != 0 && lock.outcome != verification::LockOutcome::kFellBack) {
    return std::string{"memory frame: a detail on an outcome that has none"};
  }
  for (std::size_t at = kHeader; at < bytes.size(); ++at) {
    lock.detail.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[at])));
  }
  return lock;
}

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
  verification::Tally tally;
  const std::array<std::uint64_t*, kCounters> fields{
      &tally.units_computed,  &tally.oracle_checked,       &tally.oracle_blocks,
      &tally.oracle_failed,   &tally.oracle_not_performed, &tally.integrity_checked,
      &tally.integrity_failed};
  // Read front to back: a cursor, not an index, so that no byte is addressed
  // by arithmetic the compiler cannot see the bound of.
  auto cursor = bytes.begin() + 1;
  for (std::uint64_t* field : fields) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      value |= std::to_integer<std::uint64_t>(*cursor) << shift;
      ++cursor;
    }
    *field = value;
  }
  return tally;
}

}  // namespace loadforge::worker
