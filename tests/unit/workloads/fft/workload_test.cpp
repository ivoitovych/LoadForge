// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FFT workload's own behaviour beyond the contract's suite: its descriptor,
// its plan for each radix, the refusals and where each boundary lies, and the
// input it generates against the specification written out independently.

#include "workloads/workload.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "oracle/workloads/fft/fft_support.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

constexpr std::uint64_t kUnit = kInputBytes + kOutputBytes;

WorkloadConfig config(std::uint32_t radix = 0, std::uint64_t footprint = 8 * kUnit) {
  return WorkloadConfig{42, footprint, radix, simd::IsaPath::kScalar};
}

std::unique_ptr<Workload> made(const WorkloadConfig& c) {
  auto created = create(c);
  EXPECT_TRUE(created.has_value());
  return created.has_value() ? std::move(created).value() : nullptr;
}

TEST(FftWorkload, TheDescriptorSaysWhatTheSpecificationDeclares) {
  const auto workload = made(config());
  ASSERT_NE(workload, nullptr);
  const Descriptor& d = workload->descriptor();
  EXPECT_EQ(d.name, "fft");
  EXPECT_EQ(d.version, 1U);
  EXPECT_EQ(d.determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(d.contract, verification::Contract::kBounded);
  EXPECT_EQ(d.paths, simd::IsaSet{simd::IsaPath::kScalar});
}

TEST(FftWorkload, ThePlanForEachRadixCountsUnitsStagesAndOperations) {
  for (const std::uint32_t radix : kRadices) {
    const auto workload = made(config(radix, 5 * kUnit + 17));
    ASSERT_NE(workload, nullptr);
    const Plan& p = workload->plan();
    EXPECT_EQ(p.input_bytes, 65536U);
    EXPECT_EQ(p.output_bytes, 65536U);
    EXPECT_EQ(p.scratch_bytes, 65536U);
    EXPECT_EQ(p.units, 5U) << "the 17 spare bytes hold no unit";
    EXPECT_EQ(p.decomposition, radix);
    EXPECT_EQ(p.logical_bytes, stage_count(radix) * 2U * 65536U)
        << "each stage reads and writes the whole vector";
    EXPECT_EQ(p.operations, 5U * 4096U * 12U);
  }
  EXPECT_EQ(made(config())->plan().decomposition, kDefaultRadix);
}

TEST(FftWorkload, OneUnitExactlyFitsAndOneByteLessDoesNot) {
  const auto fits = made(config(0, kUnit));
  ASSERT_NE(fits, nullptr);
  EXPECT_EQ(fits->plan().units, 1U);
  const auto refused = create(config(0, kUnit - 1));
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(refused.error().detail.find(std::to_string(kUnit - 1)), std::string::npos);
  const auto zero = create(config(0, 0));
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error().fault, WorkloadFault::kFootprintTooSmall);
}

TEST(FftWorkload, ARadixOutsideTheSpecificationIsRefusedByName) {
  for (const std::uint32_t radix : {1U, 3U, 5U, 8U, 16U, 4096U, 0xFFFFFFFFU}) {
    const auto refused = create(config(radix));
    ASSERT_FALSE(refused.has_value()) << radix;
    EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(refused.error().detail.find(std::to_string(radix)), std::string::npos);
  }
}

TEST(FftWorkload, AnIsaPathWithoutAKernelIsRefusedNotReplacedByScalar) {
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    for (const std::uint32_t radix : {0U, 2U, 4U}) {
      WorkloadConfig c = config(radix);
      c.isa = path;
      const auto refused = create(c);
      ASSERT_FALSE(refused.has_value());
      EXPECT_EQ(refused.error().fault, WorkloadFault::kConfiguration);
      EXPECT_NE(refused.error().detail.find(std::string(simd::describe(path))), std::string::npos);
    }
  }
}

TEST(FftWorkload, ARadixFaultIsReportedBeforeAnIsaFaultAndBothBeforeTheFootprint) {
  WorkloadConfig c = config(3, 1);
  c.isa = simd::IsaPath::kAvx2;
  EXPECT_NE(create(c).error().detail.find("radix"), std::string::npos);
  c.decomposition = 4;
  EXPECT_NE(create(c).error().detail.find("avx2"), std::string::npos);
  c.isa = simd::IsaPath::kScalar;
  EXPECT_EQ(create(c).error().fault, WorkloadFault::kFootprintTooSmall);
}

TEST(FftWorkload, GeneratedInputIsTheSpecificationsEntryOfEachPhiloxDraw) {
  const auto workload = made(config());
  ASSERT_NE(workload, nullptr);
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{3}, ~std::uint64_t{0}}) {
    testing::Bytes input(kInputBytes);
    workload->generate(index, input);
    EXPECT_EQ(testing::to_doubles(input), testing::spec_input(42, index)) << index;
  }
}

TEST(FftWorkload, GeneratedComponentsAreFiniteMultiplesOfTwoToMinus52Within1) {
  const auto workload = made(config());
  testing::Bytes input(kInputBytes);
  workload->generate(7, input);
  for (const double value : testing::to_doubles(input)) {
    ASSERT_TRUE(std::isfinite(value));
    EXPECT_GE(value, -1.0);
    EXPECT_LT(value, 1.0);
    EXPECT_EQ(std::ldexp(std::nearbyint(std::ldexp(value, 52)), -52), value);
  }
}

TEST(FftWorkload, ComputeMatchesTheDirectDftOfTheGeneratedInput) {
  const auto workload = made(config());
  testing::Threads serial(1);
  testing::Bytes input(kInputBytes);
  workload->generate(1, input);
  const testing::Bytes output = testing::compute_unit(*workload, 1, input, serial);
  const auto want = testing::direct_dft(testing::to_doubles(input));
  const std::vector<double> got = testing::to_doubles(output);
  const double bound =
      64.0 * testing::norm2(testing::to_doubles(input)) * bin_tolerance_factor(kDefaultRadix);
  for (std::size_t k = 0; k < kPoints; ++k) {
    ASSERT_LE(std::fabs(static_cast<long double>(got[2 * k]) - want[k].real()), bound) << k;
    ASSERT_LE(std::fabs(static_cast<long double>(got[2 * k + 1]) - want[k].imag()), bound) << k;
  }
}

TEST(FftWorkload, OnlyTheFirstSlotsScratchIsUsedWhateverTheThreadCount) {
  const auto workload = made(config());
  testing::Threads four(4);
  testing::Bytes input(kInputBytes);
  workload->generate(2, input);
  testing::Bytes output(kOutputBytes);
  testing::Bytes scratch(workload->plan().scratch_bytes * 4, std::byte{0x5A});
  workload->compute(2, input, output, scratch, four);
  for (std::size_t i = workload->plan().scratch_bytes; i < scratch.size(); ++i) {
    ASSERT_EQ(scratch[i], std::byte{0x5A}) << i;
  }
}

}  // namespace
}  // namespace loadforge::workloads::fft
