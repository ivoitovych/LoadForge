// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_FFT_FFT_EVIDENCE_HPP
#define LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_FFT_FFT_EVIDENCE_HPP

// Evidence for the FFT oracle's tests, in every tier: units of the real tool's
// output (numpy.fft.fft, tests/fixtures/verification/oracle/fft/), their
// inputs regenerated with the contract's Philox, and byte/double conversions.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::verification::oracle::fft::evidence {

namespace spec = workloads::fft;

/// The seed and unit indices of the committed fixtures (generate_fixtures.py).
inline constexpr std::uint64_t kSeed = 0x4C6F616446697263ULL;
inline constexpr std::array<std::uint64_t, 2> kFixtureIndices = {5, 1031};

struct Unit {
  std::uint64_t index = 0;
  std::vector<std::byte> input;
  std::vector<std::byte> output;
};

inline std::vector<std::byte> bytes_of(std::span<const double> values) {
  std::vector<std::byte> out(values.size() * sizeof(double));
  for (std::size_t e = 0; e < values.size(); ++e) {
    const auto bits = std::bit_cast<std::uint64_t>(values[e]);
    for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
      out[e * sizeof(double) + byte] = static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU);
    }
  }
  return out;
}

inline std::vector<double> doubles_of(std::span<const std::byte> bytes) {
  std::vector<double> out(bytes.size() / sizeof(double));
  for (std::size_t e = 0; e < out.size(); ++e) {
    std::uint64_t bits = 0;
    for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
      bits |= std::to_integer<std::uint64_t>(bytes[e * sizeof(double) + byte]) << (8U * byte);
    }
    out[e] = std::bit_cast<double>(bits);
  }
  return out;
}

/// The input the specification generates for (seed, index).
inline std::vector<std::byte> generated_input(std::uint64_t seed, std::uint64_t index) {
  std::vector<double> values(2 * spec::kPoints);
  for (std::size_t e = 0; e < values.size(); ++e) {
    values[e] = spec::entry_value(rng::u64_of(rng::block_at(seed, index, e)));
  }
  return bytes_of(values);
}

inline std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t at = 0; at < raw.size(); ++at) {
    out[at] = static_cast<std::byte>(static_cast<unsigned char>(raw[at]));
  }
  return out;
}

/// The unit whose output numpy computed.
inline Unit real_tool_unit(std::uint64_t index) {
  const std::string path = std::string(LOADFORGE_SOURCE_DIR) +
                           "/tests/fixtures/verification/oracle/fft/unit-" + std::to_string(index) +
                           ".output.bin";
  return Unit{index, generated_input(kSeed, index), read_file(path)};
}

/// A unit whose input is `x` (2 kPoints doubles) and whose output is `spectrum`.
inline Unit unit_of(std::uint64_t index, std::span<const double> x,
                    std::span<const double> spectrum) {
  return Unit{index, bytes_of(x), bytes_of(spectrum)};
}

}  // namespace loadforge::verification::oracle::fft::evidence

#endif
