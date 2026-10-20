// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the oracle against the reference tool. Every block lz4 1.9.4 made
// decodes, under the strict rules, to the input it was made from; every
// hand-built block decodes to what `lz4 -d` decoded it to; and the fixtures
// reach the boundaries they are kept for (tests/fixtures/lz4/README.md).

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "unit/verification/oracle/compression/lz4_support.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/compression/decoder.hpp"
#include "verification/oracle/compression/oracle.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::verification::oracle::compression {
namespace {

namespace lz4 = test::lz4;
namespace spec = workloads::compression;

Summary decoded_summary(const lz4::Fixture& fixture) {
  lz4::Bytes decoded(fixture.input.size());
  const auto result = decode_block(fixture.block, decoded);
  EXPECT_TRUE(result.has_value()) << fixture.name << ": " << describe(result.error().rule)
                                  << " at byte " << result.error().at;
  EXPECT_EQ(decoded, fixture.input) << fixture.name;
  return result.has_value() ? result.value() : Summary{};
}

TEST(Lz4ReferenceTool, EveryFixtureDecodesStrictlyToItsInput) {
  const std::vector<lz4::Fixture> fixtures = lz4::fixtures();
  ASSERT_EQ(fixtures.size(), 22U);
  for (const lz4::Fixture& fixture : fixtures) {
    decoded_summary(fixture);
    if (fixture.kind != "crafted") {
      EXPECT_LE(fixture.block.size(), spec::compress_bound(fixture.input.size())) << fixture.name;
    }
  }
}

TEST(Lz4ReferenceTool, EveryUnitTheToolMadeIsAgreedWith) {
  const CompressionOracle oracle;
  std::size_t units = 0;
  for (const lz4::Fixture& fixture : lz4::fixtures()) {
    if (fixture.kind != "unit") {
      continue;
    }
    ASSERT_EQ(fixture.input.size(), spec::kInputBytes) << fixture.name;
    const auto unit = lz4::unit_case(units++, fixture.input, fixture.block);
    const Verdict verdict = oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
    EXPECT_TRUE(std::holds_alternative<Agreement>(verdict)) << fixture.name;
  }
  EXPECT_EQ(units, 7U);
}

TEST(Lz4ReferenceTool, TheUnitsCoverIncompressibleToHighlyCompressible) {
  // One literal run of the whole unit, its length 15 + 256 * 255 + 241.
  const Summary random = decoded_summary(lz4::fixture("random-l1"));
  EXPECT_EQ(random.sequences, 1U);
  EXPECT_EQ(random.matches, 0U);
  EXPECT_EQ(random.longest_literals, spec::kInputBytes);

  // Long runs of zeros: matches far past the first 255 of extension.
  const Summary runs = decoded_summary(lz4::fixture("runs-l1"));
  EXPECT_GT(runs.longest_match, 19U + 2 * 255U);
  EXPECT_GT(runs.overlapping_matches, 0U);

  // Mixed copies: long matches, many of them overlapping their own output.
  const Summary mixed = decoded_summary(lz4::fixture("mixed-l1"));
  EXPECT_GT(mixed.longest_match, 1900U);
  EXPECT_GT(mixed.overlapping_matches, 50U);

  // Text: thousands of short matches, some from near the far end of the unit.
  const Summary text = decoded_summary(lz4::fixture("text-l1"));
  EXPECT_GT(text.matches, 10000U);
  EXPECT_GT(decoded_summary(lz4::fixture("text-l12")).max_offset, 64000U);
}

TEST(Lz4ReferenceTool, MakesAMatchAtTheLargestOffset) {
  const Summary far = decoded_summary(lz4::fixture("far-l12"));
  EXPECT_EQ(far.max_offset, 65535U);
}

TEST(Lz4ReferenceTool, MeetsBothEndOfBlockMarginsExactly) {
  // a13-l12 is 13 61 01 00 50 61 61 61 61 61: one literal, a match of 7 at
  // offset 1 -- starting at byte 1, exactly 12 before the end of 13, and
  // ending at byte 8, exactly 5 before it -- and five literals.
  const lz4::Fixture fixture = lz4::fixture("a13-l12");
  EXPECT_EQ(fixture.block, lz4::block_of({{lz4::text("a"), 1, 7}, {lz4::text("aaaaa")}}));
  const Summary summary = decoded_summary(fixture);
  EXPECT_EQ(summary.matches, 1U);
  EXPECT_EQ(summary.longest_match, 7U);

  // a64-l1: one literal and a match of 58, ending exactly 5 bytes before the
  // end, its length in an extension byte.
  const lz4::Fixture longer = lz4::fixture("a64-l1");
  EXPECT_EQ(longer.block, lz4::block_of({{lz4::text("a"), 1, 58}, {lz4::text("aaaaa")}}));
  EXPECT_EQ(decoded_summary(longer).longest_match, 58U);

  // Shorter than 13 bytes: the tool writes literals only.
  EXPECT_EQ(decoded_summary(lz4::fixture("hello-l1")).matches, 0U);
}

TEST(Lz4ReferenceTool, DecodesTheHandBuiltBoundariesAsTheDecoderDoes) {
  EXPECT_EQ(lz4::fixture("empty").input.size(), 0U);
  EXPECT_EQ(decoded_summary(lz4::fixture("empty")).sequences, 1U);
  for (const std::size_t length : {15U, 270U, 525U}) {
    const std::string name = "literals-" + std::to_string(length);
    EXPECT_EQ(decoded_summary(lz4::fixture(name)).longest_literals, length) << name;
  }
  for (const std::size_t length : {18U, 19U, 274U}) {
    const std::string name = "match-" + std::to_string(length);
    EXPECT_EQ(decoded_summary(lz4::fixture(name)).longest_match, length) << name;
  }
  EXPECT_EQ(decoded_summary(lz4::fixture("no-literals")).matches, 2U);
  const Summary one = decoded_summary(lz4::fixture("offset-1"));
  EXPECT_EQ(one.max_offset, 1U);
  EXPECT_EQ(one.overlapping_matches, 1U);
  EXPECT_EQ(decoded_summary(lz4::fixture("overlap-3")).overlapping_matches, 1U);
  EXPECT_EQ(lz4::fixture("margins-exact").input, lz4::text("abcdefgabcdefgVWXYZ"));
}

TEST(Lz4ReferenceTool, TheManifestListsEveryFixtureInTheDirectory) {
  std::set<std::string> listed;
  for (const lz4::Fixture& fixture : lz4::fixtures()) {
    EXPECT_TRUE(listed.insert(fixture.name).second) << fixture.name;
  }
  std::set<std::string> present;
  for (const auto& entry : std::filesystem::directory_iterator(lz4::kFixtureDirectory)) {
    const std::filesystem::path& path = entry.path();
    if (path.extension() == ".in" || path.extension() == ".lz4") {
      present.insert(path.stem().string());
    }
  }
  EXPECT_EQ(present, listed);
}

}  // namespace
}  // namespace loadforge::verification::oracle::compression

namespace loadforge::contracts {
// lz4_support.hpp brings the oracle suites for OracleCase; this module runs
// OracleContract in its T1 suite, and the registry is not its to provide.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts
