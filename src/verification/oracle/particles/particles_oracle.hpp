// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_PARTICLES_PARTICLES_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_PARTICLES_PARTICLES_ORACLE_HPP

// The independent oracle of the particles workload (verification.md §3, §5),
// written from src/workloads/particles/spec.hpp and from nothing else: the
// force on each of kSampledParticles particles summed directly over all the
// others, term by term in the specification's evaluation model, and compared
// with the output component by component against a tolerance derived from the
// number of candidate neighbours and the size of the terms. Contract::kBounded.
//
// It allocates nothing: the positions are decoded onto the stack and the
// output is read in place, so check() cannot fail to allocate scratch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/particles/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::particles {

/// What the direct sum gives for one particle: the force components, the sum of
/// the absolute values of the terms of each (S~ of the specification), and the
/// number of candidates M_p.
struct Force {
  std::array<double, 3> force{};
  std::array<double, 3> abs_sum{};
  std::size_t candidates = 0;
};

/// The kSampledParticles distinct particle numbers a unit's check examines, in
/// increasing order. Always includes 0 and kParticles - 1; the rest are a
/// function of `index` alone.
[[nodiscard]] std::array<std::uint32_t, workloads::particles::kSampledParticles> sampled_particles(
    std::uint64_t index) noexcept;

/// The force on `particle` by every other particle, by plain scalar code in
/// increasing j. `positions` holds 3 kParticles doubles; particle < kParticles.
[[nodiscard]] Force direct_force(std::span<const double> positions, std::size_t particle) noexcept;

/// T'_pc for a particle with `candidates` = M_p and a component whose terms
/// sum, in absolute value, to `abs_sum`:
/// max(force_tolerance(M_p, S~) * (1 + kEvaluationSlack), kMinTolerance), and
/// kMinTolerance for M_p = 0.
[[nodiscard]] double tolerance_for(std::size_t candidates, double abs_sum) noexcept;

/// The little-endian double at `element` of a span of doubles stored as bytes.
/// Precondition: (element + 1) * 8 <= bytes.size().
[[nodiscard]] double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept;

class ParticlesOracle final : public Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;
};

/// The oracle for a plan the particles workload resolved: kConfiguration when
/// the plan's sizes are not a unit of the specification or its decomposition is
/// not one of its cells-per-cutoff counts. (The count does not change what is
/// checked; the plan is still held to the specification.)
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::particles

#endif
