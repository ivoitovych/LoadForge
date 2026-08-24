// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/selftest.hpp"

#include <gtest/gtest.h>

#include <string>

namespace loadforge::selftest {
namespace {

TEST(CoreDigest, IsStableAcrossRepeatedCalls) { EXPECT_EQ(core_digest(), core_digest()); }

TEST(CoreDigest, HexIsFixedWidthAndLowercase) {
  const std::string hex = core_digest_hex();
  EXPECT_EQ(hex.size(), 16U);
  for (const char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "non-hex digit: " << c;
  }
}

TEST(CoreDigest, HexMatchesTheNumericDigest) {
  const std::uint64_t value = core_digest();
  const std::string hex = core_digest_hex();
  std::uint64_t reconstructed = 0;
  for (const char c : hex) {
    reconstructed =
        (reconstructed << 4) | static_cast<std::uint64_t>(c <= '9' ? c - '0' : (c - 'a') + 10);
  }
  EXPECT_EQ(reconstructed, value);
}

TEST(Fnv1a, MatchesPublishedTestVectors) {
  // INDEPENDENT known-answer vectors, not self-consistency. The first version
  // of selftest.cpp used an offset basis of 1469598103934665603 -- nineteen
  // digits instead of twenty -- and every test still passed, because they all
  // compared the implementation against itself and one of them embedded the
  // same wrong constant.
  //
  // These values come from the FNV specification, not from this code.
  EXPECT_EQ(kFnvOffsetBasis, 0xcbf29ce484222325ULL);
  EXPECT_EQ(kFnvPrime, 0x100000001b3ULL);
  EXPECT_EQ(fnv1a("", kFnvOffsetBasis), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a("a", kFnvOffsetBasis), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(fnv1a("foobar", kFnvOffsetBasis), 0x85944171f73967e8ULL);
}

TEST(Fnv1a, ChainsAcrossCalls) {
  // core_digest folds the corpus by threading the hash through successive
  // calls, so chaining must equal hashing the concatenation.
  EXPECT_EQ(fnv1a("bar", fnv1a("foo", kFnvOffsetBasis)), fnv1a("foobar", kFnvOffsetBasis));
}

TEST(CoreDigest, IsNotTrivial) {
  // A digest of zero or of the bare offset basis would mean the corpus never
  // reached the hash.
  EXPECT_NE(core_digest(), 0U);
  EXPECT_NE(core_digest(), kFnvOffsetBasis);
}

}  // namespace
}  // namespace loadforge::selftest
