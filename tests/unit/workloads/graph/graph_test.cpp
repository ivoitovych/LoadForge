// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1: the graph workload's creation, plan, descriptor and input.

#include <gtest/gtest.h>

#include <string>

#include "graph_support.hpp"

namespace loadforge::graph_test {
namespace {

using workloads::WorkloadFault;

TEST(GraphCreate, ADecompositionOfZeroResolvesToTheDefault) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->plan().decomposition, spec::kDefaultAccumulators);
}

TEST(GraphCreate, EveryAccumulatorCountIsAccepted) {
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const auto workload = make(config_for(accumulators, simd::IsaPath::kScalar));
    ASSERT_NE(workload, nullptr) << accumulators;
    EXPECT_EQ(workload->plan().decomposition, accumulators);
  }
}

TEST(GraphCreate, ADecompositionThatIsNotAnAccumulatorCountIsRefusedByValue) {
  for (const std::uint32_t decomposition : {3U, 5U, 6U, 7U, 9U, 16U, 0xFFFFFFFFU}) {
    const auto created = spec::create(config_for(decomposition, simd::IsaPath::kScalar));
    ASSERT_FALSE(created.has_value()) << decomposition;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::to_string(decomposition)), std::string::npos)
        << created.error().detail;
  }
}

TEST(GraphCreate, APathWithoutAKernelIsRefusedByName) {
  for (const simd::IsaPath path :
       {simd::IsaPath::kAvx2, simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    if (make(config_for(0, simd::IsaPath::kScalar))->descriptor().paths.contains(path)) {
      continue;
    }
    const auto created = spec::create(config_for(0, path));
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_NE(created.error().detail.find(std::string(simd::describe(path))), std::string::npos);
  }
}

TEST(GraphCreate, AnIsaPathValueOutsideTheEnumerationIsRefusedLikeAnyOtherWithoutAKernel) {
  const auto created = spec::create(config_for(0, static_cast<simd::IsaPath>(200)));
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
  EXPECT_NE(created.error().detail.find("unrecognised"), std::string::npos);
}

TEST(GraphCreate, AFootprintOfExactlyOneUnitHoldsOneAndOneByteLessHoldsNone) {
  workloads::WorkloadConfig config = config_for(0, simd::IsaPath::kScalar);
  config.footprint_bytes = spec::kInputBytes + spec::kOutputBytes;
  const auto one = make(config);
  ASSERT_NE(one, nullptr);
  EXPECT_EQ(one->plan().units, 1U);
  config.footprint_bytes -= 1;
  const auto none = spec::create(config);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().fault, WorkloadFault::kFootprintTooSmall);
  EXPECT_NE(none.error().detail.find(std::to_string(config.footprint_bytes)), std::string::npos);
}

TEST(GraphPlan, UnitsAreTheWholeUnitsTheFootprintHolds) {
  workloads::WorkloadConfig config = config_for(0, simd::IsaPath::kScalar);
  const std::uint64_t unit = spec::kInputBytes + spec::kOutputBytes;
  for (const std::uint64_t units : {1U, 2U, 7U}) {
    config.footprint_bytes = units * unit + unit - 1;
    const auto workload = make(config);
    ASSERT_NE(workload, nullptr);
    EXPECT_EQ(workload->plan().units, units);
  }
}

TEST(GraphPlan, TheGeometryIsTheSpecifications) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const workloads::Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, 1048580U);
  EXPECT_EQ(plan.output_bytes, 393216U);
  EXPECT_EQ(plan.input_bytes, spec::kInputBytes);
  EXPECT_EQ(plan.output_bytes, spec::kOutputBytes);
  EXPECT_GT(plan.scratch_bytes, 0U);
  EXPECT_GT(plan.logical_bytes, plan.input_bytes + plan.output_bytes);
}

TEST(GraphPlan, OperationsCountThePageRankArithmeticAndGrowWithTheAccumulators) {
  // 16 iterations x (a multiply and an add per edge, and per vertex a - 1
  // combining adds, a multiply and an add) plus one examination per edge of
  // the BFS.
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const auto workload = make(config_for(accumulators, simd::IsaPath::kScalar));
    ASSERT_NE(workload, nullptr);
    EXPECT_EQ(workload->plan().operations,
              16U * (2U * 262144U + (accumulators + 1U) * 32768U) + 262144U);
  }
}

TEST(GraphDescriptor, ItStatesTheClassAndTheContractTheSpecificationDeclares) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const workloads::Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "graph");
  EXPECT_EQ(descriptor.version, 1U);
  EXPECT_EQ(descriptor.determinism, workloads::DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(descriptor.contract, verification::Contract::kExact);
  EXPECT_TRUE(descriptor.paths.contains(simd::IsaPath::kScalar));
  EXPECT_FALSE(descriptor.paths.contains(simd::IsaPath::kAvx512)) << "no kernel for it";
}

TEST(GraphDescriptor, ThePathsAreTheOnesThisBuildHasKernelsFor) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(workload->descriptor().paths, (simd::IsaSet{simd::IsaPath::kScalar}));
#elif defined(__x86_64__)
  EXPECT_EQ(workload->descriptor().paths,
            (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2}));
#elif defined(__aarch64__)
  EXPECT_EQ(workload->descriptor().paths,
            (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kNeon}));
#else
  EXPECT_EQ(workload->descriptor().paths, (simd::IsaSet{simd::IsaPath::kScalar}));
#endif
}

TEST(GraphGenerate, TheInputIsTheSpecificationsDrawsModuloTheVertexCount) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar, 42));
  ASSERT_NE(workload, nullptr);
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{3}, ~std::uint64_t{0}}) {
    Bytes input(spec::kInputBytes);
    workload->generate(index, input);
    EXPECT_EQ(input, reference_input(42, index)) << index;
  }
}

TEST(GraphGenerate, EveryWordIsAVertexAndTheLastIsTheSource) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  Bytes input(spec::kInputBytes);
  workload->generate(1, input);
  for (std::size_t at = 0; at <= spec::kEdges; ++at) {
    EXPECT_LT(word(input, at), spec::kVertices);
  }
  EXPECT_EQ(word(input, spec::kEdges),
            spec::vertex_of(rng::u64_of(rng::block_at(7, 1, spec::kEdges))));
}

TEST(GraphGenerate, TheDecompositionAndThePathDoNotTouchTheInput) {
  Bytes reference(spec::kInputBytes);
  make(config_for(1, simd::IsaPath::kScalar))->generate(2, reference);
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    for (const simd::IsaPath path : runnable_paths()) {
      Bytes input(spec::kInputBytes);
      make(config_for(accumulators, path))->generate(2, input);
      EXPECT_EQ(input, reference) << accumulators << " " << simd::describe(path);
    }
  }
}

TEST(GraphCompute, ItCallsTheExecutorForEveryBfsStepAndSweep) {
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  Bytes input(spec::kInputBytes);
  workload->generate(0, input);
  SerialExecutor executor;
  (void)run_compute(*workload, input, executor);
  EXPECT_GE(executor.runs, 16U) << "at least the sixteen PageRank sweeps";
}

}  // namespace
}  // namespace loadforge::graph_test
