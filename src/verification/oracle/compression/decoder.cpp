// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/compression/decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/result.hpp"

namespace loadforge::verification::oracle::compression {
namespace {

// The block format's constants, by the names the document gives them.
constexpr unsigned kRunMark = 15;             // a length field of 15: more bytes follow
constexpr unsigned kMoreMark = 255;           // an extension byte of 255: another follows
constexpr std::size_t kMinMatch = 4;          // "minmatch"
constexpr std::size_t kLastLiterals = 5;      // "the last 5 bytes ... are always literals"
constexpr std::size_t kMatchStartLimit = 12;  // "at least 12 bytes before the end"

/// Adds a length's extension bytes, starting at `at`, to `length`. False when
/// the block ends before the byte that is not 255.
bool read_extension(std::span<const std::byte> block, std::size_t& at,
                    std::size_t& length) noexcept {
  while (at < block.size()) {
    const auto value = std::to_integer<unsigned>(block[at++]);
    length += value;
    if (value != kMoreMark) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view describe(Rule rule) noexcept {
  switch (rule) {
    case Rule::kNoLastSequence:
      return "the block ends without a last sequence of literals only";
    case Rule::kLiteralLengthTruncated:
      return "the block ends inside a literal length";
    case Rule::kLiteralsTruncated:
      return "the block ends inside the literals";
    case Rule::kLiteralsTooLong:
      return "the literals decode past the end of the decoded data";
    case Rule::kLastTokenHasMatch:
      return "the last sequence's token has a match length";
    case Rule::kDecodedTooShort:
      return "the block ends before the end of the decoded data";
    case Rule::kOffsetTruncated:
      return "the block ends inside an offset";
    case Rule::kOffsetZero:
      return "an offset of 0";
    case Rule::kOffsetBeforeStart:
      return "an offset reaching before the first decoded byte";
    case Rule::kMatchLengthTruncated:
      return "the block ends inside a match length";
    case Rule::kMatchTooLate:
      return "a match starting fewer than 12 bytes before the end of the decoded data";
    case Rule::kMatchIntoLastLiterals:
      return "a match covering the last 5 bytes of the decoded data";
  }
  return "unrecognised rule";
}

core::Result<Summary, Malformation> decode_block(std::span<const std::byte> block,
                                                 std::span<std::byte> decoded) noexcept {
  const std::size_t size = decoded.size();
  std::size_t in = 0;
  std::size_t out = 0;
  Summary summary;
  while (true) {
    if (in == block.size()) {
      return Malformation{Rule::kNoLastSequence, in, out};
    }
    const std::size_t token_at = in;
    const auto token = std::to_integer<unsigned>(block[in++]);
    std::size_t literals = token >> 4U;
    if (literals == kRunMark && !read_extension(block, in, literals)) {
      return Malformation{Rule::kLiteralLengthTruncated, in, out};
    }
    if (literals > block.size() - in) {
      return Malformation{Rule::kLiteralsTruncated, in, out};
    }
    if (literals > size - out) {
      return Malformation{Rule::kLiteralsTooLong, in, out};
    }
    std::copy_n(block.begin() + static_cast<std::ptrdiff_t>(in), literals,
                decoded.begin() + static_cast<std::ptrdiff_t>(out));
    in += literals;
    out += literals;
    ++summary.sequences;
    summary.longest_literals = std::max(summary.longest_literals, literals);

    const unsigned match_field = token & 0x0FU;
    if (in == block.size()) {
      if (match_field != 0) {
        return Malformation{Rule::kLastTokenHasMatch, token_at, out};
      }
      if (out != size) {
        return Malformation{Rule::kDecodedTooShort, token_at, out};
      }
      return summary;
    }

    const std::size_t offset_at = in;
    if (block.size() - in < 2) {
      return Malformation{Rule::kOffsetTruncated, offset_at, out};
    }
    const std::size_t offset = std::to_integer<std::size_t>(block[in]) |
                               (std::to_integer<std::size_t>(block[in + 1]) << 8U);
    in += 2;
    if (offset == 0) {
      return Malformation{Rule::kOffsetZero, offset_at, out};
    }
    if (offset > out) {
      return Malformation{Rule::kOffsetBeforeStart, offset_at, out};
    }
    std::size_t match = match_field + kMinMatch;
    if (match_field == kRunMark && !read_extension(block, in, match)) {
      return Malformation{Rule::kMatchLengthTruncated, in, out};
    }
    // Neither subtraction can wrap: out <= size always, and the first test
    // leaves at least kMatchStartLimit bytes after out.
    if (size - out < kMatchStartLimit) {
      return Malformation{Rule::kMatchTooLate, offset_at, out};
    }
    if (match > size - out - kLastLiterals) {
      return Malformation{Rule::kMatchIntoLastLiterals, offset_at, out};
    }
    // Byte by byte, front to back: when the match overlaps its own output
    // (match > offset), the later bytes it copies are the ones it just wrote.
    for (std::size_t copied = 0; copied < match; ++copied, ++out) {
      decoded[out] = decoded[out - offset];
    }
    ++summary.matches;
    summary.max_offset = std::max(summary.max_offset, offset);
    summary.longest_match = std::max(summary.longest_match, match);
    summary.overlapping_matches += match > offset ? 1U : 0U;
  }
}

}  // namespace loadforge::verification::oracle::compression
