// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/invariant/invariant.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>

#include "core/result.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/residual/residual.hpp"

namespace loadforge::verification::invariant {
namespace {

/// The tolerance for a model and magnitude, applied to a measured residual.
Bounded judged(double residual_value, residual::ErrorModel model, double magnitude) noexcept {
  const core::Result<double, residual::Underivable> tolerance =
      residual::tolerance(model, magnitude);
  if (!tolerance.has_value()) {
    return tolerance.error();
  }
  return residual::Comparison{residual_value, tolerance.value()};
}

}  // namespace

Bounded check_sum(double claimed, std::span<const double> values) noexcept {
  const residual::Accumulation total = residual::sum(values);
  return judged(std::abs(claimed - total.value), residual::sum_model(values.size()),
                total.magnitude);
}

Bounded check_squared_norm(double claimed, std::span<const double> values) noexcept {
  const residual::Accumulation total = residual::dot(values, values);
  return judged(std::abs(claimed - total.value), residual::dot_model(values.size()),
                total.magnitude);
}

Bounded check_conserved(std::span<const double> before, std::span<const double> after) noexcept {
  const residual::Accumulation total_before = residual::sum(before);
  const residual::Accumulation total_after = residual::sum(after);
  return judged(std::abs(total_after.value - total_before.value),
                residual::ErrorModel{std::max(before.size(), after.size()), 0},
                total_after.magnitude + total_before.magnitude);
}

std::optional<std::size_t> first_descent(std::span<const std::uint64_t> values) noexcept {
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i] < values[i - 1]) {
      return i;
    }
  }
  return std::nullopt;
}

bool operator==(const Fingerprint& lhs, const Fingerprint& rhs) noexcept {
  return std::tie(lhs.count, lhs.low, lhs.high) == std::tie(rhs.count, rhs.low, rhs.high);
}

Fingerprint fingerprint(std::span<const std::uint64_t> values) noexcept {
  const rng::Key key = rng::key_of(kFingerprintSeed);
  Fingerprint result;
  for (const std::uint64_t value : values) {
    const rng::Block block =
        rng::philox4x32_10(rng::Block{static_cast<std::uint32_t>(value),
                                      static_cast<std::uint32_t>(value >> 32U), 0, 0},
                           key);
    result.low += rng::u64_of(block);
    result.high += std::uint64_t{block[2]} | (std::uint64_t{block[3]} << 32U);
    ++result.count;
  }
  return result;
}

bool is_permutation(std::span<const std::uint64_t> lhs,
                    std::span<const std::uint64_t> rhs) noexcept {
  return fingerprint(lhs) == fingerprint(rhs);
}

}  // namespace loadforge::verification::invariant
