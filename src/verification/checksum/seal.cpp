// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/checksum/seal.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "verification/checksum/compare.hpp"
#include "verification/checksum/crc64.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {

namespace {

/// "crc64 0x.. over N bytes", the CRC as sixteen hexadecimal digits so two
/// seals line up in a report.
std::string words(const Seal& seal) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string crc(16, '0');
  for (std::size_t digit = 0; digit < crc.size(); ++digit) {
    crc[crc.size() - 1 - digit] = kDigits[(seal.crc >> (4U * digit)) & 0x0FU];
  }
  return "crc64 0x" + crc + " over " + std::to_string(seal.bytes) + " bytes";
}

}  // namespace

bool operator==(const Seal& lhs, const Seal& rhs) noexcept {
  return std::tie(lhs.bytes, lhs.crc) == std::tie(rhs.bytes, rhs.crc);
}

Seal seal(std::span<const std::byte> bytes) noexcept { return Seal{bytes.size(), crc64(bytes)}; }

void RunningChecksum::update(std::span<const std::byte> bytes) noexcept {
  crc_ = crc64_update(crc_, bytes);
  bytes_ += bytes.size();
}

Seal RunningChecksum::seal() const noexcept { return Seal{bytes_, crc_}; }

std::string_view describe(SealOutcome outcome) noexcept {
  switch (outcome) {
    case SealOutcome::kIntact:
      return "intact";
    case SealOutcome::kCorrupt:
      return "corrupt";
    case SealOutcome::kNothingCompared:
      return "nothing compared";
    case SealOutcome::kNotSealed:
      return "not sealed";
  }
  return "unrecognised seal outcome";
}

bool operator==(const SealCheck& lhs, const SealCheck& rhs) noexcept {
  return std::tie(lhs.outcome, lhs.sealed, lhs.found) ==
         std::tie(rhs.outcome, rhs.sealed, rhs.found);
}

SealCheck verify_seal(std::span<const std::byte> bytes,
                      const std::optional<Seal>& sealed) noexcept {
  if (!sealed.has_value()) {
    return SealCheck{SealOutcome::kNotSealed, std::nullopt, std::nullopt};
  }
  if (bytes.empty()) {
    return SealCheck{SealOutcome::kNothingCompared, sealed, std::nullopt};
  }
  const Seal found = seal(bytes);
  return SealCheck{found == *sealed ? SealOutcome::kIntact : SealOutcome::kCorrupt, sealed, found};
}

std::optional<ErrorRecord> to_record(const SealCheck& check, const Subject& subject) {
  if (check.outcome == SealOutcome::kIntact) {
    return std::nullopt;
  }
  if (check.outcome == SealOutcome::kNotSealed) {
    return not_performed(subject,
                         "no checksum was sealed when the bytes were generated, so there is "
                         "nothing to check them against");
  }
  if (check.outcome == SealOutcome::kNothingCompared) {
    std::string why =
        "there were no bytes to check against the checksum sealed at their generation (";
    why += words(*check.sealed);
    why += ")";
    return not_performed(subject, std::move(why));
  }
  ExactBitCorruption record;
  record.provenance = subject.provenance;
  record.side = subject.side;
  record.check = subject.check + " -- " + words(*check.found) + ", sealed at generation as " +
                 words(*check.sealed) +
                 ": the bytes or the checksum changed since; a checksum cannot say which bits";
  record.line_bytes = subject.line_bytes;
  return record;
}

}  // namespace loadforge::verification
