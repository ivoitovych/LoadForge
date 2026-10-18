// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_CRC32_HPP
#define LOADFORGE_CORE_CRC32_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::core {

/// CRC-32 as IEEE 802.3, zlib and PNG define it: polynomial 0xEDB88320 in
/// reflected form, initial value and final XOR both all-ones. The check value
/// every implementation of this CRC agrees on is crc32("123456789") ==
/// 0xCBF43926, and the test pins it, because a CRC that is merely
/// self-consistent would still protect a record pipe -- but could not be
/// checked against any outside reading of the same bytes (F21).
///
/// WHY A CRC AND NOT A HASH
/// ------------------------
/// The record pipe carries a worker's findings to the controller, and the
/// doctrine is that garbage in must never become a hardware verdict out. A
/// CRC-32 detects every single-bit error, every odd number of bit errors,
/// every burst up to 32 bits, and misses a random corruption one time in four
/// billion. That is the right tool against a truncated write, a byte lost to
/// a bug on either side, or a reader that fell out of step with the frames;
/// it is not a defence against an adversary, and nothing on this pipe has one.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

/// Continues a CRC over more bytes: crc32_update(crc32(a), b) == crc32(a ++ b)
/// for any split of a stream, which is what lets a frame's checksum cover its
/// header and its payload without first copying them into one buffer.
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc,
                                         std::span<const std::byte> bytes) noexcept;

}  // namespace loadforge::core

#endif
