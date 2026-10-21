// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/checksum/crc64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::verification {
namespace {

constexpr std::uint64_t kPolynomial = 0xC96C5795D7870F42ULL;  ///< 0x42F0E1EBA9EA3693 reflected.
constexpr std::size_t kTableSize = 256;
constexpr std::size_t kSlices = 8;

using Tables = std::array<std::array<std::uint64_t, kTableSize>, kSlices>;

/// Slicing-by-8: table k gives the CRC contribution of a byte followed by k
/// zero bytes, so eight bytes are folded in with eight lookups and no loop
/// over bits. Computed at compile time from the polynomial rather than copied:
/// 2048 literal entries would be 2048 chances for a typo that the check value
/// would catch only if "123456789" happened to use the wrong one.
consteval Tables make_tables() {
  Tables tables{};
  for (std::size_t index = 0; index < kTableSize; ++index) {
    auto entry = static_cast<std::uint64_t>(index);
    for (int bit = 0; bit < 8; ++bit) {
      entry = (entry & 1U) != 0 ? (entry >> 1U) ^ kPolynomial : entry >> 1U;
    }
    tables[0].at(index) = entry;
  }
  for (std::size_t slice = 1; slice < kSlices; ++slice) {
    for (std::size_t index = 0; index < kTableSize; ++index) {
      const std::uint64_t previous = tables.at(slice - 1).at(index);
      tables.at(slice).at(index) = (previous >> 8U) ^ tables[0].at(previous & 0xFFU);
    }
  }
  return tables;
}

constexpr Tables kTables = make_tables();

}  // namespace

std::uint64_t crc64_update(std::uint64_t crc, std::span<const std::byte> bytes) noexcept {
  // Kept inverted between calls, so the empty input has CRC 0 and a
  // continuation is the same arithmetic as a fresh start.
  std::uint64_t running = ~crc;
  std::size_t at = 0;
  for (; at + kSlices <= bytes.size(); at += kSlices) {
    std::uint64_t word = 0;
    for (std::size_t byte = 0; byte < kSlices; ++byte) {
      word |= std::to_integer<std::uint64_t>(bytes[at + byte]) << (8U * byte);
    }
    running ^= word;
    std::uint64_t folded = 0;
    for (std::size_t byte = 0; byte < kSlices; ++byte) {
      folded ^= kTables.at(kSlices - 1 - byte).at((running >> (8U * byte)) & 0xFFU);
    }
    running = folded;
  }
  for (; at < bytes.size(); ++at) {
    const auto index = (running ^ std::to_integer<std::uint64_t>(bytes[at])) & 0xFFU;
    running = kTables[0].at(index) ^ (running >> 8U);
  }
  return ~running;
}

std::uint64_t crc64(std::span<const std::byte> bytes) noexcept { return crc64_update(0, bytes); }

}  // namespace loadforge::verification
