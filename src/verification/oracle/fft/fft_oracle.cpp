// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/fft/fft_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <optional>
#include <string>
#include <utility>

#include "verification/oracle/fft/twiddle_table.hpp"

namespace loadforge::verification::oracle::fft {
namespace {

namespace spec = workloads::fft;

constexpr std::size_t kElements = 2 * spec::kPoints;
/// sqrt(n), exactly: n = 4096.
constexpr double kRootPoints = 64.0;
static_assert(kRootPoints * kRootPoints == static_cast<double>(spec::kPoints));

/// The stride that spreads a unit's sampled bins; odd and prime, so coprime to
/// both halves' moduli, which are 2047 = 23 * 89.
constexpr std::size_t kBinStride = 293;
constexpr std::size_t kHalfBins = spec::kSampledBins / 2 - 1;
constexpr std::uint64_t kHalfModulus = spec::kPoints / 2 - 1;
static_assert(kHalfBins * 2 + 2 == spec::kSampledBins);
static_assert(kHalfModulus == 2047 && std::gcd(kBinStride, kHalfModulus) == 1);

constexpr std::uint64_t kUpperSalt = 0xA5A5A5A5A5A5A5A5ULL;

/// The splitmix64 finaliser: a bijective mixing of 64 bits, unsigned throughout.
constexpr std::uint64_t mix(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

/// max(|a|, |b|), NaN if either is: a NaN must fail the comparison it feeds.
double larger_magnitude(double a, double b) noexcept {
  const double ra = std::fabs(a);
  const double rb = std::fabs(b);
  return (ra >= rb || std::isnan(ra)) ? ra : rb;
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

Complex twiddle(std::size_t m) noexcept {
  const std::span<const std::uint64_t> table(kTwiddleBits);
  return Complex{std::bit_cast<double>(table[2 * m]), std::bit_cast<double>(table[2 * m + 1])};
}

std::array<std::uint32_t, spec::kSampledBins> sampled_bins(std::uint64_t index) noexcept {
  std::array<std::uint32_t, spec::kSampledBins> bins{};
  const std::span<std::uint32_t> out(bins);
  out[0] = 0;
  out[1] = static_cast<std::uint32_t>(spec::kPoints / 2);
  const std::uint64_t lower = mix(index) % kHalfModulus;
  const std::uint64_t upper = mix(index ^ kUpperSalt) % kHalfModulus;
  for (std::size_t i = 0; i < kHalfBins; ++i) {
    out[2 + i] = static_cast<std::uint32_t>(1 + (lower + i * kBinStride) % kHalfModulus);
    out[2 + kHalfBins + i] =
        static_cast<std::uint32_t>(spec::kPoints / 2 + 1 + (upper + i * kBinStride) % kHalfModulus);
  }
  std::ranges::sort(bins);
  return bins;
}

Complex direct_dft_bin(std::span<const double> x, std::size_t k) noexcept {
  Complex sum;
  for (std::size_t j = 0; j < spec::kPoints; ++j) {
    const Complex w = twiddle((j * k) % spec::kPoints);
    const double re = x[2 * j];
    const double im = x[2 * j + 1];
    sum.re += re * w.re - im * w.im;
    sum.im += re * w.im + im * w.re;
  }
  return sum;
}

double bin_tolerance(std::uint32_t radix, double norm) noexcept {
  return kRootPoints * norm * spec::bin_tolerance_factor(radix) * (1.0 + spec::kEvaluationSlack);
}

double parseval_tolerance(std::uint32_t radix, double norm) noexcept {
  return kRootPoints * norm * spec::parseval_tolerance_factor(radix) *
         (1.0 + spec::kEvaluationSlack);
}

std::string_view FftOracle::workload() const noexcept { return spec::kName; }

Contract FftOracle::contract() const noexcept { return Contract::kBounded; }

Verdict FftOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    return not_checked("fft unit", "evidence is not 65536 bytes of input and 65536 of output");
  }

  std::array<double, kElements> storage{};
  const std::span<double> x(storage);
  double input_squares = 0.0;
  bool usable = true;
  for (std::size_t e = 0; e < kElements; ++e) {
    x[e] = load_double(evidence.input, e);
    usable = usable && std::isfinite(x[e]) && std::fabs(x[e]) <= 1.0;
    input_squares += x[e] * x[e];
  }
  if (!usable) {
    return not_checked("fft tolerance",
                       "an input component is not finite or has magnitude above 1, so the "
                       "tolerance cannot be derived");
  }
  // Every component is at most 1, so the sum is finite and is zero only when
  // every component is: a squared multiple of 2^-52 does not underflow.
  if (input_squares == 0.0) {
    return not_checked("fft tolerance", "every input component is zero, so the tolerance is zero");
  }
  const double norm = std::sqrt(input_squares);
  const double bin_limit = bin_tolerance(radix_, norm);

  std::uint64_t compared = 0;
  for (const std::uint32_t k : sampled_bins(evidence.index)) {
    ++compared;
    const Complex expected = direct_dft_bin(x, k);
    const double re = load_double(evidence.output, 2 * static_cast<std::size_t>(k));
    const double im = load_double(evidence.output, 2 * static_cast<std::size_t>(k) + 1);
    const double residual = larger_magnitude(re - expected.re, im - expected.im);
    if (!(residual <= bin_limit)) {
      return BoundedMismatch{"dft-bin", residual, bin_limit, std::nullopt, k, compared};
    }
  }

  ++compared;
  double output_squares = 0.0;
  for (std::size_t e = 0; e < kElements; ++e) {
    const double value = load_double(evidence.output, e);
    output_squares += value * value;
  }
  const double residual = std::fabs(std::sqrt(output_squares) - kRootPoints * norm);
  const double parseval_limit = parseval_tolerance(radix_, norm);
  if (!(residual <= parseval_limit)) {
    return BoundedMismatch{"parseval",   residual,     parseval_limit,
                           std::nullopt, std::nullopt, compared};
  }
  return Agreement{compared};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& /*config*/, const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    std::string detail = "the plan's unit is not 4096 complex points in and out";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  if (!spec::is_radix(plan.decomposition)) {
    std::string detail = "the plan's decomposition is not radix 2 or 4";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  std::unique_ptr<Oracle> oracle = std::make_unique<FftOracle>(plan.decomposition);
  return oracle;
}

}  // namespace loadforge::verification::oracle::fft
