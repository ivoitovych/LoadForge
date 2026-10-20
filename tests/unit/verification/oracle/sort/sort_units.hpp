// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_VERIFICATION_ORACLE_SORT_SORT_UNITS_HPP
#define LOADFORGE_TESTS_VERIFICATION_ORACLE_SORT_SORT_UNITS_HPP

// Sort units for the oracle's tests. The right output of a unit is built with
// std::sort over (key, input position) pairs -- the standard library's sort,
// a tool the oracle is forbidden to be -- from keys the specification's
// generator gives, or from keys chosen to sit at the ends of the order.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::tests::sort_units {

namespace spec = workloads::sort;

inline void put64(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value) {
  for (std::size_t byte = 0; byte < 8; ++byte) {
    bytes[at + byte] = static_cast<std::byte>(value >> (8U * byte));
  }
}

inline void put32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    bytes[at + byte] = static_cast<std::byte>(value >> (8U * byte));
  }
}

inline std::uint64_t get64(const std::vector<std::byte>& bytes, std::size_t at) {
  return spec::load_le64(bytes, at);
}

inline std::uint32_t get32(const std::vector<std::byte>& bytes, std::size_t at) {
  return spec::load_le32(bytes, at);
}

inline std::vector<std::uint64_t> generated_keys(std::uint64_t seed, std::uint64_t index) {
  std::vector<std::uint64_t> keys(spec::kKeys);
  for (std::size_t position = 0; position < spec::kKeys; ++position) {
    keys[position] = spec::key_at(seed, index, position);
  }
  return keys;
}

/// Keys drawn from `values` by a small deterministic generator: many ties.
inline std::vector<std::uint64_t> keys_from(const std::vector<std::uint64_t>& values,
                                            std::uint64_t seed) {
  std::vector<std::uint64_t> keys(spec::kKeys);
  std::uint64_t state = seed * 2 + 1;
  for (std::uint64_t& key : keys) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    key = values[(state >> 33U) % values.size()];
  }
  return keys;
}

inline std::vector<std::byte> input_of(const std::vector<std::uint64_t>& keys) {
  std::vector<std::byte> input(spec::kInputBytes);
  for (std::size_t position = 0; position < keys.size(); ++position) {
    put64(input, position * 8, keys[position]);
  }
  return input;
}

/// The right output for `keys`: the pairs sorted by key, then input position.
inline std::vector<std::byte> sorted_output_of(const std::vector<std::uint64_t>& keys) {
  std::vector<std::pair<std::uint64_t, std::uint32_t>> pairs;
  pairs.reserve(keys.size());
  for (std::size_t position = 0; position < keys.size(); ++position) {
    pairs.emplace_back(keys[position], static_cast<std::uint32_t>(position));
  }
  std::sort(pairs.begin(), pairs.end());
  std::vector<std::byte> output(spec::kOutputBytes);
  for (std::size_t position = 0; position < pairs.size(); ++position) {
    put64(output, spec::key_offset(position), pairs[position].first);
    put32(output, spec::index_offset(position), pairs[position].second);
  }
  return output;
}

inline contracts::OracleCase unit_of(std::uint64_t index, const std::vector<std::uint64_t>& keys) {
  return contracts::OracleCase{index, input_of(keys), sorted_output_of(keys)};
}

inline std::vector<std::uint64_t> descending_keys() {
  std::vector<std::uint64_t> keys(spec::kKeys);
  for (std::size_t position = 0; position < keys.size(); ++position) {
    keys[position] = ~std::uint64_t{0} - position * 3;
  }
  return keys;
}

inline std::vector<std::uint64_t> ascending_keys() {
  std::vector<std::uint64_t> keys(spec::kKeys);
  for (std::size_t position = 0; position < keys.size(); ++position) {
    keys[position] = position * 5;
  }
  return keys;
}

/// The units a right output is shown for: random keys, every key equal, keys
/// at both ends of the unsigned range (and either side of the sign bit), keys
/// already in order, keys in reverse order.
inline std::vector<contracts::OracleCase> build_good_units() {
  return {unit_of(0, generated_keys(1, 0)), unit_of(7, std::vector<std::uint64_t>(spec::kKeys, 42)),
          unit_of(3, keys_from({0, 1, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
                                ~std::uint64_t{0} - 1, ~std::uint64_t{0}},
                               5)),
          unit_of(9, ascending_keys()), unit_of(11, descending_keys())};
}

/// Built once: sorting five units under the test build takes seconds.
inline const std::vector<contracts::OracleCase>& cached_good_units() {
  static const std::vector<contracts::OracleCase> units = build_good_units();
  return units;
}

inline std::vector<contracts::OracleCase> good_units() { return cached_good_units(); }

}  // namespace loadforge::tests::sort_units

#endif
