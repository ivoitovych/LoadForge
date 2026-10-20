// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_COMPRESSION_LZ4_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_COMPRESSION_LZ4_SUPPORT_HPP

// What the compression oracle's tests share, in every tier: the fixtures the
// lz4 command line made (tests/fixtures/lz4/, read through its MANIFEST), a
// unit's output built around a block as workloads/compression/spec.hpp lays
// it out, and a writer of LZ4 sequences for the blocks the tests build by
// hand from the block-format document.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "verification/oracle/compression/decoder.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::test::lz4 {

namespace spec = workloads::compression;

using Bytes = std::vector<std::byte>;

inline const std::string kFixtureDirectory =
    std::string(LOADFORGE_SOURCE_DIR) + "/tests/fixtures/lz4";

inline Bytes read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  Bytes bytes(text.size());
  for (std::size_t at = 0; at < text.size(); ++at) {
    bytes[at] = static_cast<std::byte>(text[at]);
  }
  return bytes;
}

inline std::uint32_t le32(std::span<const std::byte> bytes, std::size_t at) {
  return spec::load_le32(bytes, at);
}

/// One fixture: the input, and the one block the legacy frame holds.
///   kind  "unit" (a 64 KiB input), "block" (any other size), or "crafted"
///         (a hand-built block whose input is what `lz4 -d` decoded it to).
struct Fixture {
  std::string name;
  std::string kind;
  Bytes input;
  Bytes block;
};

/// The single block of an lz4 legacy frame: magic 0x184C2102, then one
/// little-endian u32 length and that many bytes.
inline Bytes legacy_block(const Bytes& frame, const std::string& name) {
  constexpr std::uint32_t kLegacyMagic = 0x184C2102U;
  if (frame.size() < 8 || le32(frame, 0) != kLegacyMagic) {
    throw std::runtime_error(name + ": not an lz4 legacy frame");
  }
  const std::size_t length = le32(frame, 4);
  if (frame.size() != 8 + length) {
    throw std::runtime_error(name + ": not exactly one block");
  }
  return {frame.begin() + 8, frame.end()};
}

/// Every fixture the MANIFEST lists, in its order.
inline std::vector<Fixture> fixtures() {
  std::ifstream manifest(kFixtureDirectory + "/MANIFEST");
  if (!manifest) {
    throw std::runtime_error("cannot open the lz4 fixture MANIFEST");
  }
  std::vector<Fixture> out;
  std::string line;
  while (std::getline(manifest, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    Fixture fixture;
    fields >> fixture.name >> fixture.kind;
    const std::string stem = kFixtureDirectory + "/" + fixture.name;
    fixture.input = read_file(stem + ".in");
    fixture.block = legacy_block(read_file(stem + ".lz4"), fixture.name);
    out.push_back(std::move(fixture));
  }
  return out;
}

inline Fixture fixture(const std::string& name) {
  for (Fixture& candidate : fixtures()) {
    if (candidate.name == name) {
      return std::move(candidate);
    }
  }
  throw std::runtime_error("no lz4 fixture named " + name);
}

inline void put_le32(Bytes& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    bytes[at + byte] = static_cast<std::byte>((value >> (8 * byte)) & 0xFFU);
  }
}

/// A unit's output around `block`: magic, length, block, zero padding.
inline Bytes unit_output(std::span<const std::byte> block) {
  Bytes output(spec::output_bytes_for(spec::kInputBytes));
  put_le32(output, 0, spec::kMagic);
  put_le32(output, 4, static_cast<std::uint32_t>(block.size()));
  for (std::size_t at = 0; at < block.size(); ++at) {
    output[spec::kHeaderBytes + at] = block[at];
  }
  return output;
}

inline contracts::OracleCase unit_case(std::uint64_t index, Bytes input,
                                       std::span<const std::byte> block) {
  return contracts::OracleCase{index, std::move(input), unit_output(block)};
}

/// A length field's extension bytes, as the document writes them.
inline void put_extension(Bytes& out, std::size_t rest) {
  for (; rest >= 255; rest -= 255) {
    out.push_back(std::byte{0xFF});
  }
  out.push_back(static_cast<std::byte>(rest));
}

/// One sequence: its literals, then -- unless it is the last -- a match of
/// `match` bytes at `offset`. `match_field` overrides the token's low four
/// bits for a test that needs a token the document forbids.
struct Sequence {
  // Not explicit: the tests write a block as a braced list of sequences.
  // NOLINTNEXTLINE(google-explicit-constructor)
  Sequence(Bytes literals_in, std::optional<std::uint16_t> offset_in = std::nullopt,
           std::size_t match_in = 0, std::optional<unsigned> match_field_in = std::nullopt)
      : literals(std::move(literals_in)),
        offset(offset_in),
        match(match_in),
        match_field(match_field_in) {}

  Bytes literals;
  std::optional<std::uint16_t> offset;
  std::size_t match = 0;
  std::optional<unsigned> match_field;
};

inline Bytes encode(const Sequence& sequence) {
  const std::size_t literals = sequence.literals.size();
  const unsigned high = literals < 15 ? static_cast<unsigned>(literals) : 15U;
  unsigned low = 0;
  if (sequence.offset.has_value()) {
    low = sequence.match - 4 < 15 ? static_cast<unsigned>(sequence.match - 4) : 15U;
  }
  low = sequence.match_field.value_or(low);
  Bytes out{static_cast<std::byte>((high << 4U) | low)};
  if (high == 15) {
    put_extension(out, literals - 15);
  }
  out.insert(out.end(), sequence.literals.begin(), sequence.literals.end());
  if (sequence.offset.has_value()) {
    out.push_back(static_cast<std::byte>(*sequence.offset & 0xFFU));
    out.push_back(static_cast<std::byte>(*sequence.offset >> 8U));
    if (sequence.match - 4 >= 15) {
      put_extension(out, sequence.match - 4 - 15);
    }
  }
  return out;
}

inline Bytes block_of(const std::vector<Sequence>& sequences) {
  Bytes out;
  for (const Sequence& sequence : sequences) {
    const Bytes encoded = encode(sequence);
    out.insert(out.end(), encoded.begin(), encoded.end());
  }
  return out;
}

inline Bytes text(const std::string& value) {
  Bytes out(value.size());
  for (std::size_t at = 0; at < value.size(); ++at) {
    out[at] = static_cast<std::byte>(value[at]);
  }
  return out;
}

inline Bytes slice(const Bytes& bytes, std::size_t from, std::size_t to) {
  return {bytes.begin() + static_cast<std::ptrdiff_t>(from),
          bytes.begin() + static_cast<std::ptrdiff_t>(to)};
}

/// A unit input with period 8, "abcdefgh" repeated: the one input whose
/// encodings the tests can write by hand, since every match of it is known.
inline Bytes periodic_input() {
  Bytes input(spec::kInputBytes);
  for (std::size_t at = 0; at < input.size(); ++at) {
    input[at] = static_cast<std::byte>('a' + at % 8);
  }
  return input;
}

/// The periodic input as one literal run of `literals`, one match of `match`
/// at offset 8, and the rest as the last literals.
inline Bytes periodic_block(std::size_t literals, std::size_t match) {
  const Bytes input = periodic_input();
  const std::size_t tail = literals + match;
  return block_of({{slice(input, 0, literals), 8, match}, {slice(input, tail, input.size())}});
}

/// One match of a block, as the test's own walk of the fields finds it.
struct MatchField {
  std::size_t decoded_at = 0;
  std::size_t offset = 0;
  std::size_t length = 0;
};

/// Every match of a well-formed block, by the position of its offset's first
/// byte. A walk of the fields only -- lengths and offsets, no decoding -- for
/// the tests that need to know what a byte of a tool's block is. Assumes the
/// block is well formed: it is called on the reference tool's blocks only.
inline std::map<std::size_t, MatchField> match_fields(std::span<const std::byte> block) {
  const auto length_at = [&](std::size_t& at, std::size_t field) {
    std::size_t length = field;
    if (field == 15) {
      unsigned more = 255;
      while (more == 255) {
        more = std::to_integer<unsigned>(block[at++]);
        length += more;
      }
    }
    return length;
  };
  std::map<std::size_t, MatchField> fields;
  std::size_t at = 0;
  std::size_t decoded = 0;
  while (true) {
    const auto token = std::to_integer<std::size_t>(block[at++]);
    const std::size_t literals = length_at(at, token >> 4U);
    at += literals;
    decoded += literals;
    if (at == block.size()) {
      return fields;
    }
    const std::size_t offset_at = at;
    const std::size_t offset = std::to_integer<std::size_t>(block[at]) |
                               (std::to_integer<std::size_t>(block[at + 1]) << 8U);
    at += 2;
    const std::size_t match = length_at(at, token & 0x0FU) + 4;
    fields[offset_at] = MatchField{decoded, offset, match};
    decoded += match;
  }
}

/// A unit whose output the specification says is wrong, and how the oracle
/// must say so.
///   malformed_at  the byte of the output the check must name, when the
///                 output is malformed; absent when it decodes, wrongly.
///   says          text the check must contain: the rule, in words.
struct WrongUnit {
  std::string name;
  contracts::OracleCase unit;
  std::optional<std::size_t> malformed_at;
  std::string says;
};

/// The periodic unit's own block: eight literals, one match of 65523 at
/// offset 8 that ends exactly five bytes before the end, five literals.
/// Laid out: token 0, literals 1-8, offset 9-10, the match length's 257
/// extension bytes 11-267, last token 268, last literals 269-273.
inline Bytes periodic_unit_block() { return periodic_block(8, spec::kInputBytes - 13); }

/// One wrong unit for every rule the oracle checks: the header's, every rule
/// of the block format (decoder.hpp), and a block that decodes to the wrong
/// bytes -- built by hand from the periodic unit, or from a fixture the
/// reference tool made.
inline std::vector<WrongUnit> wrong_units() {
  using verification::oracle::compression::describe;
  using verification::oracle::compression::Rule;
  constexpr std::size_t kHeader = spec::kHeaderBytes;
  const Bytes input = periodic_input();
  const Bytes block = periodic_unit_block();
  const auto from_block = [&](std::uint64_t index, const Bytes& wrong_block) {
    return unit_case(index, input, wrong_block);
  };
  const auto from_output = [&](std::uint64_t index, std::size_t at, std::byte value) {
    contracts::OracleCase unit = unit_case(index, input, block);
    unit.output[at] = value;
    return unit;
  };
  const auto rule = [](Rule which) { return std::string(describe(which)); };
  const Bytes last5 = slice(input, input.size() - 5, input.size());
  std::vector<WrongUnit> out;

  out.push_back({"magic", from_output(100, 0, std::byte{0x4D}), 0, "magic"});
  {
    contracts::OracleCase unit = unit_case(101, input, block);
    put_le32(unit.output, 4,
             static_cast<std::uint32_t>(spec::compress_bound(spec::kInputBytes) + 1));
    out.push_back({"length beyond the bound", unit, 4, "compress_bound"});
  }
  out.push_back({"padding", from_output(102, kHeader + block.size() + 10, std::byte{1}),
                 kHeader + block.size() + 10, "padding"});
  out.push_back({"empty block", from_block(103, {}), kHeader, rule(Rule::kNoLastSequence)});
  out.push_back({"ends after a match", from_block(104, slice(block, 0, 268)), kHeader + 268,
                 rule(Rule::kNoLastSequence)});
  out.push_back({"inside a literal length", from_block(105, Bytes{std::byte{0xF0}}), kHeader + 1,
                 rule(Rule::kLiteralLengthTruncated)});
  out.push_back({"inside the literals", from_block(106, slice(block, 0, 273)), kHeader + 269,
                 rule(Rule::kLiteralsTruncated)});
  {
    Bytes six = last5;
    six.push_back(std::byte{'a'});
    out.push_back(
        {"literals past the end",
         from_block(107, block_of({{slice(input, 0, 8), 8, spec::kInputBytes - 13}, {six}})),
         kHeader + 269, rule(Rule::kLiteralsTooLong)});
  }
  out.push_back({"last token with a match", from_output(108, kHeader + 268, std::byte{0x51}),
                 kHeader + 268, rule(Rule::kLastTokenHasMatch)});
  out.push_back({"decodes short",
                 from_block(109, block_of({{slice(input, 0, 8), 8, spec::kInputBytes - 13},
                                           {slice(last5, 0, 4)}})),
                 kHeader + 268, rule(Rule::kDecodedTooShort)});
  out.push_back({"inside an offset", from_block(110, slice(block, 0, 10)), kHeader + 9,
                 rule(Rule::kOffsetTruncated)});
  {
    const contracts::OracleCase unit = from_output(111, kHeader + 9, std::byte{0});
    out.push_back({"offset 0", unit, kHeader + 9, rule(Rule::kOffsetZero)});
  }
  out.push_back({"offset before the start", from_output(112, kHeader + 9, std::byte{9}),
                 kHeader + 9, rule(Rule::kOffsetBeforeStart)});
  out.push_back({"inside a match length", from_block(113, slice(block, 0, 111)), kHeader + 111,
                 rule(Rule::kMatchLengthTruncated)});
  {
    constexpr std::size_t kLiterals = spec::kInputBytes - 11;
    const Bytes late =
        block_of({{slice(input, 0, kLiterals), 8, 4}, {slice(input, kLiterals + 4, input.size())}});
    const std::size_t offset_at = 1 + 1 + (kLiterals - 15) / 255 + kLiterals;
    out.push_back({"match starts 11 bytes before the end", from_block(114, late),
                   kHeader + offset_at, rule(Rule::kMatchTooLate)});
  }
  out.push_back({"match covers the fifth byte from the end",
                 from_block(115, block_of({{slice(input, 0, 8), 8, spec::kInputBytes - 12},
                                           {slice(last5, 1, 5)}})),
                 kHeader + 9, rule(Rule::kMatchIntoLastLiterals)});
  out.push_back({"a literal of the periodic unit", from_output(116, kHeader + 1, std::byte{'`'}),
                 std::nullopt, "decoded"});
  for (const char* name : {"text-l1", "runs-l1"}) {
    const Fixture tool = fixture(name);
    contracts::OracleCase unit = unit_case(117, tool.input, tool.block);
    unit.output[kHeader + tool.block.size() - 1] ^= std::byte{0x10};
    out.push_back({std::string("the last literal of ") + name, unit, std::nullopt, "decoded"});
  }
  return out;
}

}  // namespace loadforge::test::lz4

#endif
