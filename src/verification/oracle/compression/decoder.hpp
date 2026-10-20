// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_COMPRESSION_DECODER_HPP
#define LOADFORGE_VERIFICATION_ORACLE_COMPRESSION_DECODER_HPP

// A strict decoder of one LZ4 block, written from the block-format document
// (lz4/doc/lz4_Block_format.md, revised 2022-07-31) and nothing else: not
// from the compressor it checks, and not from lz4's own decoder. It is the
// independent half of the compression oracle (verification.md §3).
//
// STRICT, BECAUSE A LENIENT DECODER HIDES CORRUPTION
// --------------------------------------------------
// The document lets a decoder reject a block that breaks its end-of-block
// rules; this one always does, and it also refuses what the document says
// cannot be in a block at all. Each refusal names the rule and the byte of
// the block where it broke, so the oracle can say what was wrong and where.
// The reference decoder (lz4 1.9.4, `lz4 -d`) is more lenient on three
// counts: it decodes an offset of 0 as zeros, a match that starts fewer than
// twelve bytes before the end, and a last token with a match length. Every
// block the reference compressor makes keeps the stricter rules, and the T3
// fixtures check that it does.
//
// The decoded size is known: the caller passes a buffer of exactly that
// many bytes, and the end-of-block rules are measured from its end.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/result.hpp"

namespace loadforge::verification::oracle::compression {

/// The rules of the block format, each as the document states it.
///
///   kNoLastSequence        the block ends where a token should be: it is
///                          empty, or its last sequence has a match. "The
///                          last sequence contains only literals", and even
///                          empty data is one zero token.
///   kLiteralLengthTruncated  the block ends inside a literal length's
///                          extension bytes.
///   kLiteralsTruncated     fewer literals remain in the block than the
///                          length says.
///   kLiteralsTooLong       the literals would decode past the decoded size.
///   kLastTokenHasMatch     the last sequence's token has a match length;
///                          the last sequence has literals and nothing else.
///   kDecodedTooShort       the block ends having decoded fewer bytes than
///                          the decoded size.
///   kOffsetTruncated       the block ends inside a match's offset.
///   kOffsetZero            "0 is an invalid offset value".
///   kOffsetBeforeStart     the offset reaches before the first decoded byte.
///   kMatchLengthTruncated  the block ends inside a match length's
///                          extension bytes.
///   kMatchTooLate          the match starts fewer than 12 bytes before the
///                          end of the decoded data ("the last match must
///                          start at least 12 bytes before the end of block").
///   kMatchIntoLastLiterals the match covers any of the last 5 decoded bytes
///                          ("the last 5 bytes of input are always literals"),
///                          or would decode past the decoded size.
enum class Rule : std::uint8_t {
  kNoLastSequence = 0,
  kLiteralLengthTruncated,
  kLiteralsTruncated,
  kLiteralsTooLong,
  kLastTokenHasMatch,
  kDecodedTooShort,
  kOffsetTruncated,
  kOffsetZero,
  kOffsetBeforeStart,
  kMatchLengthTruncated,
  kMatchTooLate,
  kMatchIntoLastLiterals,
};

/// The rule, in words, for a report.
[[nodiscard]] std::string_view describe(Rule rule) noexcept;

/// Where and how a block broke a rule.
///
///   at       the byte of the block holding what broke it: the token for a
///            rule about the token, the first literal for the literals, the
///            first byte of the offset for a rule about the match, and the
///            block's size -- the byte that is not there -- for a block that
///            ended too soon.
///   decoded  how many bytes had been decoded when it broke.
struct Malformation {
  Rule rule = Rule::kNoLastSequence;
  std::size_t at = 0;
  std::size_t decoded = 0;
};

/// What a block that decoded held, for the tests that show a fixture reaches
/// the boundaries it is meant to.
struct Summary {
  std::size_t sequences = 0;
  std::size_t matches = 0;
  std::size_t max_offset = 0;
  std::size_t longest_literals = 0;
  std::size_t longest_match = 0;
  std::size_t overlapping_matches = 0;
};

/// Decodes `block` into `decoded`, whose size is the size the block must
/// decode to. On success every byte of `decoded` is written. On a
/// malformation the bytes already decoded are written and the rest are not.
[[nodiscard]] core::Result<Summary, Malformation> decode_block(
    std::span<const std::byte> block, std::span<std::byte> decoded) noexcept;

}  // namespace loadforge::verification::oracle::compression

#endif
