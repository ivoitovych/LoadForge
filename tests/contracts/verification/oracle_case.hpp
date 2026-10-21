// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ORACLE_CASE_HPP
#define LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ORACLE_CASE_HPP

// The unit type the oracle suites take (ADR 0003, 0004), apart from the suites
// so that a test that only builds units does not register OracleContract.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "verification/oracle.hpp"

namespace loadforge::contracts {

struct OracleCase {
  std::uint64_t index = 0;
  std::vector<std::byte> input;
  std::vector<std::byte> output;
};

namespace oracle_suite {

inline verification::Verdict check(const verification::Oracle& oracle, const OracleCase& unit) {
  return oracle.check(verification::UnitEvidence{unit.index, unit.input, unit.output});
}

inline std::string name_of(const verification::Verdict& verdict) {
  switch (verdict.index()) {
    case 0:
      return "Agreement";
    case 1:
      return "ExactMismatch";
    case 2:
      return "BoundedMismatch";
    default:
      return "NotChecked";
  }
}

}  // namespace oracle_suite

}  // namespace loadforge::contracts

#endif
