// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_CHECKSUM_CRC64_HPP
#define LOADFORGE_VERIFICATION_CHECKSUM_CRC64_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::verification {

/// CRC-64 as the .xz container defines it (CRC-64/XZ, the ECMA-182 polynomial
/// 0x42F0E1EBA9EA3693 in reflected form, 0xC96C5795D7870F42; initial value and
/// final XOR all-ones). Its check value is crc64("123456789") ==
/// 0x995DC9BBDF1939FA, and every value the tests pin was read from xz itself
/// (`xz --robot --list -vv`), not from a table of published constants (F21).
///
/// WHY THIS CHECKSUM GUARDS INPUT THAT CANNOT BE REGENERATED
/// ---------------------------------------------------------
/// verification.md §4a rule 2: where regeneration is impractical, a checksum
/// taken at generation and verified before use narrows the window in which a
/// corruption goes unseen. What it must catch is what memory does: a flipped
/// bit, a few bits in one word, a burst along a line. A CRC with this
/// polynomial detects every single-bit error, every odd number of bit errors
/// and every burst of up to 64 bits, whatever the length, and misses a random
/// corruption one time in 2^64 -- guarantees a 64-bit hash of the same width
/// does not make, and a 32-bit CRC makes only for messages far shorter than a
/// working set. It is no defence against an adversary; nothing here has one.
[[nodiscard]] std::uint64_t crc64(std::span<const std::byte> bytes) noexcept;

/// Continues a CRC over more bytes: crc64_update(crc64(a), b) == crc64(a ++ b)
/// for every split of a stream, so input generated in pieces is sealed as it
/// is written.
[[nodiscard]] std::uint64_t crc64_update(std::uint64_t crc,
                                         std::span<const std::byte> bytes) noexcept;

}  // namespace loadforge::verification

#endif
