// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_PARTICLES_PARTICLES_EVIDENCE_HPP
#define LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_PARTICLES_PARTICLES_EVIDENCE_HPP

// Evidence for the particles oracle's tests, in every tier: the unit whose
// forces scipy's neighbour search and numpy computed
// (tests/fixtures/verification/oracle/particles/), its positions regenerated
// with the contract's Philox, and a rig that places the sampled particles of a
// unit by hand -- alone, with one neighbour, or inside a symmetric cluster of
// thirty-three candidates -- so that the force on each is known.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/oracle/particles/particles_oracle.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::verification::oracle::particles::evidence {

namespace spec = workloads::particles;

/// The seed and unit index of the committed fixture (generate_fixtures.py).
inline constexpr std::uint64_t kSeed = 0x4C6F616446697263ULL;
inline constexpr std::uint64_t kFixtureIndex = 13;

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

/// The positions the specification generates for (seed, index).
inline std::vector<double> generated_positions(std::uint64_t seed, std::uint64_t index) {
  std::vector<double> positions(3 * spec::kParticles);
  for (std::size_t e = 0; e < positions.size(); ++e) {
    positions[e] = spec::position_value(rng::u64_of(rng::block_at(seed, index, e)));
  }
  return positions;
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

/// The unit whose forces scipy's neighbour search and numpy computed.
inline Unit real_tool_unit() {
  const std::string path = std::string(LOADFORGE_SOURCE_DIR) +
                           "/tests/fixtures/verification/oracle/particles/unit-" +
                           std::to_string(kFixtureIndex) + ".output.bin";
  return Unit{kFixtureIndex, bytes_of(generated_positions(kSeed, kFixtureIndex)), read_file(path)};
}

using Point = std::array<double, 3>;

/// A unit whose particles all sit at one far corner, except the sampled ones of
/// `index`, each at a spot of its own with its neighbours (if any) placed round
/// it. The spots are 4.5 apart and a cluster reaches at most 2.0 from its spot,
/// so no cluster sees another, and the corner is far from every spot.
struct Rig {
  std::uint64_t index = 0;
  std::vector<double> positions;
  std::array<std::uint32_t, spec::kSampledParticles> sampled{};
  std::array<Point, spec::kSampledParticles> spots{};
  std::vector<std::uint32_t> free_particles;
  std::size_t next_free = 0;

  explicit Rig(std::uint64_t unit_index) : index(unit_index) {
    sampled = sampled_particles(unit_index);
    positions.assign(3 * spec::kParticles, 15.5);
    for (std::size_t k = 0; k < sampled.size(); ++k) {
      const double step = 4.5;
      const std::size_t column = k % 3;
      const std::size_t row = (k / 3) % 3;
      const std::size_t layer = k / 9;
      spots.at(k) =
          Point{2.5 + step * static_cast<double>(column), 2.5 + step * static_cast<double>(row),
                2.5 + step * static_cast<double>(layer)};
      set(sampled.at(k), spots.at(k));
    }
    for (std::uint32_t p = 0; p < spec::kParticles; ++p) {
      if (std::find(sampled.begin(), sampled.end(), p) == sampled.end()) {
        free_particles.push_back(p);
      }
    }
  }

  void set(std::size_t particle, const Point& at) {
    for (std::size_t c = 0; c < 3; ++c) {
      positions.at(3 * particle + c) = at.at(c);
    }
  }

  /// Moves the next unused particle to spot k's position plus `offset`.
  void place_near(std::size_t k, const Point& offset) {
    const Point& spot = spots.at(k);
    set(free_particles.at(next_free++),
        Point{spot[0] + offset[0], spot[1] + offset[1], spot[2] + offset[2]});
  }

  [[nodiscard]] Unit unit_with(std::span<const double> forces) const {
    return Unit{index, bytes_of(positions), bytes_of(forces)};
  }

  /// The oracle's own force on every sampled particle, entered in a zero
  /// output: a unit the oracle agrees with.
  [[nodiscard]] std::vector<double> agreeing_forces() const {
    std::vector<double> forces(3 * spec::kParticles, 0.0);
    for (const std::uint32_t p : sampled) {
      const Force f = direct_force(positions, p);
      for (std::size_t c = 0; c < 3; ++c) {
        forces.at(3 * std::size_t{p} + c) = f.force.at(c);
      }
    }
    return forces;
  }
};

/// Sixteen opposite pairs of offsets round a spot plus the spot itself: thirty-
/// three candidates whose forces cancel in pairs. Each offset is shorter than 2.
inline std::vector<Point> symmetric_offsets() {
  std::vector<Point> half;
  for (int i = -1; i <= 1; ++i) {
    for (int j = -1; j <= 1; ++j) {
      for (int k = -1; k <= 1; ++k) {
        const bool positive = i > 0 || (i == 0 && j > 0) || (i == 0 && j == 0 && k > 0);
        if (positive) {
          half.push_back(
              Point{static_cast<double>(i), static_cast<double>(j), static_cast<double>(k)});
        }
      }
    }
  }
  half.push_back(Point{1.2, 0.0, 0.0});
  half.push_back(Point{0.0, 1.2, 0.0});
  half.push_back(Point{0.0, 0.0, 1.2});
  std::vector<Point> all = half;
  for (const Point& offset : half) {
    all.push_back(Point{-offset[0], -offset[1], -offset[2]});
  }
  all.push_back(Point{0.0, 0.0, 0.0});  // a particle at the sampled one's own position
  return all;
}

}  // namespace loadforge::verification::oracle::particles::evidence

#endif
