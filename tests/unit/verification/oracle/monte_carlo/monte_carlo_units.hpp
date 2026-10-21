// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_VERIFICATION_ORACLE_MONTE_CARLO_MONTE_CARLO_UNITS_HPP
#define LOADFORGE_TESTS_VERIFICATION_ORACLE_MONTE_CARLO_MONTE_CARLO_UNITS_HPP

// Monte Carlo units for the oracle's tests. The right output of a unit is
// built here the way the specification's DECOMPOSITION paragraph says -- each
// chunk of C consecutive samples summed left to right, the chunk sums added in
// increasing order from chunk 0's -- from the integrand values the
// specification's functions give. A different order from the oracle's
// compensated sum, so the two agree only within the derived bound.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "contracts/verification/oracle_case.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::tests::mc_units {

namespace spec = workloads::monte_carlo;

inline void put64(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value) {
  for (std::size_t byte = 0; byte < 8; ++byte) {
    bytes[at + byte] = static_cast<std::byte>(value >> (8U * byte));
  }
}

inline std::uint64_t get64(const std::vector<std::byte>& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    value |= std::to_integer<std::uint64_t>(bytes[at + byte]) << (8U * byte);
  }
  return value;
}

inline double output_of(const contracts::OracleCase& unit, std::size_t batch) {
  return std::bit_cast<double>(get64(unit.output, batch * 8));
}

inline void set_output(contracts::OracleCase& unit, std::size_t batch, double value) {
  put64(unit.output, batch * 8, std::bit_cast<std::uint64_t>(value));
}

inline std::uint64_t key_of(const contracts::OracleCase& unit, std::size_t batch) {
  return get64(unit.input, batch * 8);
}

/// The integrand values of a batch, from the specification's functions.
inline std::vector<double> terms_of(std::uint64_t key) {
  std::vector<double> terms(spec::kSamplesPerBatch);
  for (std::size_t sample = 0; sample < terms.size(); ++sample) {
    terms[sample] = spec::sample_value(spec::sample_bits(key, sample));
  }
  return terms;
}

/// The batch's sum in chunks of `chunk`, as the decomposition defines it.
inline double chunked_sum(const std::vector<double>& terms, std::size_t chunk) {
  double total = 0.0;
  for (std::size_t first = 0; first < terms.size(); first += chunk) {
    double part = terms[first];
    for (std::size_t at = first + 1; at < first + chunk; ++at) {
      part += terms[at];
    }
    total = first == 0 ? part : total + part;
  }
  return total;
}

/// The terms of batch b of the unit (seed 1, index 0)'s input, built once.
inline const std::vector<double>& cached_terms(std::size_t batch) {
  static std::map<std::size_t, std::vector<double>> cache;
  auto found = cache.find(batch);
  if (found == cache.end()) {
    found = cache.emplace(batch, terms_of(spec::batch_key(1, 0, batch))).first;
  }
  return found->second;
}

/// A unit with index `index` whose input is the input of (seed 1, unit 0) and
/// whose output is right for chunk length `chunk`.
inline contracts::OracleCase unit_of(std::uint64_t index, std::size_t chunk) {
  contracts::OracleCase unit{index, std::vector<std::byte>(spec::kInputBytes),
                             std::vector<std::byte>(spec::kOutputBytes)};
  for (std::size_t batch = 0; batch < spec::kBatches; ++batch) {
    put64(unit.input, batch * 8, spec::batch_key(1, 0, batch));
    set_output(unit, batch, chunked_sum(cached_terms(batch), chunk));
  }
  return unit;
}

}  // namespace loadforge::tests::mc_units

#endif
