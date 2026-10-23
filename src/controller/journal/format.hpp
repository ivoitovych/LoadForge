// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_JOURNAL_FORMAT_HPP
#define LOADFORGE_CONTROLLER_JOURNAL_FORMAT_HPP

// The journal's bytes, and only its bytes: no storage, no failure but "these
// bytes are not a record". journal.cpp wires this to a Storage; the tests cut
// and corrupt it directly.
//
// A RECORD
// --------
//   offset  size  field
//   0       4     length of the payload, little-endian
//   4       4     CRC-32 (core::crc32) over those four length bytes and then
//                 the payload, little-endian
//   8       n     payload
//
// The check covers the length as well as the payload, so a flipped length bit
// is a failed check and not a record that reads a different number of bytes
// and happens to end somewhere plausible. A length past kMaxPayloadBytes is
// refused before any byte is trusted to it.
//
// A length that reaches past the bytes there are cannot be checked, because
// the check needs every byte. What can be told is whether the bytes there
// could be the start of a record of that length: a write cut short leaves
// such bytes, and the ending is a torn tail. A flipped length bit that leaves
// bytes no such record starts with (a type that is not an entry, a length
// other than the one the entry's own label sizes give, one the entry cannot
// have) is a failed check, and the ending is corrupt. A flipped bit in a
// length that the bytes there do not contradict reads as a torn tail: no
// reader can tell it from a cut, and a cut must never read as corrupt.
//
// A PAYLOAD
// ---------
// The first byte is the type. kTerminator is the whole payload of the record
// that closes a journal. kEntry is followed by, all little-endian:
//
//   8   elapsed_ns, not negative
//   1   presence bits: 0 last_verified_unit, 1 temperature_c,
//       2 package_power_w, 3 frequency_hz, 4 ecc_corrected, 5 ecc_uncorrected;
//       the other two bits are zero
//   1+n phase: its length, then its bytes
//   1+n workload: likewise
//   48  the six optionals in the order above, 8 bytes each, a double as its
//       IEEE-754 bits; the bytes of an absent one are zero, and a payload
//       with a non-zero byte there is not an entry
//   4   workers_alive
//   4   workers_stale
//
// Fixed width and no varints: a record is about 200 bytes, an entry is
// written every few seconds, and a format a reader can check by counting is
// worth more than the bytes a compact one would save.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "controller/journal/journal.hpp"

namespace loadforge::journal::format {

inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kMaxPayloadBytes = 512;
inline constexpr std::byte kEntry{1};
inline constexpr std::byte kTerminator{2};

/// The record that carries `payload`: header, then payload.
[[nodiscard]] std::vector<std::byte> frame(std::span<const std::byte> payload);

/// The payload of `entry`. The caller has checked the labels.
[[nodiscard]] std::vector<std::byte> encode_entry(const Entry& entry);

/// The payload of the terminator record.
[[nodiscard]] std::vector<std::byte> terminator_payload();

/// The entry a kEntry payload carries, or nothing when the payload is not
/// exactly one: short, long, a label past kMaxLabelBytes, a reserved bit set,
/// a non-zero byte behind a clear presence bit, a negative elapsed time.
///
/// The order of elapsed times from one entry to the next is not checked here
/// or by scan(): this format promises none, and a record is checked alone.
[[nodiscard]] std::optional<Entry> decode_entry(std::span<const std::byte> payload);

/// What the bytes of a whole journal say. Never fails: damage is an Ending.
[[nodiscard]] Replay scan(std::span<const std::byte> bytes);

}  // namespace loadforge::journal::format

#endif
