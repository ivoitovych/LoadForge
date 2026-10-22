// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/integrity/control_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/crc32.hpp"
#include "verification/integrity/locking.hpp"

namespace loadforge::verification::integrity {
namespace {

using Bytes = std::array<std::byte, kCopyBytes>;
constexpr std::size_t kWordsBytes = kControlWords * 8;

/// The copy's memory, read and written only through volatile, so that every
/// vote is a read of the memory and every write a store to it.
std::span<volatile std::byte, kCopyBytes> view(const platform::Mapping& mapping) noexcept {
  return std::span<volatile std::byte, kCopyBytes>(
      static_cast<volatile std::byte*>(mapping.address()), kCopyBytes);
}

/// One copy as it is stored: the words little-endian, then their CRC-32.
Bytes encode(const ControlWords& words) noexcept {
  Bytes bytes{};
  for (std::size_t index = 0; index < kControlWords; ++index) {
    for (std::size_t byte = 0; byte < 8; ++byte) {
      bytes.at(index * 8 + byte) = static_cast<std::byte>(words.at(index) >> (8 * byte));
    }
  }
  const std::uint32_t crc = core::crc32(std::span<const std::byte>(bytes).first(kWordsBytes));
  for (std::size_t byte = 0; byte < 4; ++byte) {
    bytes.at(kWordsBytes + byte) = static_cast<std::byte>(crc >> (8 * byte));
  }
  return bytes;
}

/// One copy as read back: its words, and whether its checksum holds.
struct Decoded {
  ControlWords words{};
  bool verified = false;
};

Decoded decode(std::span<const volatile std::byte, kCopyBytes> stored) noexcept {
  Bytes bytes{};
  for (std::size_t at = 0; at < kCopyBytes; ++at) {
    bytes.at(at) = stored[at];
  }
  Decoded decoded;
  for (std::size_t index = 0; index < kControlWords; ++index) {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
      value |= std::to_integer<std::uint64_t>(bytes.at(index * 8 + byte)) << (8 * byte);
    }
    decoded.words.at(index) = value;
  }
  std::uint32_t crc = 0;
  for (std::size_t byte = 0; byte < 4; ++byte) {
    crc |= std::to_integer<std::uint32_t>(bytes.at(kWordsBytes + byte)) << (8 * byte);
  }
  decoded.verified = crc == core::crc32(std::span<const std::byte>(bytes).first(kWordsBytes));
  return decoded;
}

}  // namespace

std::string_view describe(VoteFault fault) noexcept {
  switch (fault) {
    case VoteFault::kDisagreement:
      return "the verified copies of the control state disagree";
    case VoteFault::kTooFewVerified:
      return "fewer than two copies of the control state pass their checksums";
  }
  return "unrecognised vote fault";
}

std::string describe(const VoteFailure& failure) {
  std::string text(describe(failure.fault));
  text += " (";
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    text += copy == 0 ? "copy " : ", copy ";
    text += std::to_string(copy);
    text += failure.verified.at(copy) ? " verified" : " fails its checksum";
  }
  text += ")";
  return text;
}

core::Result<ControlState, ControlRefused> ControlState::create(
    platform::Syscalls& syscalls, const ControlWords& initial, std::span<const std::uint32_t> nodes,
    LockPolicy policy) {
  platform::Memory memory(syscalls);
  std::vector<platform::Mapping> copies;
  copies.reserve(kCopies);
  LockOutcome outcome =
      policy == LockPolicy::kNone ? LockOutcome::kNotLocked : LockOutcome::kLocked;
  std::string detail;
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    platform::MappingRequest request;
    request.bytes = kCopyBytes;
    request.populate = true;
    if (!nodes.empty()) {
      request.node = nodes[copy % nodes.size()];
    }
    auto mapped = map_by_policy(memory, request, policy);
    if (!mapped.has_value()) {
      // The copies already mapped are unmapped by the vector's destructor.
      return ControlRefused{copy, mapped.error()};
    }
    if (mapped.value().outcome == LockOutcome::kFellBack && outcome != LockOutcome::kFellBack) {
      outcome = LockOutcome::kFellBack;
      detail = mapped.value().detail;
    }
    copies.push_back(std::move(mapped).value().mapping);
  }
  ControlState state(std::move(copies), outcome, std::move(detail));
  state.write(initial);
  return state;
}

ControlState::ControlState(std::vector<platform::Mapping> copies, LockOutcome outcome,
                           std::string detail) noexcept
    : copies_(std::move(copies)), outcome_(outcome), detail_(std::move(detail)) {}

void ControlState::write_copy(std::size_t copy, const ControlWords& words) noexcept {
  // Encoded afresh for every copy: three encodings, three checksums, three
  // sequences of stores.
  const Bytes bytes = encode(words);
  const std::span<volatile std::byte, kCopyBytes> stored = view(copies_[copy]);
  for (std::size_t at = 0; at < kCopyBytes; ++at) {
    stored[at] = bytes.at(at);
  }
}

void ControlState::write(const ControlWords& words) noexcept {
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    write_copy(copy, words);
  }
}

core::Result<ControlWords, VoteFailure> ControlState::read() {
  std::array<Decoded, kCopies> decoded{};
  VoteFailure failure;
  std::size_t verified = 0;
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    decoded.at(copy) = decode(view(copies_[copy]));
    failure.verified.at(copy) = decoded.at(copy).verified;
    verified += decoded.at(copy).verified ? 1U : 0U;
  }
  if (verified < 2) {
    failure.fault = VoteFault::kTooFewVerified;
    return failure;
  }

  // A value held by two verified copies. With three copies, if any two
  // verified ones agree, copy 0 or copy 1 is one of them.
  std::optional<ControlWords> decided;
  for (std::size_t candidate = 0; candidate < 2 && !decided.has_value(); ++candidate) {
    if (!decoded.at(candidate).verified) {
      continue;
    }
    for (std::size_t other = candidate + 1; other < kCopies; ++other) {
      if (decoded.at(other).verified && decoded.at(other).words == decoded.at(candidate).words) {
        decided = decoded.at(candidate).words;
      }
    }
  }
  if (!decided.has_value()) {
    failure.fault = VoteFault::kDisagreement;
    return failure;
  }

  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    if (!decoded.at(copy).verified || decoded.at(copy).words != *decided) {
      write_copy(copy, *decided);
      ++repairs_;
    }
  }
  return *decided;
}

}  // namespace loadforge::verification::integrity
