// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/crc32.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::core {
namespace {

constexpr std::uint32_t kPolynomial = 0xEDB88320U;  ///< 0x04C11DB7 reflected.
constexpr std::size_t kTableSize = 256;

/// The byte table, computed at compile time rather than copied from a
/// reference: a 256-entry literal is 256 chances for a typo that the check
/// value would catch only if the wrong entry happens to be used by "123456789".
consteval std::array<std::uint32_t, kTableSize> make_table() {
  std::array<std::uint32_t, kTableSize> table{};
  for (std::size_t index = 0; index < kTableSize; ++index) {
    auto entry = static_cast<std::uint32_t>(index);
    for (int bit = 0; bit < 8; ++bit) {
      entry = (entry & 1U) != 0 ? (entry >> 1U) ^ kPolynomial : entry >> 1U;
    }
    table.at(index) = entry;
  }
  return table;
}

constexpr std::array<std::uint32_t, kTableSize> kTable = make_table();

}  // namespace

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::byte> bytes) noexcept {
  // The running value is kept inverted between calls, as every CRC-32
  // implementation does, so that the empty input has CRC 0 and a continuation
  // is the same arithmetic as a fresh start.
  std::uint32_t running = ~crc;
  for (const std::byte byte : bytes) {
    const auto index =
        static_cast<std::size_t>((running ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU);
    running = kTable.at(index) ^ (running >> 8U);
  }
  return ~running;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept { return crc32_update(0, bytes); }

}  // namespace loadforge::core
