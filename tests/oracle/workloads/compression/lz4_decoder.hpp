// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_COMPRESSION_LZ4_DECODER_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_COMPRESSION_LZ4_DECODER_HPP

// The compression workload's test decoder: an LZ4 block decoder written from
// the block-format document (lz4/doc/lz4_Block_format.md, revision of
// 2022-07-31, the one lz4 1.9.4 ships) and from spec.hpp, never from the
// compressor in src/workloads/compression/, and checked against blocks the
// real lz4 tool made (tests/fixtures/lz4-workload/, F21).
//
// It is not the oracle. The oracle is another stream's, written from the same
// two documents; this is the workload's own second reading of them, so that
// the workload's tests do not take the compressor's word for what it wrote.
//
// STRICT, BECAUSE THE FORMAT ALLOWS IT TO BE
// ------------------------------------------
// "When a block does not respect these end conditions, a conformant decoder
// is allowed to reject the block as incorrect." This one does, so that a
// compressor that broke an end-of-block rule fails here rather than in some
// historical decoder in the field. Each way a block can be wrong is its own
// Breach, with the byte of the block at which it was seen, so a T5 test can
// say which was found and where.
//
// TOTAL: any bytes are a block. Every length is checked against what is left
// of the block and of the capacity before it is used, in std::size_t, and a
// length that would pass either is a breach, never a read or write outside.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression::testing {

/// How a block breaks the format.
enum class Breach : std::uint8_t {
  kNone,               ///< a block that decodes
  kNoToken,            ///< the block is empty: not even the final token
  kTruncatedLength,    ///< a length said "more bytes follow" and none did
  kTruncatedLiterals,  ///< fewer literals left in the block than its length says
  kTruncatedOffset,    ///< a match's two offset bytes are not both there
  kOffsetZero,         ///< "0 is an invalid offset value"
  kOffsetBeforeStart,  ///< the match would copy from before the first decoded byte
  kPastCapacity,       ///< more decoded bytes than the capacity allows
  kEndsWithMatch,      ///< "the last sequence contains only literals"
  kShortLastLiterals,  ///< "the last 5 bytes of input are always literals"
  kLateMatch,          ///< "the last match must start at least 12 bytes before the end"
};

[[nodiscard]] constexpr std::string_view describe(Breach breach) noexcept {
  switch (breach) {
    case Breach::kNone:
      return "none";
    case Breach::kNoToken:
      return "no token";
    case Breach::kTruncatedLength:
      return "truncated length";
    case Breach::kTruncatedLiterals:
      return "truncated literals";
    case Breach::kTruncatedOffset:
      return "truncated offset";
    case Breach::kOffsetZero:
      return "offset zero";
    case Breach::kOffsetBeforeStart:
      return "offset before start";
    case Breach::kPastCapacity:
      return "past capacity";
    case Breach::kEndsWithMatch:
      return "ends with a match";
    case Breach::kShortLastLiterals:
      return "fewer than five last literals";
    case Breach::kLateMatch:
      return "last match too late";
  }
  return "unrecognised breach";
}

/// One sequence as the block coded it. `match` is 0 for the last sequence,
/// which has none.
struct Sequence {
  std::size_t literals = 0;
  std::size_t offset = 0;
  std::size_t match = 0;
  friend bool operator==(const Sequence&, const Sequence&) = default;
};

/// What decoding a block found: the decoded bytes and the sequences that
/// made them, and if the block is not valid, the breach and the byte of the
/// block where it was found (for the end-of-block rules, the token of the
/// sequence that broke them).
struct Decoded {
  std::vector<std::byte> bytes;
  std::vector<Sequence> sequences;
  Breach breach = Breach::kNone;
  std::size_t at = 0;
  /// Where the last match started in the decoded bytes, if any did.
  bool has_match = false;
  std::size_t last_match_start = 0;
};

namespace detail {

/// A length field: `nibble`, and if it is 15, bytes added while each is 255.
/// Returns false if the block ends before the field does.
inline bool read_length(std::span<const std::byte> block, std::size_t& at, std::size_t nibble,
                        std::size_t& length) {
  length = nibble;
  if (nibble != 15) {
    return true;
  }
  for (;;) {
    if (at >= block.size()) {
      return false;
    }
    const auto more = std::to_integer<std::size_t>(block[at++]);
    length += more;
    if (more != 255) {
      return true;
    }
  }
}

}  // namespace detail

/// Decodes one block into at most `capacity` bytes.
inline Decoded decode_block(std::span<const std::byte> block, std::size_t capacity) {
  Decoded result;
  std::vector<std::byte>& out = result.bytes;
  const auto fail = [&result](Breach breach, std::size_t at) {
    result.breach = breach;
    result.at = at;
    return result;
  };
  if (block.empty()) {
    return fail(Breach::kNoToken, 0);
  }
  std::size_t at = 0;
  std::size_t last_token = 0;
  std::size_t last_literals = 0;
  while (at < block.size()) {
    last_token = at;
    const auto token = std::to_integer<std::size_t>(block[at++]);
    std::size_t literals = 0;
    if (!detail::read_length(block, at, token >> 4U, literals)) {
      return fail(Breach::kTruncatedLength, at);
    }
    if (literals > block.size() - at) {
      return fail(Breach::kTruncatedLiterals, at);
    }
    if (literals > capacity - out.size()) {
      return fail(Breach::kPastCapacity, at);
    }
    out.insert(out.end(), block.begin() + static_cast<std::ptrdiff_t>(at),
               block.begin() + static_cast<std::ptrdiff_t>(at + literals));
    at += literals;
    last_literals = literals;
    result.sequences.push_back(Sequence{literals, 0, 0});
    if (at == block.size()) {
      // The block ends right after the literals: this was the last sequence.
      const std::size_t total = out.size();
      if (last_literals < std::min<std::size_t>(5, total)) {
        return fail(Breach::kShortLastLiterals, last_token);
      }
      if (result.has_match && result.last_match_start + 12 > total) {
        return fail(Breach::kLateMatch, last_token);
      }
      return result;
    }
    if (block.size() - at < 2) {
      return fail(Breach::kTruncatedOffset, at);
    }
    const std::size_t offset = std::to_integer<std::size_t>(block[at]) |
                               (std::to_integer<std::size_t>(block[at + 1]) << 8U);
    if (offset == 0) {
      return fail(Breach::kOffsetZero, at);
    }
    if (offset > out.size()) {
      return fail(Breach::kOffsetBeforeStart, at);
    }
    at += 2;
    std::size_t match = 0;
    if (!detail::read_length(block, at, token & 0x0FU, match)) {
      return fail(Breach::kTruncatedLength, at);
    }
    match += 4;
    if (match > capacity - out.size()) {
      return fail(Breach::kPastCapacity, at);
    }
    result.has_match = true;
    result.last_match_start = out.size();
    result.sequences.back().offset = offset;
    result.sequences.back().match = match;
    // Byte by byte, so a match longer than its offset repeats what it has
    // just written: "the overlap match scenario".
    const std::size_t from = out.size() - offset;
    for (std::size_t k = 0; k < match; ++k) {
      out.push_back(out[from + k]);
    }
  }
  // The loop leaves only through the last sequence's literals; reaching here
  // means the block ended right after a match.
  return fail(Breach::kEndsWithMatch, at);
}

/// What the spec's oracle answers, as this test decoder reads it (spec.hpp,
/// "WHAT THE ORACLE ANSWERS").
enum class Verdict : std::uint8_t { kAgreement, kMismatch, kNotChecked };

/// What was wrong, when the verdict is kMismatch.
enum class Check : std::uint8_t {
  kNone,
  kMagic,            ///< bytes [0, 4) are not kMagic
  kLengthOverBound,  ///< L > compress_bound(kInputBytes)
  kPaddingNotZero,   ///< a byte after the block is not zero
  kBlock,            ///< the block breaks the format: see `breach`
  kDecodedSize,      ///< the block decodes to other than kInputBytes bytes
  kDecodedBytes,     ///< the block decodes to bytes that differ from the input
};

/// Whose bytes `at` counts: the unit's output, or the decoded bytes.
enum class Domain : std::uint8_t { kStored, kDerived };

struct Judgement {
  Verdict verdict = Verdict::kNotChecked;
  Check check = Check::kNone;
  Breach breach = Breach::kNone;
  Domain domain = Domain::kStored;
  std::size_t at = 0;
  std::string why;
};

/// Judges one unit's output against the input it was computed from.
inline Judgement judge_unit(std::span<const std::byte> input, std::span<const std::byte> output) {
  Judgement judgement;
  if (input.size() != kInputBytes || output.size() != output_bytes_for(kInputBytes)) {
    judgement.why = "evidence of the wrong size: input " + std::to_string(input.size()) +
                    " bytes, output " + std::to_string(output.size()) + " bytes";
    return judgement;
  }
  judgement.verdict = Verdict::kMismatch;
  if (load_le32(output, 0) != kMagic) {
    judgement.check = Check::kMagic;
    return judgement;
  }
  const std::size_t length = load_le32(output, 4);
  if (length > compress_bound(kInputBytes)) {
    judgement.check = Check::kLengthOverBound;
    judgement.at = 4;
    return judgement;
  }
  const std::span<const std::byte> padding = output.subspan(kHeaderBytes + length);
  const auto stray = std::ranges::find_if(padding, [](std::byte b) { return b != std::byte{0}; });
  if (stray != padding.end()) {
    judgement.check = Check::kPaddingNotZero;
    judgement.at = kHeaderBytes + length + static_cast<std::size_t>(stray - padding.begin());
    return judgement;
  }
  const Decoded decoded = decode_block(output.subspan(kHeaderBytes, length), kInputBytes);
  if (decoded.breach != Breach::kNone) {
    judgement.check = Check::kBlock;
    judgement.breach = decoded.breach;
    judgement.at = kHeaderBytes + decoded.at;
    return judgement;
  }
  if (decoded.bytes.size() != kInputBytes) {
    judgement.check = Check::kDecodedSize;
    judgement.domain = Domain::kDerived;
    judgement.at = decoded.bytes.size();
    return judgement;
  }
  const auto differ = std::ranges::mismatch(decoded.bytes, input);
  if (differ.in1 != decoded.bytes.end()) {
    judgement.check = Check::kDecodedBytes;
    judgement.domain = Domain::kDerived;
    judgement.at = static_cast<std::size_t>(differ.in1 - decoded.bytes.begin());
    return judgement;
  }
  judgement.verdict = Verdict::kAgreement;
  return judgement;
}

}  // namespace loadforge::workloads::compression::testing

#endif
