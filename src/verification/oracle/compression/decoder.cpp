// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/compression/decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/// One pass over one block: the position in the block, the bytes decoded so
/// far, and a step per field of a sequence. Each step either moves on or
/// says which rule broke where.
class Decoder {
 public:
  Decoder(std::span<const std::byte> block, std::span<std::byte> decoded) noexcept
      : block_(block), decoded_(decoded) {}

  core::Result<Summary, Malformation> run() noexcept {
    while (true) {
      if (in_ == block_.size()) {
        return broken(Rule::kNoLastSequence, in_);
      }
      const std::size_t token_at = in_;
      const auto token = std::to_integer<unsigned>(block_[in_++]);
      if (const auto malformation = literals(token >> 4U)) {
        return *malformation;
      }
      if (in_ == block_.size()) {
        return last(token_at);
      }
      if (const auto malformation = match(token & 0x0FU)) {
        return *malformation;
      }
    }
  }

 private:
  [[nodiscard]] Malformation broken(Rule rule, std::size_t at) const noexcept {
    return Malformation{rule, at, out_};
  }

  /// Adds a length's extension bytes to `length`. False when the block ends
  /// before the byte that is not 255.
  bool extend(std::size_t& length) noexcept {
    while (in_ < block_.size()) {
      const auto value = std::to_integer<unsigned>(block_[in_++]);
      length += value;
      if (value != kMoreMark) {
        return true;
      }
    }
    return false;
  }

  std::optional<Malformation> literals(unsigned field) noexcept {
    std::size_t length = field;
    if (field == kRunMark && !extend(length)) {
      return broken(Rule::kLiteralLengthTruncated, in_);
    }
    if (length > block_.size() - in_) {
      return broken(Rule::kLiteralsTruncated, in_);
    }
    if (length > decoded_.size() - out_) {
      return broken(Rule::kLiteralsTooLong, in_);
    }
    std::copy_n(block_.begin() + static_cast<std::ptrdiff_t>(in_), length,
                decoded_.begin() + static_cast<std::ptrdiff_t>(out_));
    in_ += length;
    out_ += length;
    ++summary_.sequences;
    summary_.longest_literals = std::max(summary_.longest_literals, length);
    return std::nullopt;
  }

  /// The block ended after a sequence's literals: that was the last one.
  [[nodiscard]] core::Result<Summary, Malformation> last(std::size_t token_at) const noexcept {
    const unsigned match_field = std::to_integer<unsigned>(block_[token_at]) & 0x0FU;
    if (match_field != 0) {
      return broken(Rule::kLastTokenHasMatch, token_at);
    }
    if (out_ != decoded_.size()) {
      return broken(Rule::kDecodedTooShort, token_at);
    }
    return summary_;
  }

  std::optional<Malformation> match(unsigned field) noexcept {
    const std::size_t offset_at = in_;
    if (block_.size() - in_ < 2) {
      return broken(Rule::kOffsetTruncated, offset_at);
    }
    const std::size_t offset = std::to_integer<std::size_t>(block_[in_]) |
                               (std::to_integer<std::size_t>(block_[in_ + 1]) << 8U);
    in_ += 2;
    if (offset == 0) {
      return broken(Rule::kOffsetZero, offset_at);
    }
    if (offset > out_) {
      return broken(Rule::kOffsetBeforeStart, offset_at);
    }
    std::size_t length = field + kMinMatch;
    if (field == kRunMark && !extend(length)) {
      return broken(Rule::kMatchLengthTruncated, in_);
    }
    // Neither subtraction can wrap: out_ <= size always, and the first test
    // leaves at least kMatchStartLimit bytes after out_.
    const std::size_t size = decoded_.size();
    if (size - out_ < kMatchStartLimit) {
      return broken(Rule::kMatchTooLate, offset_at);
    }
    if (length > size - out_ - kLastLiterals) {
      return broken(Rule::kMatchIntoLastLiterals, offset_at);
    }
    // Byte by byte, front to back: when the match overlaps its own output
    // (length > offset), the later bytes it copies are the ones it just wrote.
    for (std::size_t copied = 0; copied < length; ++copied, ++out_) {
      decoded_[out_] = decoded_[out_ - offset];
    }
    ++summary_.matches;
    summary_.max_offset = std::max(summary_.max_offset, offset);
    summary_.longest_match = std::max(summary_.longest_match, length);
    summary_.overlapping_matches += length > offset ? 1U : 0U;
    return std::nullopt;
  }

  std::span<const std::byte> block_;
  std::span<std::byte> decoded_;
  std::size_t in_ = 0;
  std::size_t out_ = 0;
  Summary summary_;
};

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
  return Decoder(block, decoded).run();
}

}  // namespace loadforge::verification::oracle::compression
