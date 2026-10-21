// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_COMPRESSION_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_COMPRESSION_SUPPORT_HPP

// Small helpers the compression workload's tests share across their tiers:
// byte vectors, the committed fixtures, and one unit computed end to end.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/workloads/compression/executors.hpp"
#include "workloads/compression/lz4_block.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression::testing {

using Bytes = std::vector<std::byte>;

inline Bytes bytes_of(std::string_view text) {
  Bytes bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

inline Bytes bytes_of(std::initializer_list<unsigned> values) {
  Bytes bytes;
  for (const unsigned value : values) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

inline Bytes concat(std::initializer_list<std::span<const std::byte>> parts) {
  Bytes all;
  for (const std::span<const std::byte> part : parts) {
    all.insert(all.end(), part.begin(), part.end());
  }
  return all;
}

/// Philox bytes from a stream no workload unit uses with these seeds: data no
/// compressor can shrink, the same on every machine.
inline Bytes noise(std::size_t size, std::uint64_t seed) {
  Bytes bytes(size);
  rng::fill_stream(seed, 0xC0FFEE, bytes);
  return bytes;
}

/// A committed fixture under tests/fixtures/lz4-workload/, whole. An absent
/// file is an empty vector, which every test that reads one refuses.
inline Bytes fixture(std::string_view name) {
  const std::string path =
      std::string(LOADFORGE_SOURCE_DIR) + "/tests/fixtures/lz4-workload/" + std::string(name);
  std::ifstream file(path, std::ios::binary);
  const std::vector<char> chars((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  Bytes bytes;
  bytes.reserve(chars.size());
  for (const char c : chars) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

/// The blocks of an lz4 legacy frame (magic 0x184C2102, then each block's
/// little-endian length and the block). An empty result with `ok` false means
/// the frame did not parse.
struct Frame {
  bool ok = false;
  std::vector<Bytes> blocks;
};

inline Frame legacy_blocks(std::span<const std::byte> frame) {
  Frame parsed;
  if (frame.size() < 4 || load_le32(frame, 0) != 0x184C2102U) {
    return parsed;
  }
  std::size_t at = 4;
  while (at < frame.size()) {
    if (frame.size() - at < 4) {
      return parsed;
    }
    const std::size_t length = load_le32(frame, at);
    at += 4;
    if (frame.size() - at < length) {
      return parsed;
    }
    parsed.blocks.emplace_back(frame.begin() + static_cast<std::ptrdiff_t>(at),
                               frame.begin() + static_cast<std::ptrdiff_t>(at + length));
    at += length;
  }
  parsed.ok = true;
  return parsed;
}

/// compress_block() into a buffer of exactly the bound, trimmed to the block.
inline Bytes compressed(std::span<const std::byte> input, std::byte table_fill = std::byte{0}) {
  Bytes block(compress_bound(input.size()), std::byte{0xEE});
  Bytes table(kTableBytes, table_fill);
  block.resize(compress_block(input, block, table));
  return block;
}

}  // namespace loadforge::workloads::compression::testing

#endif
