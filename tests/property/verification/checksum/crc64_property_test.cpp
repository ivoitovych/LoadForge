// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for the CRC a seal is made of: the error-detection guarantees crc64.hpp
// claims, each checked rather than recalled -- every single-bit error, every
// pair of bit errors in a short message, every odd number of bit errors, every
// burst of up to 64 bits -- and the algebra a running checksum depends on.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/checksum/crc64.hpp"
#include "verification/checksum/seal.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

Bytes random_bytes(std::size_t size, std::uint64_t stream) {
  Bytes bytes(size);
  rng::fill_stream(0xCAC, stream, bytes);
  return bytes;
}

void flip(Bytes& bytes, std::uint64_t bit) {
  bytes[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

TEST(Crc64Property, EverySingleBitErrorIsDetected) {
  const Bytes bytes = random_bytes(1024, 0);
  const std::uint64_t crc = crc64(bytes);
  for (std::uint64_t bit = 0; bit < bytes.size() * 8; ++bit) {
    Bytes changed = bytes;
    flip(changed, bit);
    ASSERT_NE(crc64(changed), crc) << bit;
  }
}

TEST(Crc64Property, EveryPairOfBitErrorsInALineIsDetected) {
  const Bytes bytes = random_bytes(64, 1);
  const std::uint64_t crc = crc64(bytes);
  for (std::uint64_t first = 0; first < 512; ++first) {
    for (std::uint64_t second = first + 1; second < 512; ++second) {
      Bytes changed = bytes;
      flip(changed, first);
      flip(changed, second);
      ASSERT_NE(crc64(changed), crc) << first << ", " << second;
    }
  }
}

TEST(Crc64Property, EveryOddNumberOfBitErrorsIsDetected) {
  // The polynomial has an even number of terms, so x + 1 divides it.
  std::mt19937_64 random(3);
  const Bytes bytes = random_bytes(65537, 2);
  const std::uint64_t crc = crc64(bytes);
  for (int round = 0; round < 2000; ++round) {
    Bytes changed = bytes;
    std::vector<std::uint64_t> bits;
    const int count = 1 + 2 * static_cast<int>(random() % 4);
    while (static_cast<int>(bits.size()) < count) {
      const std::uint64_t bit = random() % (bytes.size() * 8);
      if (std::find(bits.begin(), bits.end(), bit) == bits.end()) {
        bits.push_back(bit);
        flip(changed, bit);
      }
    }
    ASSERT_NE(crc64(changed), crc) << round;
  }
}

TEST(Crc64Property, EveryBurstOfUpTo64BitsIsDetected) {
  std::mt19937_64 random(4);
  const Bytes bytes = random_bytes(1 << 16, 3);
  const std::uint64_t crc = crc64(bytes);
  for (int round = 0; round < 5000; ++round) {
    const std::uint64_t length = 1 + random() % 64;
    const std::uint64_t start = random() % (bytes.size() * 8 - length + 1);
    // A burst begins and ends with an error; what lies between is anything.
    const std::uint64_t pattern =
        random() | 1U | (length > 1 ? std::uint64_t{1} << (length - 1) : 0);
    Bytes changed = bytes;
    for (std::uint64_t bit = 0; bit < length; ++bit) {
      if ((pattern >> bit & 1U) != 0) {
        flip(changed, start + bit);
      }
    }
    ASSERT_NE(crc64(changed), crc) << "burst of " << length << " at " << start;
  }
}

TEST(Crc64Property, TheCrcIsAffineInItsInput) {
  // crc(a ^ b) == crc(a) ^ crc(b) ^ crc(0) for messages of equal length: the
  // linearity every CRC has, and that a table built wrongly would break.
  for (std::uint64_t stream = 0; stream < 200; ++stream) {
    const std::size_t size = stream * 7 % 300;
    const Bytes a = random_bytes(size, stream);
    const Bytes b = random_bytes(size, stream + 1000);
    Bytes both(size);
    for (std::size_t at = 0; at < size; ++at) {
      both[at] = a[at] ^ b[at];
    }
    EXPECT_EQ(crc64(both), crc64(a) ^ crc64(b) ^ crc64(Bytes(size))) << size;
  }
}

TEST(Crc64Property, AnySplitIntoAnyPiecesSealsTheSame) {
  std::mt19937_64 random(5);
  for (std::uint64_t stream = 0; stream < 300; ++stream) {
    const Bytes bytes = random_bytes(random() % 2000, stream);
    RunningChecksum running;
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t piece = std::min<std::size_t>(random() % 40, bytes.size() - at);
      running.update(std::span(bytes).subspan(at, piece));
      at += piece;
    }
    EXPECT_EQ(running.seal(), seal(bytes)) << stream;
  }
}

}  // namespace
}  // namespace loadforge::verification
