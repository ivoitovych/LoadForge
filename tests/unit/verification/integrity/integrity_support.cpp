// SPDX-License-Identifier: GPL-3.0-or-later
//
// The out-of-line parts of the input-integrity stream's test doubles
// (integrity_support.hpp), compiled once into one library both of its test
// executables link.

#include "unit/verification/integrity/integrity_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::integrity_testing {

void RotorWorkload::compute(std::uint64_t index, std::span<const std::byte> input,
                            std::span<std::byte> output, std::span<std::byte> scratch,
                            worker::Executor& executor) const noexcept {
  const std::size_t chunks = plan_.decomposition;
  const auto mask = static_cast<std::byte>(key(index));
  auto chunk = [&](std::size_t which, std::size_t slot) noexcept {
    const std::span<std::byte> mine = scratch.subspan(slot * kScratchBytes, kScratchBytes);
    for (std::size_t at = which * input.size() / chunks; at < (which + 1) * input.size() / chunks;
         ++at) {
      const auto value = std::to_integer<std::uint8_t>(input[at]);
      mine[at % kScratchBytes] = static_cast<std::byte>(std::rotl(value, 3));
      output[at] = mine[at % kScratchBytes] ^ mask;
    }
  };
  worker::for_each_index(executor, chunks, chunk);
}

void MeanWorkload::compute(std::uint64_t /*index*/, std::span<const std::byte> input,
                           std::span<std::byte> output, std::span<std::byte> /*scratch*/,
                           worker::Executor& executor) const noexcept {
  std::array<std::uint64_t, 2> partial{};
  auto half = [&](std::size_t which, std::size_t /*slot*/) noexcept {
    std::uint64_t sum = 0;
    for (std::size_t at = which * 32; at < (which + 1) * 32; ++at) {
      sum += std::to_integer<std::uint64_t>(input[at]);
    }
    partial.at(which) = sum;
  };
  worker::for_each_index(executor, 2, half);
  const double mean = static_cast<double>(partial[0] + partial[1]) / 64.0;
  const auto bits = std::bit_cast<std::uint64_t>(mean);
  for (std::size_t byte = 0; byte < 8; ++byte) {
    output[byte] = static_cast<std::byte>(bits >> (8 * byte));
  }
}

}  // namespace loadforge::integrity_testing
