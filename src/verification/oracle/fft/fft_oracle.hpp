// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_FFT_FFT_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_FFT_FFT_ORACLE_HPP

// The independent oracle of the FFT workload (verification.md §3, §5), written
// from src/workloads/fft/spec.hpp and from nothing else: a direct O(n^2) DFT
// of the input at kSampledBins bins, then Parseval over every output bin, each
// judged against a tolerance derived in the specification. Contract::kBounded.
//
// The oracle holds no state but the radix its tolerances are derived for, and
// allocates nothing: the input is decoded onto the stack, the output is read
// in place. check() is therefore const, re-entrant and cannot fail to
// allocate scratch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/fft/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::fft {

/// A complex number, as two doubles.
struct Complex {
  double re = 0.0;
  double im = 0.0;
};

/// W[m], the specification's twiddle factor, for m in [0, kPoints).
[[nodiscard]] Complex twiddle(std::size_t m) noexcept;

/// The kSampledBins distinct bins a unit's check examines, in increasing k.
/// Always includes 0 and kPoints / 2; the rest are a function of `index` alone.
[[nodiscard]] std::array<std::uint32_t, workloads::fft::kSampledBins> sampled_bins(
    std::uint64_t index) noexcept;

/// X'_k = sum_j x_j W[jk mod n], by plain scalar loops in increasing j.
/// `x` holds 2 kPoints doubles, re_0, im_0, re_1, ...; k < kPoints.
[[nodiscard]] Complex direct_dft_bin(std::span<const double> x, std::size_t k) noexcept;

/// T' of a "dft-bin" verdict for an input of 2-norm `norm`, radix `radix`:
/// sqrt(n) * norm * bin_tolerance_factor(radix) * (1 + kEvaluationSlack).
[[nodiscard]] double bin_tolerance(std::uint32_t radix, double norm) noexcept;

/// T' of the "parseval" verdict, likewise.
[[nodiscard]] double parseval_tolerance(std::uint32_t radix, double norm) noexcept;

/// The little-endian double at `element` of a span of doubles stored as bytes.
/// Precondition: (element + 1) * 8 <= bytes.size().
[[nodiscard]] double load_double(std::span<const std::byte> bytes, std::size_t element) noexcept;

class FftOracle final : public Oracle {
 public:
  /// Precondition: workloads::fft::is_radix(radix); create() checks it.
  explicit FftOracle(std::uint32_t radix) noexcept : radix_(radix) {}

  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;

 private:
  std::uint32_t radix_;
};

/// The oracle for a plan the FFT workload resolved: kConfiguration when the
/// plan's sizes are not a unit of the specification or its decomposition is
/// not one of its radices.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::fft

#endif
