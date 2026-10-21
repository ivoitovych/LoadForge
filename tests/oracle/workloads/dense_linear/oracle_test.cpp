// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the dense-linear workload: its output against naive dot products,
// written here from the definition c_ij = sum_k a_ik b_kj and never from a
// kernel, with the bound derived in support.hpp from spec.hpp's. The
// verification stream writes the oracle the runner uses
// (src/verification/oracle/dense_linear/); this is the workload's own
// evidence, independent of it and of the kernels alike.
//
//   * Sampled entries -- a function of the unit index alone, plus the four
//     corners -- on every path and every depth, for two units.
//   * Every entry of one unit on every path, at the default depth.
//   * The bound is not vacuous: the worst entry uses a measurable part of it,
//     and the tolerance stays a small multiple of n u s.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/dense_linear/support.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

using simd::IsaPath;

/// Which entries a unit's check samples: the corners, then 60 drawn from the
/// unit index alone.
std::vector<std::pair<std::size_t, std::size_t>> sampled(std::uint64_t index) {
  std::vector<std::pair<std::size_t, std::size_t>> entries = {
      {0, 0}, {0, kN - 1}, {kN - 1, 0}, {kN - 1, kN - 1}};
  for (std::uint64_t draw = 0; draw < 60; ++draw) {
    const rng::Block block = rng::block_at(0x0AC1E, index, draw);
    entries.emplace_back(block[0] % kN, block[1] % kN);
  }
  return entries;
}

/// c' and s for every entry, as plain doubles: the same loops as reference(),
/// over arrays rather than bytes.
struct Full {
  std::vector<double> c;
  std::vector<double> s;
};

Full full_reference(const Bytes& input) {
  std::vector<double> a(kMatrix);
  std::vector<double> b(kMatrix);
  for (std::size_t e = 0; e < kMatrix; ++e) {
    a[e] = get(input, e);
    b[e] = get(input, kMatrix + e);
  }
  Full full{std::vector<double>(kMatrix, 0.0), std::vector<double>(kMatrix, 0.0)};
  for (std::size_t i = 0; i < kN; ++i) {
    for (std::size_t k = 0; k < kN; ++k) {
      const double aik = a[i * kN + k];
      const double* row = b.data() + k * kN;
      double* c = full.c.data() + i * kN;
      double* s = full.s.data() + i * kN;
      for (std::size_t j = 0; j < kN; ++j) {
        const double product = aik * row[j];
        c[j] = c[j] + product;
        const double magnitude = std::fabs(aik) * std::fabs(row[j]);
        s[j] = s[j] + magnitude;
      }
    }
  }
  return full;
}

TEST(DenseLinearOracle, SampledEntriesAreWithinTheBoundOnEveryPathAndDepth) {
  for (const IsaPath path : runnable_paths()) {
    for (const std::uint32_t depth : kPanelDepths) {
      const auto workload = make(config_for(path, depth, 31));
      ASSERT_NE(workload, nullptr);
      for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{1}}) {
        const Bytes input = generated(*workload, index);
        Serial serial;
        const Bytes output = computed(*workload, index, input, serial);
        for (const auto& [i, j] : sampled(index)) {
          const Reference r = reference(input, i, j);
          EXPECT_TRUE(within_bound(c_of(output, i, j), r.c, r.s))
              << simd::describe(path) << " depth " << depth << " unit " << index << ": c(" << i
              << ", " << j << ") = " << c_of(output, i, j) << ", naive " << r.c << ", tolerance "
              << tolerance(r.s);
        }
      }
    }
  }
}

TEST(DenseLinearOracle, EveryEntryOfAUnitIsWithinTheBoundOnEveryPath) {
  for (const IsaPath path : runnable_paths()) {
    const auto workload = make(config_for(path, 0, 32));
    ASSERT_NE(workload, nullptr);
    const Bytes input = generated(*workload, 4);
    Shuffled executor(3, 4);
    const Bytes output = computed(*workload, 4, input, executor);
    const Full naive = full_reference(input);
    double worst = 0.0;
    for (std::size_t e = 0; e < kMatrix; ++e) {
      ASSERT_TRUE(within_bound(get(output, e), naive.c[e], naive.s[e]))
          << simd::describe(path) << ": entry " << e;
      worst = std::max(worst, std::fabs(get(output, e) - naive.c[e]) / tolerance(naive.s[e]));
    }
    std::cout << "  " << simd::describe(path)
              << ": worst |c - c'| / tolerance over the unit = " << worst << "\n";
    // Not vacuous: the two results do differ, by a measurable part of the
    // bound -- a bound a million times too loose would pass anything.
    EXPECT_GT(worst, 1e-6) << simd::describe(path);
    EXPECT_LT(worst, 1.0) << simd::describe(path);
  }
}

TEST(DenseLinearOracle, TheToleranceIsTwiceGammaNOfSAndNoLooser) {
  const double g = gamma(kN);
  const double nu = 256.0 / 9007199254740992.0;
  EXPECT_NEAR(g / nu, 1.0, 1e-13) << "gamma_256 is n u to first order";
  for (const double s : {1e-3, 1.0, 64.0, 256.0}) {
    EXPECT_GE(tolerance(s), 2.0 * g * s);
    EXPECT_LE(tolerance(s), 2.0 * g * s * (1.0 + 1e-12)) << s;
  }
}

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
