// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_SPARSE_SPARSE_EVIDENCE_HPP
#define LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_SPARSE_SPARSE_EVIDENCE_HPP

// Evidence for the sparse oracle's tests, in every tier: a system built the way
// src/workloads/sparse/spec.hpp says, the output scipy's conjugate gradients
// computed for it (tests/fixtures/verification/oracle/sparse/), and the
// conversions between a system and the bytes of a unit's input and output.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "workloads/sparse/spec.hpp"

namespace loadforge::verification::oracle::sparse::evidence {

namespace spec = workloads::sparse;

/// The seed and unit index of the committed fixture (generate_fixtures.py).
inline constexpr std::uint64_t kSeed = 0x4C6F616446697263ULL;
inline constexpr std::uint64_t kFixtureIndex = 11;

struct Unit {
  std::uint64_t index = 0;
  std::vector<std::byte> input;
  std::vector<std::byte> output;
};

/// A system in CSR form: the four parts of the unit's input.
struct System {
  std::vector<double> values;
  std::vector<double> rhs;
  std::vector<std::uint32_t> columns;
  std::vector<std::uint32_t> row_ptr;
};

inline void append_double(std::vector<std::byte>& out, double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    out.push_back(static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU));
  }
}

inline void append_u32(std::vector<std::byte>& out, std::uint32_t word) {
  for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
    out.push_back(static_cast<std::byte>((word >> (8U * byte)) & 0xFFU));
  }
}

inline std::vector<std::byte> bytes_of(std::span<const double> values) {
  std::vector<std::byte> out;
  out.reserve(values.size() * sizeof(double));
  for (const double value : values) {
    append_double(out, value);
  }
  return out;
}

inline std::vector<double> doubles_of(std::span<const std::byte> bytes) {
  std::vector<double> out(bytes.size() / sizeof(double));
  for (std::size_t e = 0; e < out.size(); ++e) {
    std::uint64_t bits = 0;
    for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
      bits |= std::to_integer<std::uint64_t>(bytes[e * sizeof(double) + byte]) << (8U * byte);
    }
    out[e] = std::bit_cast<double>(bits);
  }
  return out;
}

/// The system with the fixed structure of the specification whose off-diagonal
/// pair (i, k) -- the entries (i, i + o_k) and (i + o_k, i) -- is `pair(i, k)`,
/// whose diagonal is 2 and whose right-hand side is `rhs(i)`.
inline System build_system(const std::function<double(std::size_t, std::size_t)>& pair,
                           const std::function<double(std::size_t)>& rhs) {
  System system;
  system.row_ptr.push_back(0);
  for (std::size_t i = 0; i < spec::kRows; ++i) {
    for (std::size_t k = spec::kOffsets.size(); k-- > 0;) {
      const std::size_t offset = spec::kOffsets.at(k);
      if (i >= offset) {
        system.columns.push_back(static_cast<std::uint32_t>(i - offset));
        system.values.push_back(pair(i - offset, k));
      }
    }
    system.columns.push_back(static_cast<std::uint32_t>(i));
    system.values.push_back(spec::kDiagonal);
    for (std::size_t k = 0; k < spec::kOffsets.size(); ++k) {
      const std::size_t offset = spec::kOffsets.at(k);
      if (i + offset < spec::kRows) {
        system.columns.push_back(static_cast<std::uint32_t>(i + offset));
        system.values.push_back(pair(i, k));
      }
    }
    system.row_ptr.push_back(static_cast<std::uint32_t>(system.columns.size()));
    system.rhs.push_back(rhs(i));
  }
  return system;
}

/// The system the specification generates for (seed, index).
inline System generated_system(std::uint64_t seed, std::uint64_t index) {
  return build_system(
      [&](std::size_t i, std::size_t k) {
        return spec::off_diagonal_value(rng::u64_of(rng::block_at(seed, index, 3 * i + k)));
      },
      [&](std::size_t i) {
        return spec::entry_value(rng::u64_of(rng::block_at(seed, index, 3 * spec::kRows + i)));
      });
}

/// A system whose off-diagonal values are all `value` and whose right-hand side
/// is `rhs`.
inline System uniform_system(double value, const std::function<double(std::size_t)>& rhs) {
  return build_system([&](std::size_t, std::size_t) { return value; }, rhs);
}

inline std::vector<std::byte> input_bytes(const System& system) {
  std::vector<std::byte> out;
  out.reserve(spec::kInputBytes);
  for (const double value : system.values) {
    append_double(out, value);
  }
  for (const double value : system.rhs) {
    append_double(out, value);
  }
  for (const std::uint32_t column : system.columns) {
    append_u32(out, column);
  }
  for (const std::uint32_t pointer : system.row_ptr) {
    append_u32(out, pointer);
  }
  return out;
}

inline std::vector<std::byte> output_bytes(std::span<const double> iterate,
                                           std::span<const double> residual) {
  std::vector<std::byte> out = bytes_of(iterate);
  const std::vector<std::byte> tail = bytes_of(residual);
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

/// r~ = b - A x and s~ = |b| + |A| |x|, row by row, in the order the oracle uses
/// (the right-hand side first, then the entries by increasing column, a plain
/// multiplication and subtraction apiece): the two agree to the bit.
struct Residual {
  std::vector<double> r;
  std::vector<double> s;
};

inline Residual explicit_residual(const System& system, std::span<const double> x) {
  Residual out;
  for (std::size_t i = 0; i < spec::kRows; ++i) {
    double residual = system.rhs[i];
    double abs_sum = std::fabs(residual);
    for (std::size_t e = system.row_ptr[i]; e < system.row_ptr[i + 1]; ++e) {
      const double product = system.values[e] * x[system.columns[e]];
      residual -= product;
      abs_sum += std::fabs(system.values[e]) * std::fabs(x[system.columns[e]]);
    }
    out.r.push_back(residual);
    out.s.push_back(abs_sum);
  }
  return out;
}

inline std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t at = 0; at < raw.size(); ++at) {
    out[at] = static_cast<std::byte>(static_cast<unsigned char>(raw[at]));
  }
  return out;
}

/// The unit whose iterate and residual scipy computed.
inline Unit real_tool_unit() {
  const std::string path = std::string(LOADFORGE_SOURCE_DIR) +
                           "/tests/fixtures/verification/oracle/sparse/unit-" +
                           std::to_string(kFixtureIndex) + ".output.bin";
  return Unit{kFixtureIndex, input_bytes(generated_system(kSeed, kFixtureIndex)), read_file(path)};
}

}  // namespace loadforge::verification::oracle::sparse::evidence

#endif
