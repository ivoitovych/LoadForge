// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two trivial reference oracles -- one exact, one bounded -- and a registry of
// them, run against the oracle suites; and the definitions
// src/verification/oracle.hpp carries.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/verification/oracle_contract.hpp"
#include "contracts/verification/oracle_registry_contract.hpp"
#include "contracts/verification/record_contract.hpp"
#include "verification/oracle.hpp"

namespace loadforge::contracts {
namespace {

using verification::Agreement;
using verification::BoundedMismatch;
using verification::Contract;
using verification::ExactMismatch;
using verification::NotChecked;
using verification::UnitEvidence;
using verification::Verdict;

constexpr std::size_t kUnitBytes = 256;

/// byte_xor's specification: output[i] = input[i] XOR (index mod 256).
class ByteXorOracle final : public verification::Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override { return "byte_xor"; }
  [[nodiscard]] Contract contract() const noexcept override { return Contract::kExact; }
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override {
    if (evidence.input.size() != kUnitBytes || evidence.output.size() != kUnitBytes) {
      return NotChecked{"byte_xor unit", "evidence is not 256 bytes in and out", 0};
    }
    const auto key = static_cast<std::byte>(evidence.index & 0xFFU);
    std::optional<std::uint64_t> first;
    std::uint64_t last = 0;
    std::uint64_t popcount = 0;
    for (std::size_t at = 0; at < kUnitBytes; ++at) {
      const auto delta =
          std::to_integer<unsigned>((evidence.input[at] ^ key) ^ evidence.output[at]);
      if (delta != 0) {
        first = first.value_or(at);
        last = at;
        popcount += static_cast<std::uint64_t>(std::popcount(delta));
      }
    }
    if (!first.has_value()) {
      return Agreement{1};
    }
    verification::Difference difference;
    difference.domain = verification::Domain::kStored;
    difference.first_offset = *first;
    difference.last_offset = last;
    difference.popcount = popcount;
    difference.window_offset = *first / 8 * 8;
    const std::size_t end = std::min<std::size_t>(kUnitBytes, difference.window_offset + 64);
    for (std::size_t at = difference.window_offset; at < end; ++at) {
      difference.expected.push_back(evidence.input[at] ^ key);
      difference.actual.push_back(evidence.output[at]);
    }
    return ExactMismatch{"output against input XOR index", difference, 1};
  }
};

/// byte_mean's specification: the output is the mean of the input bytes as a
/// little-endian double. Every partial sum is an integer below 2^16, so the
/// mean is exact in any order, and the tolerance is the spacing of doubles
/// near 255: anything further is wrong.
class ByteMeanOracle final : public verification::Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override { return "byte_mean"; }
  [[nodiscard]] Contract contract() const noexcept override { return Contract::kBounded; }
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override {
    if (evidence.input.size() != kUnitBytes || evidence.output.size() != 8) {
      return NotChecked{"byte_mean unit", "evidence is not 256 bytes in and 8 out", 0};
    }
    double sum = 0.0;
    for (const std::byte byte : evidence.input) {
      sum += std::to_integer<int>(byte);
    }
    std::uint64_t bits = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
      bits |= std::to_integer<std::uint64_t>(evidence.output[byte]) << (8 * byte);
    }
    const double residual = std::abs(std::bit_cast<double>(bits) - sum / kUnitBytes);
    const double tolerance = std::ldexp(1.0, -44);
    if (residual <= tolerance) {
      return Agreement{1};
    }
    return BoundedMismatch{"|mean - output|", residual, tolerance, std::nullopt, 0, 1};
  }
};

std::vector<std::byte> bytes_of(double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  std::vector<std::byte> out(8);
  for (std::size_t byte = 0; byte < 8; ++byte) {
    out[byte] = static_cast<std::byte>(bits >> (8 * byte));
  }
  return out;
}

OracleCase xor_case(std::uint64_t index) {
  OracleCase unit{index, std::vector<std::byte>(kUnitBytes), std::vector<std::byte>(kUnitBytes)};
  for (std::size_t at = 0; at < kUnitBytes; ++at) {
    unit.input[at] = static_cast<std::byte>(at * 7);
    unit.output[at] = unit.input[at] ^ static_cast<std::byte>(index & 0xFFU);
  }
  return unit;
}

OracleCase mean_case(std::uint64_t index) {
  OracleCase unit{index, std::vector<std::byte>(kUnitBytes), {}};
  double sum = 0.0;
  for (std::size_t at = 0; at < kUnitBytes; ++at) {
    unit.input[at] = static_cast<std::byte>((at * 13 + index) & 0xFFU);
    sum += static_cast<double>((at * 13 + index) & 0xFFU);
  }
  unit.output = bytes_of(sum / kUnitBytes);
  return unit;
}

struct ReferenceExactOracle {
  static constexpr const char* kWorkload = "byte_xor";
  static constexpr Contract kContract = Contract::kExact;
  static std::unique_ptr<verification::Oracle> make() { return std::make_unique<ByteXorOracle>(); }
  static std::vector<OracleCase> good() { return {xor_case(0), xor_case(3), xor_case(300)}; }
  static std::vector<OracleCase> wrong() {
    OracleCase one_bit = xor_case(3);
    one_bit.output[100] ^= std::byte{0x04};
    OracleCase spread = xor_case(5);
    spread.output[0] ^= std::byte{0xFF};
    spread.output[255] ^= std::byte{0x01};
    return {one_bit, spread};
  }
};

struct ReferenceBoundedOracle {
  static constexpr const char* kWorkload = "byte_mean";
  static constexpr Contract kContract = Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return std::make_unique<ByteMeanOracle>(); }
  static std::vector<OracleCase> good() { return {mean_case(0), mean_case(1)}; }
  static std::vector<OracleCase> wrong() {
    OracleCase off = mean_case(1);
    off.output = bytes_of(1.0e6);
    OracleCase nan = mean_case(2);
    nan.output = bytes_of(std::numeric_limits<double>::quiet_NaN());
    return {off, nan};
  }
};

using ReferenceOracles = ::testing::Types<ReferenceExactOracle, ReferenceBoundedOracle>;
INSTANTIATE_TYPED_TEST_SUITE_P(Reference, OracleContract, ReferenceOracles);

class ReferenceOracleRegistryImpl final : public verification::OracleRegistry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override {
    return {"byte_mean", "byte_xor"};
  }
  [[nodiscard]] core::Result<std::unique_ptr<verification::Oracle>, verification::OracleError>
  create(std::string_view name, const workloads::WorkloadConfig& /*config*/,
         const workloads::Plan& /*plan*/) const override {
    if (name == "byte_mean") {
      return std::unique_ptr<verification::Oracle>(std::make_unique<ByteMeanOracle>());
    }
    if (name == "byte_xor") {
      return std::unique_ptr<verification::Oracle>(std::make_unique<ByteXorOracle>());
    }
    return verification::OracleError{verification::OracleFault::kUnknownWorkload,
                                     std::string(name)};
  }
};

struct ReferenceOracleRegistry {
  static std::unique_ptr<verification::OracleRegistry> make() {
    return std::make_unique<ReferenceOracleRegistryImpl>();
  }
  static workloads::WorkloadConfig config(const std::string& /*name*/) { return {}; }
  static workloads::Plan plan(const std::string& /*name*/) {
    return workloads::Plan{kUnitBytes, kUnitBytes, 0, 1, 1, 0, 0};
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, OracleRegistryContract,
                               ::testing::Types<ReferenceOracleRegistry>);

TEST(OracleDefinition, VerdictValidityRulesOneAtATime) {
  using verification::is_valid;
  EXPECT_TRUE(is_valid(Agreement{1}, Contract::kExact));
  EXPECT_FALSE(is_valid(Agreement{0}, Contract::kExact)) << "agreeing after comparing nothing";

  const ExactMismatch exact{"c", record_samples::difference(), 1};
  EXPECT_TRUE(is_valid(exact, Contract::kExact));
  EXPECT_TRUE(is_valid(ExactMismatch{"c", std::nullopt, 1}, Contract::kExact));
  EXPECT_FALSE(is_valid(exact, Contract::kBounded)) << "never the other contract's kind";
  EXPECT_FALSE(is_valid(ExactMismatch{"c", record_samples::difference(), 0}, Contract::kExact));
  EXPECT_FALSE(is_valid(ExactMismatch{"", std::nullopt, 1}, Contract::kExact));
  EXPECT_FALSE(
      is_valid(ExactMismatch{std::string(verification::kMaxTextBytes + 1, 'c'), std::nullopt, 1},
               Contract::kExact));
  verification::Difference broken = record_samples::difference();
  broken.popcount = 0;
  EXPECT_FALSE(is_valid(ExactMismatch{"c", broken, 1}, Contract::kExact));

  const BoundedMismatch bounded{"n", 2.0, 1.0, std::nullopt, std::nullopt, 1};
  EXPECT_TRUE(is_valid(bounded, Contract::kBounded));
  EXPECT_FALSE(is_valid(bounded, Contract::kExact));
  BoundedMismatch variant = bounded;
  variant.blocks_compared = 0;
  EXPECT_FALSE(is_valid(variant, Contract::kBounded));
  variant = bounded;
  variant.check.clear();
  EXPECT_FALSE(is_valid(variant, Contract::kBounded));
  variant.check = std::string(verification::kMaxTextBytes + 1, 'n');
  EXPECT_FALSE(is_valid(variant, Contract::kBounded));
  variant = bounded;
  variant.tolerance = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(is_valid(variant, Contract::kBounded));
  variant.tolerance = 0.0;
  EXPECT_FALSE(is_valid(variant, Contract::kBounded));
  variant = bounded;
  variant.residual = 1.0;
  EXPECT_FALSE(is_valid(variant, Contract::kBounded)) << "within tolerance is not a mismatch";
  variant.residual = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(is_valid(variant, Contract::kBounded)) << "a NaN residual is a failure";

  EXPECT_TRUE(is_valid(NotChecked{"w", "y", 0}, Contract::kExact));
  EXPECT_FALSE(is_valid(NotChecked{"", "y", 0}, Contract::kExact));
  EXPECT_FALSE(is_valid(NotChecked{"w", "", 0}, Contract::kExact));
  EXPECT_FALSE(is_valid(NotChecked{std::string(verification::kMaxTextBytes + 1, 'w'), "y", 0},
                        Contract::kExact));
  EXPECT_FALSE(is_valid(NotChecked{"w", std::string(verification::kMaxTextBytes + 1, 'y'), 0},
                        Contract::kExact));
}

TEST(OracleDefinition, BlocksAndFaultNames) {
  EXPECT_EQ(verification::blocks_of(Agreement{3}), 3U);
  EXPECT_EQ(verification::blocks_of(ExactMismatch{"c", std::nullopt, 4}), 4U);
  EXPECT_EQ(verification::blocks_of(BoundedMismatch{"n", 2, 1, {}, {}, 5}), 5U);
  EXPECT_EQ(verification::blocks_of(NotChecked{"w", "y", 6}), 6U);
  EXPECT_EQ(verification::describe(verification::OracleFault::kUnknownWorkload),
            "no oracle for that workload");
  EXPECT_EQ(verification::describe(verification::OracleFault::kConfiguration),
            "configuration outside the specification");
  EXPECT_EQ(verification::describe(static_cast<verification::OracleFault>(9)),
            "unrecognised oracle fault");
  expect_memberwise_equality(
      verification::OracleError{verification::OracleFault::kConfiguration, "d"},
      {[](verification::OracleError& e) { e.fault = verification::OracleFault::kUnknownWorkload; },
       [](verification::OracleError& e) { e.detail = "e"; }});
}

}  // namespace
}  // namespace loadforge::contracts
