// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/monte_carlo/monte_carlo_oracle.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::verification::oracle {
namespace {

namespace spec = workloads::monte_carlo;

std::uint64_t load_le64(std::span<const std::byte> bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
    value |= std::to_integer<std::uint64_t>(bytes[at + byte]) << (8U * byte);
  }
  return value;
}

/// The smallest sampled batch whose sum is outside its tolerance.
struct Failure {
  std::uint64_t batch = 0;
  double residual = 0.0;
  double tolerance = 0.0;
};

// Built inside a noexcept function so the caller carries no cleanup for the
// two strings, which would hold a branch no test can take.
Verdict unchecked(const char* what, const char* why) noexcept { return NotChecked{what, why, 0}; }

class MonteCarloOracle final : public Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override { return spec::kName; }
  [[nodiscard]] Contract contract() const noexcept override { return Contract::kBounded; }

  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override {
    if (evidence.input.size() != spec::kInputBytes ||
        evidence.output.size() != spec::kOutputBytes) {
      return unchecked("monte carlo unit",
                       "evidence is not a monte carlo unit's input and output size");
    }
    std::optional<Failure> worst;
    for (std::size_t m = 0; m < spec::kSampledBatches; ++m) {
      const std::uint64_t batch = spec::sampled_batch(evidence.index, m);
      const double reference = replay_batch_sum(load_le64(evidence.input, batch * 8));
      const auto computed =
          std::bit_cast<double>(load_le64(evidence.output, batch * sizeof(double)));
      const double residual = std::abs(computed - reference);
      const double tolerance = spec::tolerance(reference);
      // The negation is the point: a NaN residual is not within tolerance.
      if (!(residual <= tolerance) && (!worst.has_value() || batch < worst->batch)) {
        worst = Failure{batch, residual, tolerance};
      }
    }
    if (worst.has_value()) {
      return BoundedMismatch{"batch " + std::to_string(worst->batch) + " sum",
                             worst->residual,
                             worst->tolerance,
                             std::nullopt,
                             worst->batch,
                             spec::kSampledBatches};
    }
    return Agreement{spec::kSampledBatches};
  }
};

}  // namespace

double replay_batch_sum(std::uint64_t key) {
  double sum = 0.0;
  double compensation = 0.0;
  for (std::size_t sample = 0; sample < spec::kSamplesPerBatch; ++sample) {
    const double term = spec::sample_value(spec::sample_bits(key, sample));
    const double next = sum + term;
    // Neumaier: the rounding error of this addition, whichever operand is larger.
    compensation += std::abs(sum) >= std::abs(term) ? (sum - next) + term : (term - next) + sum;
    sum = next;
  }
  return sum + compensation;
}

core::Result<std::unique_ptr<Oracle>, OracleError> make_monte_carlo_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan) {
  if (config.decomposition != 0 && !spec::is_chunk_length(config.decomposition)) {
    return OracleError{OracleFault::kConfiguration,
                       "chunk length " + std::to_string(config.decomposition) +
                           " is not one of 64, 256, 1024, 4096, 16384"};
  }
  if (!spec::is_chunk_length(plan.decomposition)) {
    return OracleError{OracleFault::kConfiguration,
                       "plan chunk length " + std::to_string(plan.decomposition) +
                           " is not one of 64, 256, 1024, 4096, 16384"};
  }
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    return OracleError{OracleFault::kConfiguration,
                       "plan sizes " + std::to_string(plan.input_bytes) + " and " +
                           std::to_string(plan.output_bytes) + " are not a monte carlo unit's " +
                           std::to_string(spec::kInputBytes) + " and " +
                           std::to_string(spec::kOutputBytes)};
  }
  return std::unique_ptr<Oracle>(std::make_unique<MonteCarloOracle>());
}

}  // namespace loadforge::verification::oracle
