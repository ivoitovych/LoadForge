// SPDX-License-Identifier: GPL-3.0-or-later
//
// CRC-64/XZ against the values xz itself reports for the same bytes
// (tests/fixtures/xz/verification/checksum/crc64.txt, which says how they
// were made), and the continuation rule a running checksum depends on.

#include "verification/checksum/crc64.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

struct XzValue {
  std::string stream;
  std::size_t length = 0;
  std::uint64_t crc = 0;
};

std::vector<XzValue> xz_values() {
  std::ifstream file(LOADFORGE_SOURCE_DIR "/tests/fixtures/xz/verification/checksum/crc64.txt");
  EXPECT_TRUE(file.is_open());
  std::vector<XzValue> values;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    XzValue value;
    std::string crc;
    fields >> value.stream >> value.length >> crc;
    value.crc = std::stoull(crc, nullptr, 16);
    values.push_back(value);
  }
  return values;
}

Bytes input_of(const XzValue& value) {
  if (value.stream == "check") {
    const std::string text = "123456789";
    Bytes bytes;
    for (const char c : text) {
      bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
  }
  Bytes bytes(value.length);
  rng::fill_stream(0x0C4C64, std::stoull(value.stream), bytes);
  return bytes;
}

TEST(Crc64, EveryValueXzReportedForTheSameBytes) {
  const std::vector<XzValue> values = xz_values();
  ASSERT_EQ(values.size(), 19U) << "the fixture is read whole";
  for (const XzValue& value : values) {
    const Bytes bytes = input_of(value);
    ASSERT_EQ(bytes.size(), value.length);
    EXPECT_EQ(crc64(bytes), value.crc)
        << "stream " << value.stream << ", " << value.length << " bytes";
  }
}

TEST(Crc64, TheCatalogueCheckValueAndTheEmptyInput) {
  const std::string text = "123456789";
  EXPECT_EQ(crc64(std::as_bytes(std::span(text))), 0x995DC9BBDF1939FAULL);
  EXPECT_EQ(crc64({}), 0U) << "the empty input: all-ones in, all-ones out";
}

TEST(Crc64, AContinuationIsTheCrcOfTheWhole) {
  // Splits at every position of a buffer long enough for the eight-byte loop
  // and its tail on both sides of the split.
  Bytes bytes(83);
  rng::fill_stream(5, 0, bytes);
  const std::uint64_t whole = crc64(bytes);
  for (std::size_t split = 0; split <= bytes.size(); ++split) {
    const std::span<const std::byte> all(bytes);
    EXPECT_EQ(crc64_update(crc64(all.first(split)), all.subspan(split)), whole) << split;
  }
}

}  // namespace
}  // namespace loadforge::verification
