// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seals: taken whole or in pieces, checked against, and the record each
// outcome calls for.

#include "verification/checksum/seal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "contracts/equality.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/checksum/crc64.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

Bytes random_bytes(std::size_t size) {
  Bytes bytes(size);
  rng::fill_stream(0x5EA1, 0, bytes);
  return bytes;
}

Subject subject() {
  Subject subject;
  subject.provenance.seed = 99;
  subject.side = Side::kInput;
  subject.check = "input against its checksum";
  subject.line_bytes = 128;
  return subject;
}

TEST(Seal, IsTheLengthAndTheCrc) {
  const Bytes bytes = random_bytes(100);
  EXPECT_EQ(seal(bytes), (Seal{100, crc64(bytes)}));
  EXPECT_EQ(seal({}), (Seal{0, 0}));
}

TEST(Seal, EveryMemberIsCompared) {
  contracts::expect_memberwise_equality(Seal{1, 2},
                                        {[](Seal& s) { s.bytes = 3; }, [](Seal& s) { s.crc = 3; }});
  contracts::expect_memberwise_equality(
      SealCheck{SealOutcome::kCorrupt, Seal{1, 2}, Seal{1, 3}},
      {[](SealCheck& c) { c.outcome = SealOutcome::kIntact; },
       [](SealCheck& c) { c.sealed.reset(); }, [](SealCheck& c) { c.found = Seal{1, 2}; }});
}

TEST(RunningChecksum, PiecesOfAnySizeSealTheWhole) {
  const Bytes bytes = random_bytes(1000);
  for (const std::size_t piece :
       {std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{333}, std::size_t{1000}}) {
    RunningChecksum running;
    for (std::size_t at = 0; at < bytes.size(); at += piece) {
      running.update(std::span(bytes).subspan(at, std::min(piece, bytes.size() - at)));
    }
    EXPECT_EQ(running.seal(), seal(bytes)) << piece;
  }
  RunningChecksum nothing;
  EXPECT_EQ(nothing.seal(), seal({}));
  nothing.update({});
  EXPECT_EQ(nothing.seal(), seal({})) << "an empty piece changes nothing";
}

TEST(VerifySeal, IntactBytesAreIntact) {
  const Bytes bytes = random_bytes(64);
  const SealCheck check = verify_seal(bytes, seal(bytes));
  EXPECT_EQ(check, (SealCheck{SealOutcome::kIntact, seal(bytes), seal(bytes)}));
  EXPECT_EQ(to_record(check, subject()), std::nullopt);
}

TEST(VerifySeal, ChangedBytesAreCorrupt) {
  const Bytes bytes = random_bytes(64);
  const Seal sealed = seal(bytes);
  Bytes changed = bytes;
  changed[10] ^= std::byte{4};
  const SealCheck check = verify_seal(changed, sealed);
  EXPECT_EQ(check.outcome, SealOutcome::kCorrupt);
  EXPECT_EQ(check.sealed, sealed);
  EXPECT_EQ(check.found, seal(changed));
}

TEST(VerifySeal, NoBytesOrNoSealIsNotAPass) {
  const Bytes bytes = random_bytes(64);
  EXPECT_EQ(verify_seal({}, seal(bytes)),
            (SealCheck{SealOutcome::kNothingCompared, seal(bytes), std::nullopt}));
  EXPECT_EQ(verify_seal({}, seal({})).outcome, SealOutcome::kNothingCompared)
      << "even a seal of nothing checks nothing";
  EXPECT_EQ(verify_seal(bytes, std::nullopt),
            (SealCheck{SealOutcome::kNotSealed, std::nullopt, std::nullopt}));
}

TEST(VerifySeal, OutcomeWords) {
  EXPECT_EQ(describe(SealOutcome::kIntact), "intact");
  EXPECT_EQ(describe(SealOutcome::kCorrupt), "corrupt");
  EXPECT_EQ(describe(SealOutcome::kNothingCompared), "nothing compared");
  EXPECT_EQ(describe(SealOutcome::kNotSealed), "not sealed");
  EXPECT_EQ(describe(static_cast<SealOutcome>(9)), "unrecognised seal outcome");
}

TEST(SealToRecord, CorruptIsAnExactCorruptionThatClaimsNoBits) {
  const SealCheck check{SealOutcome::kCorrupt, Seal{4096, 0x00000000000000FFULL},
                        Seal{4096, 0xFEEDFACECAFEBEEFULL}};
  const std::optional<ErrorRecord> record = to_record(check, subject());
  ASSERT_TRUE(record.has_value());
  const auto* exact = std::get_if<ExactBitCorruption>(&*record);
  ASSERT_NE(exact, nullptr);
  EXPECT_EQ(exact->provenance, subject().provenance);
  EXPECT_EQ(exact->side, Side::kInput);
  EXPECT_EQ(exact->line_bytes, 128U);
  EXPECT_FALSE(exact->difference.has_value());
  EXPECT_EQ(exact->check,
            "input against its checksum -- crc64 0xfeedfacecafebeef over 4096 bytes, sealed at "
            "generation as crc64 0x00000000000000ff over 4096 bytes: the bytes or the checksum "
            "changed since; a checksum cannot say which bits");
  EXPECT_TRUE(is_valid(*record));
}

TEST(SealToRecord, NotSealedAndNothingComparedAreVerificationNotPerformed) {
  std::optional<ErrorRecord> record =
      to_record(SealCheck{SealOutcome::kNotSealed, std::nullopt, std::nullopt}, subject());
  ASSERT_TRUE(record.has_value());
  const auto* unrun = std::get_if<VerificationNotPerformed>(&*record);
  ASSERT_NE(unrun, nullptr);
  EXPECT_EQ(unrun->what, "input against its checksum");
  EXPECT_EQ(unrun->why,
            "no checksum was sealed when the bytes were generated, so there is nothing to check "
            "them against");
  EXPECT_EQ(unrun->blocks_compared, 0U);

  record =
      to_record(SealCheck{SealOutcome::kNothingCompared, Seal{8, 0x1}, std::nullopt}, subject());
  ASSERT_TRUE(record.has_value());
  unrun = std::get_if<VerificationNotPerformed>(&*record);
  ASSERT_NE(unrun, nullptr);
  EXPECT_EQ(unrun->why,
            "there were no bytes to check against the checksum sealed at their generation "
            "(crc64 0x0000000000000001 over 8 bytes)");
  EXPECT_EQ(unrun->provenance, subject().provenance);
}

TEST(SealToRecord, TheLongestCheckWithTheLongestSealsIsStillValid) {
  Subject longest = subject();
  longest.check = std::string(kMaxCheckBytes, 'c');
  const Seal widest{std::numeric_limits<std::uint64_t>::max(),
                    std::numeric_limits<std::uint64_t>::max()};
  const std::optional<ErrorRecord> record =
      to_record(SealCheck{SealOutcome::kCorrupt, widest, widest}, longest);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(is_valid(*record));
}

}  // namespace
}  // namespace loadforge::verification
