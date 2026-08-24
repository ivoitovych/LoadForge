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

TEST(CoreDigest, IsNotTrivial) {
  // A digest of zero or of the bare FNV offset would mean the corpus never
  // reached the hash.
  EXPECT_NE(core_digest(), 0U);
  EXPECT_NE(core_digest(), 1469598103934665603ULL);
}

}  // namespace
}  // namespace loadforge::selftest
