// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two trivial reference workloads and a registry of them, run against the
// workload suites, and the definitions src/workloads/workload.hpp carries.
// Neither is a LoadForge workload: they exist to prove the suites run and can
// be passed, and to show the shape a workload has under this contract.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/workloads/workload_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {
namespace {

using workloads::Descriptor;
using workloads::Plan;
using workloads::Workload;
using workloads::WorkloadConfig;
using workloads::WorkloadError;
using workloads::WorkloadFault;

std::uint64_t load_le64(std::span<const std::byte> bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < 8; ++byte) {
    value |= std::to_integer<std::uint64_t>(bytes[at + byte]) << (8 * byte);
  }
  return value;
}

core::Result<Plan, WorkloadError> plan_for(const WorkloadConfig& config, std::uint64_t input,
                                           std::uint64_t output, std::uint64_t scratch,
                                           std::uint32_t default_decomposition) {
  if (config.isa != simd::IsaPath::kScalar) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "no kernel for " + std::string(simd::describe(config.isa))};
  }
  const std::uint32_t decomposition =
      config.decomposition == 0 ? default_decomposition : config.decomposition;
  if (decomposition > 64) {
    return WorkloadError{WorkloadFault::kConfiguration, "decomposition above 64"};
  }
  const std::uint64_t units = config.footprint_bytes / (input + output);
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         std::to_string(config.footprint_bytes) + " bytes hold no unit"};
  }
  return Plan{input, output, scratch, units, decomposition, input + output, input / 8};
}

/// Output: the sum, modulo 2^64, of the input's little-endian 64-bit words.
/// Chunked over the executor by the decomposition, partial sums combined in
/// chunk order -- integer addition, so the bits are the same at any thread
/// count and any decomposition: BitExact.
class WordSum final : public Workload {
 public:
  static constexpr std::uint64_t kInputBytes = 4096;

  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(
      const WorkloadConfig& config) {
    auto plan = plan_for(config, kInputBytes, 8, 0, 4);
    if (!plan.has_value()) {
      return plan.error();
    }
    return std::unique_ptr<Workload>(new WordSum(config, plan.value()));
  }

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(seed_, index, input);
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& executor) const noexcept override {
    const std::size_t chunks = plan_.decomposition;
    const std::size_t words = input.size() / 8;
    std::array<std::uint64_t, 64> partial{};
    auto sum_chunk = [&](std::size_t chunk, std::size_t /*slot*/) noexcept {
      std::uint64_t sum = 0;
      for (std::size_t word = chunk * words / chunks; word < (chunk + 1) * words / chunks; ++word) {
        sum += load_le64(input, word * 8);
      }
      partial.at(chunk) = sum;
    };
    worker::for_each_index(executor, chunks, sum_chunk);
    std::uint64_t total = 0;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
      total += partial.at(chunk);
    }
    for (std::size_t byte = 0; byte < 8; ++byte) {
      output[byte] = static_cast<std::byte>(total >> (8 * byte));
    }
  }

 private:
  WordSum(const WorkloadConfig& config, const Plan& plan)
      : descriptor_{"word_sum", 1, workloads::DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan),
        seed_(config.seed) {}

  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
};

/// Output: each input byte XOR the unit index's low byte, through the slot's
/// scratch so that the scratch rules are exercised.
class ByteXor final : public Workload {
 public:
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(
      const WorkloadConfig& config) {
    auto plan = plan_for(config, 256, 256, 256, 1);
    if (!plan.has_value()) {
      return plan.error();
    }
    return std::unique_ptr<Workload>(new ByteXor(config, plan.value()));
  }

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(seed_, index, input);
  }

  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch,
               worker::Executor& /*executor*/) const noexcept override {
    const auto key = static_cast<std::byte>(index & 0xFFU);
    for (std::size_t at = 0; at < input.size(); ++at) {
      scratch[at] = input[at] ^ key;
    }
    for (std::size_t at = 0; at < output.size(); ++at) {
      output[at] = scratch[at];
    }
  }

 private:
  ByteXor(const WorkloadConfig& config, const Plan& plan)
      : descriptor_{"byte_xor", 2, workloads::DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan),
        seed_(config.seed) {}

  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
};

struct ReferenceWordSum {
  static constexpr const char* kName = "word_sum";
  static WorkloadConfig config() { return WorkloadConfig{42, 3 * (4096 + 8), 0, {}}; }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    return WordSum::create(c);
  }
};

struct ReferenceByteXor {
  static constexpr const char* kName = "byte_xor";
  static WorkloadConfig config() { return WorkloadConfig{7, 4096, 1, {}}; }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    return ByteXor::create(c);
  }
};

using ReferenceWorkloads = ::testing::Types<ReferenceWordSum, ReferenceByteXor>;
INSTANTIATE_TYPED_TEST_SUITE_P(Reference, WorkloadContract, ReferenceWorkloads);

class ReferenceRegistryImpl final : public workloads::Registry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return {"byte_xor", "word_sum"}; }
  [[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
      std::string_view name, const WorkloadConfig& config) const override {
    if (name == "byte_xor") {
      return ByteXor::create(config);
    }
    if (name == "word_sum") {
      return WordSum::create(config);
    }
    return WorkloadError{WorkloadFault::kUnknownWorkload,
                         "no workload '" + std::string(name) + "'"};
  }
};

struct ReferenceRegistry {
  static std::unique_ptr<workloads::Registry> make() {
    return std::make_unique<ReferenceRegistryImpl>();
  }
  static WorkloadConfig config(const std::string& /*name*/) {
    return WorkloadConfig{1, 1U << 16U, 0, {}};
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, RegistryContract, ::testing::Types<ReferenceRegistry>);

TEST(WorkloadDefinition, ClassAndFaultNames) {
  using workloads::DeterminismClass;
  EXPECT_EQ(workloads::describe(DeterminismClass::kBitExact), "BitExact");
  EXPECT_EQ(workloads::describe(DeterminismClass::kBitExactFixedDecomposition),
            "BitExactFixedDecomposition");
  EXPECT_EQ(workloads::describe(DeterminismClass::kBoundedResidual), "BoundedResidual");
  EXPECT_EQ(workloads::describe(static_cast<DeterminismClass>(9)),
            "unrecognised determinism class");
  EXPECT_EQ(workloads::describe(WorkloadFault::kUnknownWorkload), "unknown workload");
  EXPECT_EQ(workloads::describe(WorkloadFault::kConfiguration),
            "configuration outside the workload's domain");
  EXPECT_EQ(workloads::describe(WorkloadFault::kFootprintTooSmall),
            "footprint too small for one unit");
  EXPECT_EQ(workloads::describe(static_cast<WorkloadFault>(9)), "unrecognised workload fault");
}

TEST(WorkloadDefinition, NamesAreLowerCaseLettersDigitsAndUnderscoresFromALetter) {
  EXPECT_TRUE(workloads::is_valid_name("compression"));
  EXPECT_TRUE(workloads::is_valid_name("dense_linear"));
  EXPECT_TRUE(workloads::is_valid_name("fft2"));
  EXPECT_TRUE(workloads::is_valid_name("z"));
  EXPECT_FALSE(workloads::is_valid_name(""));
  EXPECT_FALSE(workloads::is_valid_name("2fft"));
  EXPECT_FALSE(workloads::is_valid_name("_x"));
  EXPECT_FALSE(workloads::is_valid_name("Dense"));
  EXPECT_FALSE(workloads::is_valid_name("dense-linear"));
  EXPECT_FALSE(workloads::is_valid_name("a{"));
  EXPECT_FALSE(workloads::is_valid_name("a/"));
  EXPECT_FALSE(workloads::is_valid_name("a:"));
}

TEST(WorkloadEquality, EveryMemberIsCompared) {
  expect_memberwise_equality(
      Descriptor{"x", 1, workloads::DeterminismClass::kBitExact, verification::Contract::kExact,
                 simd::IsaSet{simd::IsaPath::kScalar}},
      {[](Descriptor& d) { d.name = "y"; }, [](Descriptor& d) { d.version = 2; },
       [](Descriptor& d) { d.determinism = workloads::DeterminismClass::kBoundedResidual; },
       [](Descriptor& d) { d.contract = verification::Contract::kBounded; },
       [](Descriptor& d) { d.paths = d.paths.with(simd::IsaPath::kAvx2); }});
  expect_memberwise_equality(
      WorkloadConfig{1, 2, 3, simd::IsaPath::kScalar},
      {[](WorkloadConfig& c) { c.seed = 9; }, [](WorkloadConfig& c) { c.footprint_bytes = 9; },
       [](WorkloadConfig& c) { c.decomposition = 9; },
       [](WorkloadConfig& c) { c.isa = simd::IsaPath::kNeon; }});
  expect_memberwise_equality(
      Plan{1, 2, 3, 4, 5, 6, 7},
      {[](Plan& p) { p.input_bytes = 9; }, [](Plan& p) { p.output_bytes = 9; },
       [](Plan& p) { p.scratch_bytes = 9; }, [](Plan& p) { p.units = 9; },
       [](Plan& p) { p.decomposition = 9; }, [](Plan& p) { p.logical_bytes = 9; },
       [](Plan& p) { p.operations = 9; }});
  expect_memberwise_equality(WorkloadError{WorkloadFault::kConfiguration, "d"},
                             {[](WorkloadError& e) { e.fault = WorkloadFault::kUnknownWorkload; },
                              [](WorkloadError& e) { e.detail = "e"; }});
}

}  // namespace
}  // namespace loadforge::contracts
