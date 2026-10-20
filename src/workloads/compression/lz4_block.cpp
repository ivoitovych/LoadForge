// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/compression/lz4_block.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression {
namespace {

/// A length that does not fit its nibble: kLengthEscape in the nibble, then
/// `rest` = length - kLengthEscape as bytes of 255 and a final byte below 255
/// (which may be 0: "the zero must be output").
std::size_t write_length(std::span<std::byte> block, std::size_t at, std::size_t rest) noexcept {
  for (; rest >= 255; rest -= 255) {
    block[at++] = std::byte{0xFF};
  }
  block[at++] = static_cast<std::byte>(rest);
  return at;
}

/// The table holds positions as host-order 32-bit words through memcpy, so a
/// scratch span with any alignment serves. The order never leaves the table.
std::uint32_t table_get(std::span<const std::byte> table, std::size_t slot) noexcept {
  std::uint32_t position = 0;
  std::memcpy(&position, table.subspan(slot * 4, 4).data(), 4);
  return position;
}

void table_put(std::span<std::byte> table, std::size_t slot, std::uint32_t position) noexcept {
  std::memcpy(table.subspan(slot * 4, 4).data(), &position, 4);
}

}  // namespace

// The offset and the match length are both byte counts, in the order the
// format writes them.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::size_t write_sequence(std::span<std::byte> block, std::size_t at,
                           std::span<const std::byte> literals, std::size_t offset,
                           std::size_t match_length) noexcept {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  const std::size_t literal_count = literals.size();
  const std::size_t match_code = match_length == 0 ? 0 : match_length - kMinMatch;
  const std::size_t token =
      (std::min(literal_count, kLengthEscape) << 4U) | std::min(match_code, kLengthEscape);
  block[at++] = static_cast<std::byte>(token);
  if (literal_count >= kLengthEscape) {
    at = write_length(block, at, literal_count - kLengthEscape);
  }
  std::ranges::copy(literals, block.subspan(at).begin());
  at += literal_count;
  if (match_length == 0) {
    return at;
  }
  block[at++] = static_cast<std::byte>(offset & 0xFFU);
  block[at++] = static_cast<std::byte>(offset >> 8U);
  if (match_code >= kLengthEscape) {
    at = write_length(block, at, match_code - kLengthEscape);
  }
  return at;
}

// The block and the table are both writable bytes; the block comes first, as
// the output does in every call of this shape.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::size_t compress_block(std::span<const std::byte> input, std::span<std::byte> block,
                           std::span<std::byte> table) noexcept {
  const std::size_t size = input.size();
  std::size_t anchor = 0;
  std::size_t written = 0;
  if (size > kMatchFindLimit) {
    std::ranges::fill(table.first(kTableBytes), std::byte{0});
    // The last position a match may start at, and the position no match may
    // reach: the format's end-of-block rules 3 and 2.
    const std::size_t last_start = size - kMatchFindLimit;
    const std::size_t match_end = size - kLastLiterals;
    std::size_t position = 0;
    while (position <= last_start) {
      const std::uint32_t four_bytes = load_le32(input, position);
      const std::size_t slot = hash_slot(four_bytes);
      const std::size_t candidate = table_get(table, slot);
      table_put(table, slot, static_cast<std::uint32_t>(position));
      // A cleared table offers position 0 to every slot, which is only a
      // candidate once something lies before it; a table entry is otherwise
      // always a position below this one.
      if (candidate < position && position - candidate <= kMaxOffset &&
          load_le32(input, candidate) == four_bytes) {
        std::size_t length = kMinMatch;
        while (position + length < match_end &&
               input[candidate + length] == input[position + length]) {
          ++length;
        }
        written = write_sequence(block, written, input.subspan(anchor, position - anchor),
                                 position - candidate, length);
        position += length;
        anchor = position;
      } else {
        ++position;
      }
    }
  }
  return write_sequence(block, written, input.subspan(anchor), 0, 0);
}

}  // namespace loadforge::workloads::compression
