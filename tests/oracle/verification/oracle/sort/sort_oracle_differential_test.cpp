// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the sort oracle: its verdict against a second, plain reading of the
// specification (a naive judge that builds the whole right output with
// std::sort and compares bytes), over many units and many corruptions, every
// one of them a different place for a fault to be.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "sort_units.hpp"
#include "verification/oracle/sort/sort_oracle.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::contracts {
// The contract header registers both suites; this binary runs neither.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts

namespace loadforge::tests {
namespace {

namespace spec = workloads::sort;
using contracts::OracleCase;
using verification::ExactMismatch;
using verification::Verdict;

/// The specification, read plainly. The right output is built by std::sort;
/// then the three steps, each by the straightest loop.
Verdict naive_verdict(const OracleCase& unit) {
  const auto& output = unit.output;
  std::vector<bool> seen(spec::kKeys, false);
  for (std::size_t j = 0; j < spec::kKeys; ++j) {
    const std::uint32_t index = sort_units::get32(output, spec::index_offset(j));
    if (index >= spec::kKeys) {
      return ExactMismatch{"index out of range at position " + std::to_string(j), std::nullopt,
                           j + 1};
    }
    if (seen[index]) {
      return ExactMismatch{"index repeated at position " + std::to_string(j), std::nullopt, j + 1};
    }
    seen[index] = true;
  }
  // Expected keys region, byte by byte.
  std::vector<std::byte> expected(spec::kIndexRegionOffset);
  std::optional<std::size_t> first_wrong;
  std::vector<std::size_t> differing;
  for (std::size_t j = 0; j < spec::kKeys; ++j) {
    const std::uint32_t index = sort_units::get32(output, spec::index_offset(j));
    for (std::size_t byte = 0; byte < 8; ++byte) {
      expected[j * 8 + byte] = unit.input[std::size_t{index} * 8 + byte];
      if (expected[j * 8 + byte] != output[j * 8 + byte]) {
        differing.push_back(j * 8 + byte);
        if (!first_wrong.has_value()) {
          first_wrong = j;
        }
      }
    }
  }
  if (first_wrong.has_value()) {
    verification::Difference difference;
    difference.first_offset = differing.front();
    difference.last_offset = differing.back();
    for (const std::size_t at : differing) {
      difference.popcount += static_cast<std::uint64_t>(
          std::popcount(std::to_integer<unsigned>(expected[at] ^ output[at])));
    }
    difference.window_offset = difference.first_offset / 8 * 8;
    for (std::size_t at = difference.window_offset;
         at < difference.window_offset + 64 && at < spec::kIndexRegionOffset; ++at) {
      difference.expected.push_back(expected[at]);
      difference.actual.push_back(output[at]);
    }
    const std::uint32_t named = sort_units::get32(output, spec::index_offset(*first_wrong));
    return ExactMismatch{"key differs from input key " + std::to_string(named) + " at position " +
                             std::to_string(*first_wrong),
                         difference, spec::kKeys};
  }
  // Keys are the input permuted: the right order is std::sort's.
  std::vector<std::uint64_t> keys(spec::kKeys);
  for (std::size_t j = 0; j < spec::kKeys; ++j) {
    keys[j] = sort_units::get64(output, j * 8);
  }
  const std::vector<std::byte> right = [&] {
    std::vector<std::uint64_t> input_keys(spec::kKeys);
    for (std::size_t e = 0; e < spec::kKeys; ++e) {
      input_keys[e] = sort_units::get64(unit.input, e * 8);
    }
    return sort_units::sorted_output_of(input_keys);
  }();
  if (right == output) {
    return verification::Agreement{spec::kKeys};
  }
  for (std::size_t j = 1; j < spec::kKeys; ++j) {
    const std::uint32_t before = sort_units::get32(output, spec::index_offset(j - 1));
    const std::uint32_t at = sort_units::get32(output, spec::index_offset(j));
    if (keys[j - 1] > keys[j] || (keys[j - 1] == keys[j] && before >= at)) {
      return ExactMismatch{"order broken at position " + std::to_string(j), std::nullopt,
                           spec::kKeys};
    }
  }
  ADD_FAILURE() << "an output that differs from the right one is neither wrong nor misordered";
  return verification::Agreement{spec::kKeys};
}

/// A deterministic stream of numbers for choosing corruptions.
class Lcg {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_ >> 17U;
  }

 private:
  std::uint64_t state_;
};

void swap_pairs(std::vector<std::byte>& output, std::size_t a, std::size_t b) {
  for (std::size_t byte = 0; byte < 8; ++byte) {
    std::swap(output[spec::key_offset(a) + byte], output[spec::key_offset(b) + byte]);
  }
  for (std::size_t byte = 0; byte < 4; ++byte) {
    std::swap(output[spec::index_offset(a) + byte], output[spec::index_offset(b) + byte]);
  }
}

void expect_same_verdict(const verification::Oracle& oracle, const OracleCase& unit,
                         const std::string& what) {
  const Verdict got = contracts::oracle_suite::check(oracle, unit);
  const Verdict want = naive_verdict(unit);
  ASSERT_EQ(got.index(), want.index()) << what;
  if (const auto* agreement = std::get_if<verification::Agreement>(&got)) {
    EXPECT_EQ(agreement->blocks_compared, verification::blocks_of(want)) << what;
    return;
  }
  const auto& g = std::get<ExactMismatch>(got);
  const auto& w = std::get<ExactMismatch>(want);
  EXPECT_EQ(g.check, w.check) << what;
  EXPECT_EQ(g.blocks_compared, w.blocks_compared) << what;
  EXPECT_EQ(g.difference, w.difference) << what;
}

TEST(SortOracleDifferential, RightUnitsAgreeUnderEveryChunkCountAndSeed) {
  for (const std::uint32_t chunks : spec::kChunkCounts) {
    workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, chunks, 0, 0};
    const auto oracle = verification::oracle::make_sort_oracle({}, plan);
    ASSERT_TRUE(oracle.has_value());
    const OracleCase unit =
        sort_units::unit_of(chunks, sort_units::generated_keys(chunks * 31ULL, chunks));
    ASSERT_TRUE(std::holds_alternative<verification::Agreement>(
        contracts::oracle_suite::check(*oracle.value(), unit)))
        << chunks;
  }
}

TEST(SortOracleDifferential, ManySingleCorruptionsOfTiedAndUntiedUnitsAgreeWithTheNaiveJudge) {
  const auto oracle = verification::oracle::make_sort_oracle(
      {}, workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, 16, 0, 0});
  ASSERT_TRUE(oracle.has_value());
  Lcg rng(7);
  const std::vector<OracleCase> bases = {
      sort_units::cached_good_units()[0], sort_units::cached_good_units()[2],
      sort_units::unit_of(4, sort_units::keys_from({1, 2, 3}, 9))};
  for (const OracleCase& base : bases) {
    for (int round = 0; round < 24; ++round) {
      OracleCase unit = base;
      const std::uint64_t kind = rng.next() % 5;
      const std::size_t a = rng.next() % spec::kKeys;
      const std::size_t b = rng.next() % spec::kKeys;
      switch (kind) {
        case 0:  // a flipped bit anywhere in the output
          unit.output[rng.next() % spec::kOutputBytes] ^=
              static_cast<std::byte>(1U << (rng.next() % 8));
          break;
        case 1:  // two pairs exchanged
          swap_pairs(unit.output, a, b);
          break;
        case 2:  // one index replaced by another position's
          sort_units::put32(unit.output, spec::index_offset(a),
                            sort_units::get32(unit.output, spec::index_offset(b)));
          break;
        case 3:  // one key replaced by another's, index kept
          sort_units::put64(unit.output, spec::key_offset(a),
                            sort_units::get64(unit.output, spec::key_offset(b)));
          break;
        default:  // a flipped bit in the input after compute()
          unit.input[rng.next() % spec::kInputBytes] ^=
              static_cast<std::byte>(1U << (rng.next() % 8));
          break;
      }
      expect_same_verdict(*oracle.value(), unit,
                          "kind " + std::to_string(kind) + " round " + std::to_string(round));
    }
  }
}

}  // namespace
}  // namespace loadforge::tests
