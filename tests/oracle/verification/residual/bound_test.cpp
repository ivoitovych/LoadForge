// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for src/verification/residual: the derivation's premises and its
// conclusion, checked against exact arithmetic on the dense-linear
// specification's own inputs.
//
//   * Premise (1), Higham's bound: every evaluation order a correct kernel may
//     use (tests/unit/verification/residual/orders.hpp) lands within
//     gamma_n S of the EXACT dot product, computed in 128-bit integers.
//   * Conclusion: the oracle's comparison holds for every such order -- the
//     derived tolerance never accuses a correct evaluation.
//
// The worst ratios seen are printed, for the journal: how much of the bound
// real rounding uses.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/verification/residual/exact.hpp"
#include "unit/verification/residual/orders.hpp"
#include "verification/residual/residual.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::verification::residual {
namespace {

namespace spec = workloads::dense_linear;
constexpr std::size_t kN = spec::kOrder;
constexpr std::uint64_t kSeed = 0x0123456789ABCDEFULL;

/// n spec entries from stream `stream`, starting at `position`.
std::vector<double> entries(std::uint64_t stream, std::uint64_t position, std::size_t n) {
  std::vector<double> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    out[k] = spec::entry_value(rng::u64_of(rng::block_at(kSeed, stream, position + k)));
  }
  return out;
}

struct Worst {
  double premise = 0.0;     // |c^ - c| / (gamma_n S), exact c and S
  double conclusion = 0.0;  // r^ / T^
};

void check_pair(const std::vector<double>& x, const std::vector<double>& y, Worst& worst) {
  const exact::Dot truth = exact::dot(x, y);
  const double s = exact::magnitude_of(truth.magnitude);
  const Accumulation oracle = dot(x, y);
  const double t = tolerance(dot_model(x.size()), oracle.magnitude).value();
  for (const orders::Order& order : orders::all(spec::kPanelDepths)) {
    const double computed = orders::evaluate(order, x, y);
    const exact::Int128 error = exact::to_count(computed) - truth.value;
    const double premise = exact::magnitude_of(error) / (gamma(x.size()) * s);
    EXPECT_LE(premise, 1.0) << orders::name_of(order);
    const Comparison comparison = compare(computed, oracle.value, t);
    EXPECT_TRUE(within(comparison))
        << orders::name_of(order) << ": " << comparison.residual << " > " << comparison.tolerance;
    worst.premise = std::max(worst.premise, premise);
    worst.conclusion = std::max(worst.conclusion, comparison.residual / comparison.tolerance);
  }
}

TEST(ResidualBound, HoldsForEveryOrderOnSpecInputsAgainstExactArithmetic) {
  Worst worst;
  for (std::uint64_t unit = 0; unit < 48; ++unit) {
    check_pair(entries(unit, 0, kN), entries(unit, kN, kN), worst);
  }
  std::cout << "spec inputs: |c^ - c| / (gamma_n S) at most " << worst.premise
            << "; r^ / T^ at most " << worst.conclusion << "\n";
}

TEST(ResidualBound, HoldsWhenTheSumCancels) {
  // y = x reversed with alternating signs: products of both signs that
  // cancel, so the exact value is far below S -- the case a relative bound on
  // |c| would get wrong and a bound on S does not.
  Worst worst;
  for (std::uint64_t unit = 0; unit < 16; ++unit) {
    const std::vector<double> x = entries(100 + unit, 0, kN);
    std::vector<double> y(kN);
    for (std::size_t k = 0; k < kN; ++k) {
      y[k] = (k % 2 == 0 ? 1.0 : -1.0) * x[kN - 1 - k];
    }
    check_pair(x, y, worst);
    std::vector<double> negated = x;
    for (std::size_t k = kN / 2; k < kN; ++k) {
      negated[k] = -x[k - kN / 2];
    }
    std::vector<double> half(x.begin(), x.begin() + kN / 2);
    half.insert(half.end(), x.begin(), x.begin() + kN / 2);
    check_pair(half, negated, worst);  // exactly zero in exact arithmetic
  }
  std::cout << "cancelling inputs: |c^ - c| / (gamma_n S) at most " << worst.premise
            << "; r^ / T^ at most " << worst.conclusion << "\n";
}

TEST(ResidualBound, HoldsAtEveryLengthAPanelCanLeave) {
  // Lengths 1 to 33 and the panel boundaries: the depth of the tree changes
  // with the length, and dot_model(n) must follow it.
  Worst worst;
  std::vector<std::size_t> lengths;
  for (std::size_t n = 1; n <= 33; ++n) {
    lengths.push_back(n);
  }
  for (const std::size_t n : {63U, 64U, 65U, 127U, 128U, 129U, 255U, 256U}) {
    lengths.push_back(n);
  }
  for (const std::size_t n : lengths) {
    check_pair(entries(200 + n, 0, n), entries(200 + n, n, n), worst);
  }
  std::cout << "short inputs: |c^ - c| / (gamma_n S) at most " << worst.premise
            << "; r^ / T^ at most " << worst.conclusion << "\n";
}

}  // namespace
}  // namespace loadforge::verification::residual
