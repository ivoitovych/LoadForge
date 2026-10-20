// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_RECORD_CODEC_HPP
#define LOADFORGE_VERIFICATION_RECORD_CODEC_HPP

// The provider of encode() and decode() (verification/record.hpp), and what a
// decoder needs beside them: which validity rule a record breaks, and the
// words for a decode error.
//
// THE ENCODING, FORMAT VERSION 1
// ------------------------------
// The contract fixes the first two bytes and leaves the rest to this file. It
// is written down here so a reader can be checked against it. Little-endian,
// every field fixed-width, in this order:
//
//   u8  version            kRecordFormatVersion
//   u8  kind               1 ExactBitCorruption, 2 NumericalVerificationFailure,
//                          3 VerificationNotPerformed
//   provenance, each field an OPTIONAL: a u8 presence flag, 0 or 1, and the
//   value only when it is 1 --
//     u32 logical_cpu, u32 physical_core, u32 numa_node,
//     u64 buffer_address, u64 buffer_offset, u64 iteration,
//     text workload, u32 workload_version, u64 seed,
//     u8 isa, u8 input_integrity, u32 + u32 duty (oracle, integrity)
//   then, by kind --
//   1: u8 side, text check, u32 line_bytes, optional difference:
//        u8 domain, u64 first_offset, u64 last_offset, u64 popcount,
//        u64 window_offset, bytes expected, bytes actual
//   2: text check, f64 residual, f64 tolerance, optional f64 condition_number,
//      optional u64 element
//   3: text what, text why, u64 blocks_compared
//
// where `text` and `bytes` are a u64 length and that many bytes, and f64 is
// the IEEE 754 bit pattern as a u64, so a NaN keeps its payload.
//
// WHY EVERY FIELD IS FIXED-WIDTH AND EVERY FLAG IS 0 OR 1
// -------------------------------------------------------
// So the encoding is canonical: each record has exactly one encoding, and
// decode() accepts exactly the encodings of valid records. Whatever bytes
// decode() accepts, encode() gives back byte for byte. That is the strongest
// form of the contract's "this function's part is never to guess" a decoder
// without a checksum of its own can have: a corrupted byte either produces an
// error or produces the record those exact bytes encode, never one rounded,
// clamped or defaulted from them. A presence flag of 7 read as "present"
// would be a guess, and re-encoding it would not give the bytes back.
//
// HOW DECODE() ANSWERS
// --------------------
// It reads in byte order and reports the first problem it meets:
//
//   kEmpty      no bytes.
//   kVersion    byte 0 is not kRecordFormatVersion -- even when it is the
//               only byte: an unknown version says nothing about lengths.
//   kTruncated  the bytes end inside a field (a single byte of version 1
//               included: the kind is missing).
//   kKind       byte 1 is none of the three kinds.
//   kInvalid    a presence flag that is neither 0 nor 1, a text longer than
//               kMaxTextBytes or a window longer than kMaxWindowBytes -- each
//               refused where its length is read, so no length from a corrupt
//               channel is ever trusted to size an allocation -- and, once
//               the record is complete, the first validity rule it breaks
//               (broken_rule()).
//   kTrailing   bytes left after a complete, valid record.
//
// The detail names the field and the byte offset where the problem was found.
//
// THREAD-SAFETY: pure functions.

#include <optional>
#include <string>

#include "verification/record.hpp"

namespace loadforge::verification {

/// The first validity rule of record.hpp's decode() that `record` breaks, in
/// words, or nothing when it is valid. Agrees with is_valid() on every record:
/// has_value() exactly when !is_valid(record).
[[nodiscard]] std::optional<std::string> broken_rule(const ErrorRecord& record);

/// The fault and its detail, as one line: "truncated: field 'seed' at byte 40
/// needs 8 bytes, 3 remain".
[[nodiscard]] std::string describe(const DecodeError& error);

}  // namespace loadforge::verification

#endif
