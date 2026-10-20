// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the workload against a scalar SpMV and the explicit residual, and against
// a solution found another way.
//
//   * the matrix generate() writes is rebuilt here from the specification's
//     own words -- by pairs (i, k), both a(i, i + o_k) and a(i + o_k, i) --
//     where generate() works by rows;
//   * the output is checked as the specification's oracle checks it
//     (spec_check.hpp): n row checks and the residual norm, with the derived
//     tolerances;
//   * the iterate is compared with the solution of A x = b found by conjugate
//     gradients run to convergence in long double, a different precision and
//     many more steps, from the matrix built above and not from the input.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "oracle/workloads/sparse/spec_check.hpp"
#include "oracle/workloads/sparse/unit_run.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/workloads/sparse/executors.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {
namespace {

using tests::Bytes;

struct Entry {
  std::size_t column;
  double value;
};

/// The matrix and right-hand side of unit `index`, as the specification
/// describes them.
struct Reference {
  std::vector<std::vector<Entry>> rows = std::vector<std::vector<Entry>>(kRows);
  std::vector<double> b = std::vector<double>(kRows);

  Reference(std::uint64_t seed, std::uint64_t index) {
    for (std::size_t i = 0; i < kRows; ++i) {
      rows[i].push_back(Entry{i, kDiagonal});
      for (std::size_t k = 0; k < kOffsets.size(); ++k) {
        if (i + kOffsets[k] < kRows) {
          const double value =
              entry_value(rng::u64_of(rng::block_at(seed, index, 3 * i + k))) / 8.0;
          rows[i].push_back(Entry{i + kOffsets[k], value});
          rows[i + kOffsets[k]].push_back(Entry{i, value});
        }
      }
      b[i] = entry_value(rng::u64_of(rng::block_at(seed, index, 3 * kRows + i)));
    }
    for (auto& row : rows) {
      std::ranges::sort(row, {}, &Entry::column);
    }
  }

  [[nodiscard]] std::vector<long double> multiply(const std::vector<long double>& x) const {
    std::vector<long double> y(kRows, 0.0L);
    for (std::size_t i = 0; i < kRows; ++i) {
      for (const Entry& e : rows[i]) {
        y[i] += static_cast<long double>(e.value) * x[e.column];
      }
    }
    return y;
  }

  /// Conjugate gradients to convergence in long double.
  [[nodiscard]] std::vector<long double> solve() const {
    std::vector<long double> x(kRows, 0.0L);
    std::vector<long double> r(b.begin(), b.end());
    std::vector<long double> p = r;
    long double rr = 0.0L;
    for (const long double v : r) {
      rr += v * v;
    }
    for (int iteration = 0; iteration < 60 && rr > 0.0L; ++iteration) {
      const std::vector<long double> q = multiply(p);
      long double pq = 0.0L;
      for (std::size_t i = 0; i < kRows; ++i) {
        pq += p[i] * q[i];
      }
      const long double alpha = rr / pq;
      long double next = 0.0L;
      for (std::size_t i = 0; i < kRows; ++i) {
        x[i] += alpha * p[i];
        r[i] -= alpha * q[i];
        next += r[i] * r[i];
      }
      const long double beta = next / rr;
      rr = next;
      for (std::size_t i = 0; i < kRows; ++i) {
        p[i] = r[i] + beta * p[i];
      }
    }
    return x;
  }
};

TEST(SparseOracle, GenerateWritesTheMatrixTheSpecificationDescribes) {
  for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{0x1234567}, ~std::uint64_t{0}}) {
    for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{7}}) {
      const auto workload = tests::make_workload(seed, 0);
      ASSERT_NE(workload, nullptr);
      const Bytes input = tests::generate_input(*workload, index);
      const Reference reference(seed, index);
      std::size_t entry = 0;
      for (std::size_t i = 0; i < kRows; ++i) {
        ASSERT_EQ(check::u32_at(input, kRowPtrOffset, i), entry);
        for (const Entry& e : reference.rows[i]) {
          ASSERT_EQ(check::u32_at(input, kColumnsOffset, entry), e.column) << i;
          ASSERT_EQ(check::f64_at(input, kValuesOffset, entry), e.value) << i << "," << e.column;
          ++entry;
        }
        ASSERT_EQ(check::f64_at(input, kRhsOffset, i), reference.b[i]) << i;
      }
      ASSERT_EQ(entry, kNonZeros);
    }
  }
}

TEST(SparseOracle, TheOutputIsAgreedByTheSpecificationsCheckAtEveryChunkSize) {
  sparse_tests::SerialExecutor serial;
  for (const std::uint32_t rows : kChunkRows) {
    for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{2}}) {
      const auto workload = tests::make_workload(seed, rows);
      ASSERT_NE(workload, nullptr);
      const Bytes input = tests::generate_input(*workload, 3);
      const Bytes output = tests::solve(*workload, 3, input, serial);
      const check::Outcome outcome = check::check(input, output);
      EXPECT_EQ(outcome.verdict, check::Verdict::kAgreement)
          << "chunk rows " << rows << " seed " << seed << ": " << outcome.check << " "
          << outcome.element << " " << outcome.residual << " > " << outcome.tolerance;
      EXPECT_EQ(outcome.blocks_compared, kRows + 1);
    }
  }
}

TEST(SparseOracle, TheIterateIsTheSolutionToWithinTheErrorTheResidualAllows) {
  sparse_tests::SerialExecutor serial;
  const auto workload = tests::make_workload(11, 0);
  ASSERT_NE(workload, nullptr);
  const Bytes input = tests::generate_input(*workload, 5);
  const Bytes output = tests::solve(*workload, 5, input, serial);
  const Reference reference(11, 5);
  const std::vector<long double> exact = reference.solve();

  long double largest = 0.0L;
  for (std::size_t i = 0; i < kRows; ++i) {
    largest = std::max(largest, std::fabs(static_cast<long double>(
                                              check::f64_at(output, 0, i)) -
                                          exact[i]));
  }
  // x^ - x* = A^-1 r, so |x^ - x*|_inf <= |r|_2 / lambda_min, lambda_min >= 5/4.
  const double tolerance = residual_norm_factor() * 1.0 * std::sqrt(static_cast<double>(kRows)) / 1.25;
  EXPECT_LE(largest, tolerance);
  EXPECT_LE(largest, 1e-13L) << "and in fact the iterate is the solution to roundoff";

  // The solution found here solves the system: the residual of long double CG is tiny.
  const std::vector<long double> product = reference.multiply(exact);
  for (std::size_t i = 0; i < kRows; ++i) {
    ASSERT_NEAR(static_cast<double>(product[i]), reference.b[i], 1e-14) << i;
  }
}

TEST(SparseOracle, TheExplicitResidualIsBMinusAXToRoundoffAndFarBelowTheBound) {
  sparse_tests::SerialExecutor serial;
  double largest_ratio = 0.0;
  for (const std::uint64_t seed : {std::uint64_t{21}, std::uint64_t{22}, std::uint64_t{23},
                                   std::uint64_t{24}}) {
    const auto workload = tests::make_workload(seed, 0);
    ASSERT_NE(workload, nullptr);
    const Bytes input = tests::generate_input(*workload, 0);
    const Bytes output = tests::solve(*workload, 0, input, serial);
    double b_squares = 0.0;
    double r_squares = 0.0;
    for (std::size_t i = 0; i < kRows; ++i) {
      const double b = check::f64_at(input, kRhsOffset, i);
      const double r = check::f64_at(output, kRows * sizeof(double), i);
      b_squares += b * b;
      r_squares += r * r;
    }
    const double ratio = std::sqrt(r_squares) / std::sqrt(b_squares);
    largest_ratio = std::max(largest_ratio, ratio);
    EXPECT_LE(ratio, residual_norm_factor()) << "the bound of derivation 2";
  }
  EXPECT_LE(largest_ratio, 1e-14) << "measured: the attainable residual, 1e-16 and not 1e-11";
}

}  // namespace
}  // namespace loadforge::workloads::sparse
