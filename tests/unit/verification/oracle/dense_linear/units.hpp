// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_DENSE_LINEAR_UNITS_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_DENSE_LINEAR_UNITS_HPP

// Dense-linear units for the oracle's tests, built from the specification and
// exact arithmetic, never from a kernel (verification.md §2): the input as
// spec.hpp defines it, and the output as the EXACT product C = AB rounded once
// to the nearest double -- every entry of A and B is a multiple of 2^-52, so
// the product is exact in 128-bit integers (residual/exact.hpp). Other
// outputs a correct kernel may write come from the evaluation orders of
// residual/orders.hpp, applied entry by entry.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "unit/verification/residual/exact.hpp"
#include "unit/verification/residual/orders.hpp"
#include "workloads/dense_linear/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::units {

namespace spec = workloads::dense_linear;
inline constexpr std::size_t kN = spec::kOrder;
inline constexpr std::size_t kEntries = kN * kN;
inline constexpr std::uint64_t kSeed = 0x00C0FFEE00C0FFEEULL;

/// A unit: A and B row-major, and an output C row-major.
struct Unit {
  std::uint64_t index = 0;
  std::vector<double> a;
  std::vector<double> b;
  std::vector<double> c;
};

/// A and B as the specification generates them for (seed, index): element e
/// of the unit, A's entries first, is entry_value(u64_of(block_at(seed,
/// index, e))).
inline Unit spec_input(std::uint64_t seed, std::uint64_t index) {
  Unit unit{index, std::vector<double>(kEntries), std::vector<double>(kEntries), {}};
  for (std::size_t e = 0; e < kEntries; ++e) {
    unit.a[e] = spec::entry_value(rng::u64_of(rng::block_at(seed, index, e)));
    unit.b[e] = spec::entry_value(rng::u64_of(rng::block_at(seed, index, kEntries + e)));
  }
  return unit;
}

/// C = AB exactly, then each entry rounded once.
inline std::vector<double> exact_product(const std::vector<double>& a,
                                         const std::vector<double>& b) {
  std::vector<std::int64_t> sa(kEntries);
  std::vector<std::int64_t> sb(kEntries);
  for (std::size_t e = 0; e < kEntries; ++e) {
    sa[e] = exact::scaled52(a[e]);
    sb[e] = exact::scaled52(b[e]);
  }
  std::vector<double> c(kEntries);
  std::vector<exact::Int128> row(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    std::fill(row.begin(), row.end(), exact::Int128{0});
    for (std::size_t k = 0; k < kN; ++k) {
      const exact::Int128 aik = sa[i * kN + k];
      for (std::size_t j = 0; j < kN; ++j) {
        row[j] += aik * sb[k * kN + j];
      }
    }
    for (std::size_t j = 0; j < kN; ++j) {
      c[i * kN + j] = exact::to_double(row[j]);
    }
  }
  return c;
}

/// Row i of A and column j of B.
inline std::vector<double> row_of(const Unit& unit, std::size_t i) {
  return {unit.a.begin() + static_cast<std::ptrdiff_t>(i * kN),
          unit.a.begin() + static_cast<std::ptrdiff_t>((i + 1) * kN)};
}
inline std::vector<double> column_of(const Unit& unit, std::size_t j) {
  std::vector<double> column(kN);
  for (std::size_t k = 0; k < kN; ++k) {
    column[k] = unit.b[k * kN + j];
  }
  return column;
}

/// C as a kernel summing in `order` would write it.
inline std::vector<double> product_in(const Unit& unit, const orders::Order& order) {
  std::vector<double> c(kEntries);
  std::vector<std::vector<double>> columns(kN);
  for (std::size_t j = 0; j < kN; ++j) {
    columns[j] = column_of(unit, j);
  }
  for (std::size_t i = 0; i < kN; ++i) {
    const std::vector<double> row = row_of(unit, i);
    for (std::size_t j = 0; j < kN; ++j) {
      c[i * kN + j] = orders::evaluate(order, row, columns[j]);
    }
  }
  return c;
}

/// A spec unit with its exactly rounded product.
inline Unit spec_unit(std::uint64_t index) {
  Unit unit = spec_input(kSeed, index);
  unit.c = exact_product(unit.a, unit.b);
  return unit;
}

/// Little-endian bytes of doubles, as the specification lays them out.
inline void append_bytes(std::vector<std::byte>& out, std::span<const double> values) {
  for (const double value : values) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
      out.push_back(static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU));
    }
  }
}

/// A unit's evidence as bytes, owned: the shape of contracts::OracleCase,
/// declared here so that this header does not register the contract's suites
/// in every test binary that includes it.
struct Case {
  std::uint64_t index = 0;
  std::vector<std::byte> input;
  std::vector<std::byte> output;
};

inline Case to_case(const Unit& unit) {
  Case result{unit.index, {}, {}};
  result.input.reserve(spec::kInputBytes);
  append_bytes(result.input, unit.a);
  append_bytes(result.input, unit.b);
  result.output.reserve(spec::kOutputBytes);
  append_bytes(result.output, unit.c);
  return result;
}

inline workloads::Plan plan(std::uint32_t kc = spec::kDefaultPanelDepth) {
  return workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, kc, 0, 0};
}

}  // namespace loadforge::verification::units

#endif
