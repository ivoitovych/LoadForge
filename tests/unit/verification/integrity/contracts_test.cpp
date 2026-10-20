// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract suites this module meets and consumes (ADR 0003):
//
//   * SliceRunnerContract against make_slice_runner(), the provider's half;
//   * WorkloadContract, OracleContract and ExecutorContract against this
//     stream's own doubles (integrity_support.hpp), the consumer's half: a
//     double that failed its contract's suite would be a misreading of the
//     contract, and every test of the runner built on it would inherit it.

#include <cstdint>
#include <memory>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "contracts/verification/runner_contract.hpp"
#include "contracts/worker/executor_contract.hpp"
#include "contracts/workloads/workload_contract.hpp"
#include "unit/verification/integrity/integrity_support.hpp"
#include "verification/runner.hpp"

namespace loadforge::contracts {
namespace {

using integrity_testing::MeanOracle;
using integrity_testing::MeanWorkload;
using integrity_testing::rotor_config;
using integrity_testing::RotorOracle;
using integrity_testing::RotorWorkload;
using integrity_testing::SerialExecutor;
using integrity_testing::ThreadedExecutor;

/// The specifications' outputs, computed here a third way for the oracle
/// suites' cases.
OracleCase rotor_case(std::uint64_t seed, std::uint64_t index) {
  OracleCase unit{index, Bytes(RotorWorkload::kInputBytes), Bytes(RotorWorkload::kOutputBytes)};
  rng::fill_stream(seed, index, unit.input);
  for (std::size_t at = 0; at < unit.input.size(); ++at) {
    const auto value = std::to_integer<std::uint8_t>(unit.input[at]);
    unit.output[at] = static_cast<std::byte>(
        static_cast<std::uint8_t>((value << 3U) | (value >> 5U)) ^ RotorWorkload::key(index));
  }
  return unit;
}

OracleCase mean_case(std::uint64_t seed, std::uint64_t index, double shift) {
  OracleCase unit{index, Bytes(MeanWorkload::kInputBytes), Bytes(8)};
  rng::fill_stream(seed, index, unit.input);
  std::uint64_t sum = 0;
  for (const std::byte byte : unit.input) {
    sum += std::to_integer<std::uint64_t>(byte);
  }
  const auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(sum) / 64.0 + shift);
  for (std::size_t byte = 0; byte < 8; ++byte) {
    unit.output[byte] = static_cast<std::byte>(bits >> (8 * byte));
  }
  return unit;
}

struct Provider {
  static core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError> make(
      const verification::RunnerSetup& setup) {
    return verification::make_slice_runner(setup);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(InputIntegrity, SliceRunnerContract, ::testing::Types<Provider>);

struct Rotor {
  static constexpr const char* kName = "rotor";
  static workloads::WorkloadConfig config() { return rotor_config(); }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return RotorWorkload::create(config);
  }
};

struct Mean {
  static constexpr const char* kName = "mean";
  static workloads::WorkloadConfig config() {
    return workloads::WorkloadConfig{5, 4 * 72, 0, simd::IsaPath::kScalar};
  }
  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    return MeanWorkload::create(config);
  }
};

using Workloads = ::testing::Types<Rotor, Mean>;
INSTANTIATE_TYPED_TEST_SUITE_P(InputIntegrity, WorkloadContract, Workloads);

struct RotorChecked {
  static constexpr const char* kWorkload = "rotor";
  static constexpr verification::Contract kContract = verification::Contract::kExact;
  static std::unique_ptr<verification::Oracle> make() { return std::make_unique<RotorOracle>(); }
  static std::vector<OracleCase> good() {
    return {rotor_case(7, 0), rotor_case(7, 1), rotor_case(11, 900)};
  }
  static std::vector<OracleCase> wrong() {
    OracleCase one_bit = rotor_case(7, 2);
    one_bit.output[199] ^= std::byte{0x10};
    OracleCase spread = rotor_case(7, 3);
    spread.output[0] ^= std::byte{0xFF};
    spread.output[150] ^= std::byte{0x01};
    OracleCase unrotated = rotor_case(7, 4);
    unrotated.output = unrotated.input;
    return {one_bit, spread, unrotated};
  }
};

struct MeanChecked {
  static constexpr const char* kWorkload = "mean";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return std::make_unique<MeanOracle>(); }
  static std::vector<OracleCase> good() { return {mean_case(5, 0, 0.0), mean_case(5, 9, 0.0)}; }
  static std::vector<OracleCase> wrong() {
    return {mean_case(5, 1, 0x1p-40), mean_case(5, 2, -3.0),
            mean_case(5, 3, std::numeric_limits<double>::quiet_NaN())};
  }
};

using Oracles = ::testing::Types<RotorChecked, MeanChecked>;
INSTANTIATE_TYPED_TEST_SUITE_P(InputIntegrity, OracleContract, Oracles);

struct Serial {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<SerialExecutor>(); }
};

struct Threaded {
  static std::unique_ptr<worker::Executor> make() { return std::make_unique<ThreadedExecutor>(3); }
};

using Executors = ::testing::Types<Serial, Threaded>;
INSTANTIATE_TYPED_TEST_SUITE_P(InputIntegrity, ExecutorContract, Executors);

// The registries are the integration stream's to provide and consume; this
// module includes their headers for the suites above and uses neither.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);

}  // namespace
}  // namespace loadforge::contracts
