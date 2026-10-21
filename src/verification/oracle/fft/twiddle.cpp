// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "verification/oracle/fft/fft_oracle.hpp"
#include "verification/oracle/fft/twiddle_table.hpp"

namespace loadforge::verification::oracle::fft {

const std::array<std::uint64_t, kTwiddleWords> kTwiddleBits = {
#include "verification/oracle/fft/twiddle_table.inc"
};

Complex twiddle(std::size_t m) noexcept {
  const std::span<const std::uint64_t> table(kTwiddleBits);
  return Complex{std::bit_cast<double>(table[2 * m]), std::bit_cast<double>(table[2 * m + 1])};
}

}  // namespace loadforge::verification::oracle::fft
