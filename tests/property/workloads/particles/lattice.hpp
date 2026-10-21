// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_PROPERTY_WORKLOADS_PARTICLES_LATTICE_HPP
#define LOADFORGE_TESTS_PROPERTY_WORKLOADS_PARTICLES_LATTICE_HPP

// Hand-built units: a few particles of the test's choosing, and the rest of the
// 4096 parked where they exert nothing on anyone.
//
// The parking places are the points of the lattice of spacing two (2i, 2j, 2k),
// i, j, k in 0..7, that are at least 2 from every test particle. Two parked
// particles are at one point (d = 0, no force) or at distance 2 or more
// (q >= 4, t <= 0: no term), and a parked and a test particle are at least 2
// apart, so the forces on the parked are exactly +0 and the forces on the test
// particles come from the test particles alone.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles::testing {

using Point = std::array<double, 3>;

/// 3 * kParticles doubles: `test` first, then the parked particles.
/// Precondition: test.size() < kParticles and the test particles leave a
/// lattice point free.
inline std::vector<double> isolated_unit(const std::vector<Point>& test) {
  std::vector<Point> places;
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
      for (int k = 0; k < 8; ++k) {
        const Point place = {2.0 * i, 2.0 * j, 2.0 * k};
        bool free = true;
        for (const Point& other : test) {
          const double dx = place[0] - other[0];
          const double dy = place[1] - other[1];
          const double dz = place[2] - other[2];
          if (dx * dx + dy * dy + dz * dz < kCutoffSquared) {
            free = false;
            break;
          }
        }
        if (free) {
          places.push_back(place);
        }
      }
    }
  }
  std::vector<double> positions;
  positions.reserve(3 * kParticles);
  for (const Point& point : test) {
    positions.insert(positions.end(), point.begin(), point.end());
  }
  for (std::size_t parked = 0; positions.size() < 3 * kParticles; ++parked) {
    const Point& place = places[parked % places.size()];
    positions.insert(positions.end(), place.begin(), place.end());
  }
  return positions;
}

}  // namespace loadforge::workloads::particles::testing

#endif
