// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_STENCIL_STENCIL_EVIDENCE_HPP
#define LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_STENCIL_STENCIL_EVIDENCE_HPP

// Evidence for the stencil oracle's tests, in every tier: the unit scipy's
// filter swept (tests/fixtures/verification/oracle/stencil/), its input
// regenerated with the contract's Philox, and byte/double conversions.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::verification::oracle::stencil::evidence {

namespace spec = workloads::stencil;

/// The seed and unit index of the committed fixture (generate_fixtures.py).
inline constexpr std::uint64_t kSeed = 0x4C6F616446697263ULL;
inline constexpr std::uint64_t kFixtureIndex = 7;

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

/// The grid the specification generates for (seed, index).
inline std::vector<double> generated_grid(std::uint64_t seed, std::uint64_t index) {
  std::vector<double> grid(spec::kEdge * spec::kEdge);
  for (std::size_t e = 0; e < grid.size(); ++e) {
    grid[e] = spec::entry_value(rng::u64_of(rng::block_at(seed, index, e)));
  }
  return grid;
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

/// The unit whose output scipy computed.
inline Unit real_tool_unit() {
  const std::string path = std::string(LOADFORGE_SOURCE_DIR) +
                           "/tests/fixtures/verification/oracle/stencil/unit-" +
                           std::to_string(kFixtureIndex) + ".output.bin";
  return Unit{kFixtureIndex, bytes_of(generated_grid(kSeed, kFixtureIndex)), read_file(path)};
}

/// One full-grid sweep, the plainest reading of the specification: no window,
/// no clipping, the ring copied. The order of the five terms is the oracle's,
/// so the two agree to the bit.
inline std::vector<double> sweep_whole_grid(const std::vector<double>& grid) {
  std::vector<double> next = grid;
  for (std::size_t i = 1; i + 1 < spec::kEdge; ++i) {
    for (std::size_t j = 1; j + 1 < spec::kEdge; ++j) {
      const std::size_t at = i * spec::kEdge + j;
      double value = 0.5 * grid[at];
      value += 0.125 * grid[at - spec::kEdge];
      value += 0.125 * grid[at + spec::kEdge];
      value += 0.125 * grid[at - 1];
      value += 0.125 * grid[at + 1];
      next[at] = value;
    }
  }
  return next;
}

/// The grid after kSteps whole-grid sweeps.
inline std::vector<double> swept_grid(std::vector<double> grid) {
  for (std::size_t step = 0; step < spec::kSteps; ++step) {
    grid = sweep_whole_grid(grid);
  }
  return grid;
}

}  // namespace loadforge::verification::oracle::stencil::evidence

#endif
