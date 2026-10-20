// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_PARTICLES_DIRECT_CHECK_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_PARTICLES_DIRECT_CHECK_HPP

// T3 for the particles workload: the direct O(n^2) force summation on a subset
// of the particles, and the verdict of the specification (spec.hpp, "The
// check, and its bound"), written from spec.hpp alone -- never from the
// kernels. This is the stream's own reading of the check; the oracle stream's
// is a different file by a different author.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <vector>

#include "workloads/particles/spec.hpp"

namespace loadforge::workloads::particles::direct {

enum class Verdict { kAgreement, kBoundedMismatch, kNotChecked };

struct Result {
  Verdict verdict = Verdict::kNotChecked;
  std::size_t entries_compared = 0;  ///< Agreement's count, or BoundedMismatch's blocks_compared
  std::size_t element = 0;           ///< 3 p + c of the first failing entry
  double residual = 0.0;
  double tolerance = 0.0;
};

/// A function of the unit index alone: kSampledParticles distinct particles,
/// ascending, always including 0 and n - 1.
inline std::vector<std::size_t> sample(std::uint64_t index) {
  std::set<std::size_t> chosen = {0, kParticles - 1};
  for (std::uint64_t k = 1; chosen.size() < kSampledParticles; ++k) {
    chosen.insert(static_cast<std::size_t>((index * 7919 + k * 263) % kParticles));
  }
  return {chosen.begin(), chosen.end()};
}

/// What the direct sum knows about one particle: the force, the number of
/// candidates M and, per component, S~ = the sum of |term|.
struct Direct {
  std::array<double, 3> force{};
  std::array<double, 3> abs_sum{};
  std::size_t candidates = 0;
};

/// F~_p: all n - 1 terms in increasing j, plain scalar code, the model of the
/// specification ("The evaluation model").
inline Direct sum_directly(std::span<const double> positions, std::size_t p) {
  Direct direct;
  for (std::size_t j = 0; j < kParticles; ++j) {
    if (j == p) {
      continue;
    }
    const double dx = positions[3 * p] - positions[3 * j];
    const double dy = positions[3 * p + 1] - positions[3 * j + 1];
    const double dz = positions[3 * p + 2] - positions[3 * j + 2];
    const double q = dx * dx + dy * dy + dz * dz;
    if (q < kCandidateLimit) {
      ++direct.candidates;
    }
    const double t = 1.0 - q / 4.0;
    if (t <= 0.0) {
      continue;
    }
    const double w = t * t;
    const std::array<double, 3> term = {w * dx, w * dy, w * dz};
    for (std::size_t c = 0; c < 3; ++c) {
      direct.force[c] += term[c];
      direct.abs_sum[c] += std::fabs(term[c]);
    }
  }
  return direct;
}

/// T' of the specification for component c of a particle: the derived bound
/// with the slack, floored.
inline double tolerance_of(const Direct& direct, std::size_t c) {
  if (direct.candidates == 0) {
    return kMinTolerance;
  }
  const double derived = force_tolerance(direct.candidates, direct.abs_sum[c]);
  return std::max(derived * (1.0 + kEvaluationSlack), kMinTolerance);
}

inline std::vector<double> doubles_of(std::span<const std::byte> bytes) {
  std::vector<double> values(bytes.size() / sizeof(double));
  std::memcpy(values.data(), bytes.data(), values.size() * sizeof(double));
  return values;
}

/// The direct sums for `chosen`, kept so that many outputs can be judged
/// against one reference.
struct Reference {
  std::vector<std::size_t> chosen;
  std::vector<Direct> sums;
};

inline Reference make_reference(std::span<const double> positions,
                                std::span<const std::size_t> chosen) {
  Reference reference;
  reference.chosen.assign(chosen.begin(), chosen.end());
  for (const std::size_t p : chosen) {
    reference.sums.push_back(sum_directly(positions, p));
  }
  return reference;
}

/// The verdict on the forces `forces` (3 n doubles) against a reference:
/// entries in increasing particle number, x, y, z; the first that fails names
/// the verdict.
inline Result judge(const Reference& reference, std::span<const double> forces) {
  Result result;
  std::size_t compared = 0;
  for (std::size_t k = 0; k < reference.chosen.size(); ++k) {
    const std::size_t p = reference.chosen[k];
    const Direct& direct = reference.sums[k];
    for (std::size_t c = 0; c < 3; ++c) {
      ++compared;
      const double residual = std::fabs(forces[3 * p + c] - direct.force[c]);
      const double tolerance = tolerance_of(direct, c);
      if (!(residual <= tolerance)) {
        result.verdict = Verdict::kBoundedMismatch;
        result.entries_compared = compared;
        result.element = 3 * p + c;
        result.residual = residual;
        result.tolerance = tolerance;
        return result;
      }
    }
  }
  result.verdict = Verdict::kAgreement;
  result.entries_compared = compared;
  return result;
}

/// Whether the evidence is one the tolerance can be derived for: the sizes of
/// the unit, and every coordinate finite and in [0, kBoxEdge).
inline bool checkable(std::span<const std::byte> input, std::span<const std::byte> output) {
  if (input.size() != kInputBytes || output.size() != kOutputBytes) {
    return false;
  }
  const std::vector<double> positions = doubles_of(input);
  return std::ranges::all_of(
      positions, [](double v) { return std::isfinite(v) && v >= 0.0 && v < kBoxEdge; });
}

/// The verdict on `output` for the particles `chosen` (ascending), against
/// `input`. NotChecked when the evidence is not of the sizes kInputBytes and
/// kOutputBytes, or an input coordinate is not finite or is outside
/// [0, kBoxEdge).
inline Result check_particles(std::span<const std::byte> input, std::span<const std::byte> output,
                              std::span<const std::size_t> chosen) {
  if (!checkable(input, output)) {
    return Result{};
  }
  return judge(make_reference(doubles_of(input), chosen), doubles_of(output));
}

/// The check of the specification: the unit's sampled particles.
inline Result check(std::uint64_t index, std::span<const std::byte> input,
                    std::span<const std::byte> output) {
  const std::vector<std::size_t> chosen = sample(index);
  return check_particles(input, output, chosen);
}

}  // namespace loadforge::workloads::particles::direct

#endif
