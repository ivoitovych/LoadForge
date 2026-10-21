// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5: corruption is caught, and caught as the right kind. A malformed output
// is an ExactMismatch with no difference whose check names the rule and the
// byte; an output that decodes to the wrong bytes is an ExactMismatch with a
// Difference in Domain::kDerived. Every single-bit flip of every reference
// tool unit is tried, and each is said to be one or the other.
//
// T5b: a check that cannot be made is NotChecked, never an Agreement --
// evidence of the wrong size, and a scratch source that has nothing to give.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "unit/verification/oracle/compression/lz4_support.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/compression/oracle.hpp"
#include "verification/record.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::verification::oracle::compression {
namespace {

namespace lz4 = test::lz4;
namespace spec = workloads::compression;

// What every single-bit flip of runs-l1's header and block turned out to be
// (EverySingleBitFlip... below): 4728 flips. Mostly zeros, so many offsets
// have an identical copy one bit away.
constexpr std::size_t kRunsMalformed = 2916;
constexpr std::size_t kRunsDerived = 1295;
constexpr std::size_t kRunsReencoded = 517;

Verdict check(const Oracle& oracle, const contracts::OracleCase& unit) {
  return oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
}

TEST(CompressionOracleCorruption, EachWrongUnitIsCaughtAsItsKindAndNamed) {
  const CompressionOracle oracle;
  const std::vector<lz4::WrongUnit> wrong = lz4::wrong_units();
  ASSERT_EQ(wrong.size(), 19U);
  for (const lz4::WrongUnit& unit : wrong) {
    const Verdict verdict = check(oracle, unit.unit);
    ASSERT_TRUE(std::holds_alternative<ExactMismatch>(verdict)) << unit.name;
    const auto& mismatch = std::get<ExactMismatch>(verdict);
    EXPECT_TRUE(is_valid(verdict, Contract::kExact)) << unit.name;
    EXPECT_EQ(mismatch.blocks_compared, 1U) << unit.name;
    EXPECT_NE(mismatch.check.find(unit.says), std::string::npos)
        << unit.name << ": " << mismatch.check;
    if (unit.malformed_at.has_value()) {
      EXPECT_FALSE(mismatch.difference.has_value()) << unit.name;
      const std::string at = "malformed at byte " + std::to_string(*unit.malformed_at) + ": ";
      EXPECT_NE(mismatch.check.find(at), std::string::npos) << unit.name << ": " << mismatch.check;
    } else {
      ASSERT_TRUE(mismatch.difference.has_value()) << unit.name;
      EXPECT_EQ(mismatch.difference->domain, Domain::kDerived) << unit.name;
    }
  }
}

TEST(CompressionOracleCorruption, TheCheckSaysHowFarTheBlockDecoded) {
  const CompressionOracle oracle;
  for (const lz4::WrongUnit& unit : lz4::wrong_units()) {
    if (unit.name == "offset 0") {
      const Verdict verdict = check(oracle, unit.unit);
      const auto& mismatch = std::get<ExactMismatch>(verdict);
      EXPECT_EQ(mismatch.check,
                "compressed output malformed at byte 17: an offset of 0 (8 bytes decoded)");
    }
  }
}

TEST(CompressionOracleCorruption, ALiteralFlippedInTheToolsBlockIsOneDerivedBit) {
  // The last byte of a block is the last literal, the last decoded byte.
  const CompressionOracle oracle;
  const lz4::Fixture fixture = lz4::fixture("text-l1");
  contracts::OracleCase unit = lz4::unit_case(1, fixture.input, fixture.block);
  unit.output[spec::kHeaderBytes + fixture.block.size() - 1] ^= std::byte{0x10};
  const Verdict verdict = check(oracle, unit);
  const auto& difference = *std::get<ExactMismatch>(verdict).difference;
  EXPECT_EQ(difference.domain, Domain::kDerived);
  EXPECT_EQ(difference.first_offset, spec::kInputBytes - 1);
  EXPECT_EQ(difference.last_offset, spec::kInputBytes - 1);
  EXPECT_EQ(difference.popcount, 1U);
  EXPECT_EQ(difference.window_offset, spec::kInputBytes - 8);
  ASSERT_EQ(difference.expected.size(), 8U);
  EXPECT_EQ(difference.expected.back(), fixture.input.back());
  EXPECT_EQ(difference.actual.back(), fixture.input.back() ^ std::byte{0x10});
  EXPECT_EQ(classify_bits(difference.popcount, difference.first_offset, difference.last_offset, 64),
            BitClass::kSingleBit);
}

TEST(CompressionOracleCorruption, OneFlippedLiteralCopiedByAMatchIsManyDerivedBits) {
  // The periodic unit's first literal is copied by its match every eight
  // bytes: one stored bit becomes 8192 derived ones, which is why a decoded
  // difference is never presented as stored bits (record.hpp, Domain).
  const CompressionOracle oracle;
  contracts::OracleCase unit = lz4::unit_case(1, lz4::periodic_input(), lz4::periodic_unit_block());
  unit.output[spec::kHeaderBytes + 1] ^= std::byte{0x01};
  const Verdict verdict = check(oracle, unit);
  const auto& difference = *std::get<ExactMismatch>(verdict).difference;
  EXPECT_EQ(difference.domain, Domain::kDerived);
  EXPECT_EQ(difference.first_offset, 0U);
  EXPECT_EQ(difference.last_offset, spec::kInputBytes - 8);
  EXPECT_EQ(difference.popcount, spec::kInputBytes / 8);
  EXPECT_EQ(difference.window_offset, 0U);
  EXPECT_EQ(difference.expected.size(), kMaxWindowBytes);
  EXPECT_EQ(difference.expected, lz4::slice(unit.input, 0, kMaxWindowBytes));
  EXPECT_EQ(difference.actual[0], std::byte{'`'});
  EXPECT_EQ(difference.actual[8], std::byte{'`'});
  EXPECT_EQ(difference.actual[1], std::byte{'b'});
}

/// What single-bit flips of a unit's output turned out to be.
///   malformed  an ExactMismatch with no difference, its check naming a byte.
///   derived    an ExactMismatch with a Difference in Domain::kDerived.
///   reencoded  an Agreement, each one proved by the test to be another valid
///              encoding of the same input: an offset that now points at an
///              identical earlier copy. Right by the specification -- "any
///              valid block that decodes to the input is a right answer" --
///              so there is nothing for any oracle to catch.
struct Flips {
  std::size_t malformed = 0;
  std::size_t derived = 0;
  std::size_t reencoded = 0;
  [[nodiscard]] std::size_t total() const { return malformed + derived + reencoded; }
};

/// Whether flipping `mask` in byte `at` of `block` changes one match's offset
/// to another that copies the very same bytes. Read with the test's own walk
/// of the block's fields (lz4_support.hpp), not with the decoder under test.
bool reencodes(const lz4::Fixture& fixture, std::size_t at, std::byte mask) {
  const std::map<std::size_t, lz4::MatchField> fields = lz4::match_fields(fixture.block);
  for (const std::size_t field_at : {at, at - 1}) {
    const auto found = fields.find(field_at);
    if (found == fields.end()) {
      continue;
    }
    const lz4::MatchField& match = found->second;
    const unsigned shift = field_at == at ? 0U : 8U;
    const std::size_t offset = match.offset ^ (std::to_integer<std::size_t>(mask) << shift);
    if (offset == 0 || offset > match.decoded_at) {
      return false;
    }
    for (std::size_t copied = 0; copied < match.length; ++copied) {
      const std::size_t to = match.decoded_at + copied;
      if (fixture.input[to - offset] != fixture.input[to]) {
        return false;
      }
    }
    return true;
  }
  return false;
}

/// Flips each bit of each of `positions` (bytes of the unit's output) in
/// turn and says what the oracle made of each.
Flips flip_each_bit(const lz4::Fixture& fixture, const std::vector<std::size_t>& positions) {
  const CompressionOracle oracle;
  contracts::OracleCase unit = lz4::unit_case(0, fixture.input, fixture.block);
  Flips flips;
  for (const std::size_t at : positions) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      const auto mask = static_cast<std::byte>(1U << bit);
      unit.output[at] ^= mask;
      const Verdict verdict = check(oracle, unit);
      unit.output[at] ^= mask;
      const std::string where =
          fixture.name + ": byte " + std::to_string(at) + " bit " + std::to_string(bit);
      EXPECT_TRUE(is_valid(verdict, Contract::kExact)) << where;
      if (std::holds_alternative<Agreement>(verdict)) {
        EXPECT_GE(at, spec::kHeaderBytes) << where;
        EXPECT_TRUE(reencodes(fixture, at - spec::kHeaderBytes, mask)) << where;
        ++flips.reencoded;
        continue;
      }
      const auto* mismatch = std::get_if<ExactMismatch>(&verdict);
      if (mismatch == nullptr) {
        ADD_FAILURE() << where << ": " << verdict.index();
        continue;
      }
      if (mismatch->difference.has_value()) {
        EXPECT_EQ(mismatch->difference->domain, Domain::kDerived) << where;
        ++flips.derived;
      } else {
        EXPECT_TRUE(mismatch->check.starts_with("compressed output malformed at byte ")) << where;
        ++flips.malformed;
      }
    }
  }
  std::cout << fixture.name << ": " << flips.total() << " flips, " << flips.malformed
            << " malformed, " << flips.derived << " derived, " << flips.reencoded
            << " another encoding of the input\n";
  return flips;
}

std::vector<std::size_t> every_byte(std::size_t end) {
  std::vector<std::size_t> positions(end);
  for (std::size_t at = 0; at < end; ++at) {
    positions[at] = at;
  }
  return positions;
}

// Exit criterion 3: every single-bit flip of the header and the compressed
// block of a unit the reference tool made, each said to be malformed, a
// derived difference, or -- proved independently -- another encoding of the
// same input. The counts are pinned: a decoder that stopped refusing a rule
// would move flips between the columns.
TEST(CompressionOracleCorruption, EverySingleBitFlipOfAToolUnitIsCaughtOrIsAnotherEncoding) {
  const lz4::Fixture fixture = lz4::fixture("runs-l1");
  const Flips flips = flip_each_bit(fixture, every_byte(spec::kHeaderBytes + fixture.block.size()));
  EXPECT_EQ(flips.total(), (spec::kHeaderBytes + fixture.block.size()) * 8);
  EXPECT_EQ(flips.malformed, kRunsMalformed);
  EXPECT_EQ(flips.derived, kRunsDerived);
  EXPECT_EQ(flips.reencoded, kRunsReencoded);
}

// The other six units, each block too large to flip whole in a test that
// runs on every push: every bit of the header, of the first and last 32
// bytes of the block, and of every 1021st byte between.
TEST(CompressionOracleCorruption, SampledSingleBitFlipsOfTheOtherToolUnitsAreCaught) {
  // malformed, derived, another encoding of the input.
  const std::map<std::string, std::array<std::size_t, 3>> expected = {
      {"text-l1", {241, 639, 0}},   {"text-l12", {208, 584, 0}},  {"records-l9", {232, 464, 0}},
      {"mixed-l1", {193, 499, 12}}, {"mixed-l12", {171, 526, 7}}, {"random-l1", {320, 768, 0}},
  };
  for (const lz4::Fixture& fixture : lz4::fixtures()) {
    if (fixture.kind != "unit" || fixture.name == "runs-l1") {
      continue;
    }
    const std::size_t end = spec::kHeaderBytes + fixture.block.size();
    std::vector<std::size_t> positions;
    for (std::size_t at = 0; at < end; ++at) {
      const std::size_t in_block = at - spec::kHeaderBytes;
      if (at < spec::kHeaderBytes + 32 || at + 32 >= end || in_block % 1021 == 0) {
        positions.push_back(at);
      }
    }
    const Flips flips = flip_each_bit(fixture, positions);
    if (!expected.contains(fixture.name)) {
      ADD_FAILURE() << fixture.name << " has no expected counts";
      continue;
    }
    EXPECT_EQ(flips.malformed, expected.at(fixture.name)[0]) << fixture.name;
    EXPECT_EQ(flips.derived, expected.at(fixture.name)[1]) << fixture.name;
    EXPECT_EQ(flips.reencoded, expected.at(fixture.name)[2]) << fixture.name;
  }
}

TEST(CompressionOracleCorruption, AFlipInThePaddingIsMalformedAtThatByte) {
  const CompressionOracle oracle;
  const lz4::Fixture fixture = lz4::fixture("runs-l1");
  contracts::OracleCase unit = lz4::unit_case(0, fixture.input, fixture.block);
  for (const std::size_t at : {spec::kHeaderBytes + fixture.block.size(), unit.output.size() - 1}) {
    unit.output[at] ^= std::byte{0x80};
    const Verdict verdict = check(oracle, unit);
    const auto& mismatch = std::get<ExactMismatch>(verdict);
    EXPECT_EQ(mismatch.check, "compressed output malformed at byte " + std::to_string(at) +
                                  ": the padding after the block is not zero");
    unit.output[at] ^= std::byte{0x80};
  }
}

// T5b.

TEST(CompressionOracleNotChecked, EvidenceOfAnyOtherSizeComparesNothing) {
  const CompressionOracle oracle;
  const lz4::Fixture fixture = lz4::fixture("records-l9");
  const contracts::OracleCase good = lz4::unit_case(4, fixture.input, fixture.block);
  const std::vector<std::pair<lz4::Bytes, lz4::Bytes>> sizes = {
      {lz4::Bytes(spec::kInputBytes + 1), good.output},
      {good.input, lz4::Bytes(good.output.size() - 1)},
      {lz4::Bytes(good.input.size() / 2), lz4::Bytes(good.output.size() / 2)},
      {{}, {}},
  };
  for (const auto& [input, output] : sizes) {
    const Verdict verdict = oracle.check(UnitEvidence{4, input, output});
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
    const auto& unchecked = std::get<NotChecked>(verdict);
    EXPECT_EQ(unchecked.blocks_compared, 0U);
    EXPECT_TRUE(is_valid(verdict, Contract::kExact));
    EXPECT_NE(unchecked.why.find("the evidence is " + std::to_string(input.size()) +
                                 " bytes of input and " + std::to_string(output.size()) +
                                 " of output; a compression unit is 65536 and 65817"),
              std::string::npos)
        << unchecked.why;
    EXPECT_EQ(unchecked.what, "the compressed output, decoded, against the input");
  }
}

// The injected hook, a free function, counts into this; it must be mutable.
std::atomic<int> scratch_requests{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::unique_ptr<Scratch> no_scratch() noexcept {
  scratch_requests.fetch_add(1, std::memory_order_seq_cst);
  return nullptr;
}

TEST(CompressionOracleNotChecked, NoScratchToDecodeIntoIsNotCheckedNeverAgreed) {
  const CompressionOracle oracle(no_scratch);
  scratch_requests.store(0, std::memory_order_seq_cst);
  for (const lz4::Fixture& fixture : lz4::fixtures()) {
    if (fixture.kind != "unit") {
      continue;
    }
    const Verdict verdict = check(oracle, lz4::unit_case(2, fixture.input, fixture.block));
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << fixture.name;
    const auto& unchecked = std::get<NotChecked>(verdict);
    EXPECT_EQ(unchecked.blocks_compared, 0U);
    EXPECT_EQ(unchecked.why, "no scratch for the decoded bytes could be had");
    EXPECT_TRUE(is_valid(verdict, Contract::kExact));
  }
  EXPECT_EQ(scratch_requests.load(std::memory_order_seq_cst), 7);
}

TEST(CompressionOracleNotChecked, AMalformedHeaderOrPaddingIsReportedWithoutScratch) {
  // The header and the padding are judged before scratch is asked for; the
  // block's rules are judged as it decodes, so without scratch they are not.
  const CompressionOracle oracle(no_scratch);
  scratch_requests.store(0, std::memory_order_seq_cst);
  for (const lz4::WrongUnit& unit : lz4::wrong_units()) {
    const Verdict verdict = check(oracle, unit.unit);
    const bool header =
        unit.name == "magic" || unit.name == "length beyond the bound" || unit.name == "padding";
    if (header) {
      EXPECT_TRUE(std::holds_alternative<ExactMismatch>(verdict)) << unit.name;
    } else {
      EXPECT_TRUE(std::holds_alternative<NotChecked>(verdict)) << unit.name;
    }
  }
  EXPECT_EQ(scratch_requests.load(std::memory_order_seq_cst), 16);
}

}  // namespace
}  // namespace loadforge::verification::oracle::compression
