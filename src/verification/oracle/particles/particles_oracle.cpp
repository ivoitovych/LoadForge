// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/particles/particles_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <optional>
#include <string>
#include <utility>

namespace loadforge::verification::oracle::particles {
namespace {

namespace spec = workloads::particles;

constexpr std::size_t kElements = 3 * spec::kParticles;

/// The sampled particles other than the two ends are drawn from [1, n - 2]
/// with this stride: prime, so coprime to the span 4094 = 2 * 23 * 89.
constexpr std::size_t kParticleStride = 293;
constexpr std::size_t kInnerSamples = spec::kSampledParticles - 2;
constexpr std::uint64_t kInnerSpan = spec::kParticles - 2;
static_assert(std::gcd(kParticleStride, kInnerSpan) == 1);

/// The splitmix64 finaliser: a bijective mixing of 64 bits, unsigned throughout.
constexpr std::uint64_t mix(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

NotChecked not_checked(std::string what, std::string why) {
  return NotChecked{std::move(what), std::move(why), 0};
}

}  // namespace

double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept {
  std::uint64_t bits = 0;
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    bits |= std::to_integer<std::uint64_t>(bytes[element * sizeof(double) + byte]) << (8U * byte);
  }
  return std::bit_cast<double>(bits);
}

std::array<std::uint32_t, spec::kSampledParticles> sampled_particles(std::uint64_t index) noexcept {
  std::array<std::uint32_t, spec::kSampledParticles> sample{};
  const std::span<std::uint32_t> out(sample);
  out[0] = 0;
  out[1] = static_cast<std::uint32_t>(spec::kParticles - 1);
  const std::uint64_t start = mix(index) % kInnerSpan;
  for (std::size_t i = 0; i < kInnerSamples; ++i) {
    out[2 + i] = static_cast<std::uint32_t>(1 + (start + i * kParticleStride) % kInnerSpan);
  }
  std::ranges::sort(sample);
  return sample;
}

Force direct_force(std::span<const double> positions, std::size_t particle) noexcept {
  const double x = positions[3 * particle];
  const double y = positions[3 * particle + 1];
  const double z = positions[3 * particle + 2];
  Force result;
  double fx = 0.0;
  double fy = 0.0;
  double fz = 0.0;
  double sx = 0.0;
  double sy = 0.0;
  double sz = 0.0;
  for (std::size_t other = 0; other < spec::kParticles; ++other) {
    if (other == particle) {
      continue;
    }
    const double dx = x - positions[3 * other];
    const double dy = y - positions[3 * other + 1];
    const double dz = z - positions[3 * other + 2];
    double q = dx * dx;
    q += dy * dy;
    q += dz * dz;
    if (q < spec::kCandidateLimit) {
      ++result.candidates;
    }
    const double t = 1.0 - q * 0.25;
    if (t > 0.0) {
      const double w = t * t;
      const double tx = w * dx;
      const double ty = w * dy;
      const double tz = w * dz;
      fx += tx;
      fy += ty;
      fz += tz;
      sx += std::fabs(tx);
      sy += std::fabs(ty);
      sz += std::fabs(tz);
    }
  }
  result.force = {fx, fy, fz};
  result.abs_sum = {sx, sy, sz};
  return result;
}

double tolerance_for(std::size_t candidates, double abs_sum) noexcept {
  if (candidates == 0) {
    return spec::kMinTolerance;
  }
  return std::max(spec::force_tolerance(candidates, abs_sum) * (1.0 + spec::kEvaluationSlack),
                  spec::kMinTolerance);
}

std::string_view ParticlesOracle::workload() const noexcept { return spec::kName; }

Contract ParticlesOracle::contract() const noexcept { return Contract::kBounded; }

Verdict ParticlesOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    return not_checked("particles unit",
                       "evidence is not 98304 bytes of input and 98304 of output");
  }

  std::array<double, kElements> storage{};
  const std::span<double> positions(storage);
  for (std::size_t e = 0; e < kElements; ++e) {
    positions[e] = load_double(evidence.input, e);
    // Written this way so that a NaN is outside: neither comparison holds for it.
    const bool inside = positions[e] >= 0.0 && positions[e] < spec::kBoxEdge;
    if (!inside) {
      return not_checked("particles tolerance",
                         "a coordinate is not finite or is outside the box, so the tolerance "
                         "cannot be derived");
    }
  }

  std::uint64_t compared = 0;
  for (const std::uint32_t particle : sampled_particles(evidence.index)) {
    const Force expected = direct_force(positions, particle);
    for (std::size_t component = 0; component < 3; ++component) {
      ++compared;
      const std::size_t element = 3 * std::size_t{particle} + component;
      const double residual =
          std::fabs(load_double(evidence.output, element) - expected.force.at(component));
      const double limit = tolerance_for(expected.candidates, expected.abs_sum.at(component));
      if (!(residual <= limit)) {
        return BoundedMismatch{"force-component", residual, limit, std::nullopt, element, compared};
      }
    }
  }
  return Agreement{compared};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& /*config*/, const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    std::string detail = "the plan's unit is not 4096 particles' positions in and forces out";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  if (!spec::is_cells_per_cutoff(plan.decomposition)) {
    std::string detail = "the plan's decomposition is not 1, 2 or 4 cells per cutoff";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  std::unique_ptr<Oracle> oracle = std::make_unique<ParticlesOracle>();
  return oracle;
}

}  // namespace loadforge::verification::oracle::particles
