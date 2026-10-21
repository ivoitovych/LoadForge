// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/compression/generator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "primitives/rng/philox.hpp"

namespace loadforge::workloads::compression {

void generate_input(std::uint64_t seed, std::uint64_t index, std::span<std::byte> input) noexcept {
  rng::fill_stream(seed, index, input);
  std::uint64_t draw = kControlPosition;
  std::size_t at = 0;
  while (at < input.size()) {
    const std::uint64_t bits = rng::u64_of(rng::block_at(seed, index, draw++));
    // Three independent fields of one draw: the literal run from the low
    // byte, the copy run from the next, the distance from the bits above.
    at += 1 + (bits & 0xFFU) % kMaxLiteralRun;
    const std::size_t run = kMinCopyRun + ((bits >> 8U) & 0xFFU) % (kMaxCopyRun - kMinCopyRun + 1);
    const std::size_t end = std::min(at + run, input.size());
    const std::size_t distance = 1 + (bits >> 16U) % std::min(at, kMaxDistance);
    for (; at < end; ++at) {
      input[at] = input[at - distance];
    }
  }
}

}  // namespace loadforge::workloads::compression
