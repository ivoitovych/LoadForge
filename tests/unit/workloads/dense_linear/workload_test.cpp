// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dense-linear workload at T1: WorkloadContract against every path this
// build has and this machine runs, then what the contract leaves to the
// workload -- the descriptor, the plan's figures and their footprint
// boundaries, each refusal at creation and what it names, the generated
// entries at both ends of each matrix, and compute()'s driver, watched through
// a kernel table the test puts in front of it.

#include "workloads/workload.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "contracts/workloads/workload_contract.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/dense_linear/support.hpp"
#include "verification/record.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear {
namespace {

using simd::IsaPath;
using testing::Bytes;
using testing::kMatrix;
using testing::kN;

constexpr std::uint64_t kUnit = std::uint64_t{kInputBytes} + kOutputBytes;

// ---- WorkloadContract, on every path this build has -------------------------

struct ScalarSubject {
  static constexpr const char* kName = "dense_linear";
  static WorkloadConfig config() { return testing::config_for(IsaPath::kScalar); }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    return dense_linear::create(c);
  }
};

#if defined(LOADFORGE_SIMD_X86_KERNELS)
struct Avx2Subject {
  static constexpr const char* kName = "dense_linear";
  static WorkloadConfig config() { return testing::config_for(IsaPath::kAvx2, 32); }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    if (!testing::avx2_runs_here()) {
      return WorkloadError{WorkloadFault::kConfiguration,
                           "this machine does not run AVX2 with FMA3, so the AVX2 kernel this "
                           "build holds cannot be tested here"};
    }
    return dense_linear::create(c);
  }
};
#endif

#if defined(LOADFORGE_SIMD_ARM_KERNELS)
struct NeonSubject {
  static constexpr const char* kName = "dense_linear";
  static WorkloadConfig config() { return testing::config_for(IsaPath::kNeon, 128); }
  static core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& c) {
    return dense_linear::create(c);
  }
};
#endif

using Subjects = ::testing::Types<ScalarSubject
#if defined(LOADFORGE_SIMD_X86_KERNELS)
                                  ,
                                  Avx2Subject
#endif
#if defined(LOADFORGE_SIMD_ARM_KERNELS)
                                  ,
                                  NeonSubject
#endif
                                  >;

// ---- the descriptor and the kernels this build holds ------------------------

TEST(DenseLinearDescriptor, NamesTheWorkloadItsClassAndItsContract) {
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Descriptor& descriptor = workload->descriptor();
  EXPECT_EQ(descriptor.name, "dense_linear");
  EXPECT_EQ(descriptor.name, kName);
  EXPECT_EQ(descriptor.version, kVersion);
  EXPECT_EQ(descriptor.determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(descriptor.contract, verification::Contract::kBounded);
  EXPECT_EQ(descriptor.paths, simd::kernel_paths(built_kernels()));
}

TEST(DenseLinearKernels, ScalarAlwaysTheVectorPathsExactlyWhereBuiltAndNeverAvx512) {
  const Kernels& kernels = built_kernels();
  EXPECT_EQ(kernels.scalar, &panel_scalar);
  EXPECT_EQ(kernels.avx512, nullptr) << "no AVX-512 kernel exists";
#if defined(LOADFORGE_SIMD_X86_KERNELS)
  EXPECT_NE(kernels.avx2, nullptr);
#else
  EXPECT_EQ(kernels.avx2, nullptr);
#endif
#if defined(LOADFORGE_SIMD_ARM_KERNELS)
  EXPECT_NE(kernels.neon, nullptr);
#else
  EXPECT_EQ(kernels.neon, nullptr);
#endif
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(simd::kernel_paths(kernels), simd::IsaSet{IsaPath::kScalar});
#endif
  // Whatever this build holds is a subset of what it compiled.
  EXPECT_EQ(simd::kernel_paths(kernels).intersect(simd::compiled_paths()),
            simd::kernel_paths(kernels));
}

// ---- the plan -----------------------------------------------------------------

TEST(DenseLinearPlan, TheFiguresOfOneUnit) {
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Plan& plan = workload->plan();
  EXPECT_EQ(plan.input_bytes, 2U * kN * kN * 8U);
  EXPECT_EQ(plan.output_bytes, kN * kN * 8U);
  EXPECT_EQ(plan.scratch_bytes, 0U) << "nothing is packed: the tiles read A and B in place";
  EXPECT_EQ(plan.units, 2U);
  EXPECT_EQ(plan.decomposition, kDefaultPanelDepth);
  EXPECT_EQ(plan.logical_bytes, 3U * kN * kN * 8U);
  EXPECT_EQ(plan.operations, 2U * kN * kN * kN);
}

TEST(DenseLinearPlan, TheFootprintAtEachSideOfOneUnitAndOfTwo) {
  WorkloadConfig config = testing::config_for(IsaPath::kScalar);
  for (const auto& [footprint, units] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
           {kUnit, 1},
           {kUnit + 1, 1},
           {2 * kUnit - 1, 1},
           {2 * kUnit, 2},
           {std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max() / kUnit}}) {
    config.footprint_bytes = footprint;
    const auto workload = testing::make(config);
    ASSERT_NE(workload, nullptr) << footprint;
    EXPECT_EQ(workload->plan().units, units) << footprint;
    EXPECT_LE(workload->plan().units * kUnit, footprint);
  }
}

TEST(DenseLinearPlan, AFootprintOneByteShortOfAUnitIsRefusedAndNamed) {
  for (const std::uint64_t footprint : {std::uint64_t{0}, kUnit - 1}) {
    WorkloadConfig config = testing::config_for(IsaPath::kScalar);
    config.footprint_bytes = footprint;
    const auto created = create(config);
    ASSERT_FALSE(created.has_value()) << footprint;
    EXPECT_EQ(created.error().fault, WorkloadFault::kFootprintTooSmall);
    EXPECT_EQ(created.error().detail, "a footprint of " + std::to_string(footprint) +
                                          " bytes holds no unit of 1572864 bytes");
  }
}

// ---- the decomposition --------------------------------------------------------

TEST(DenseLinearDecomposition, ZeroAsksForTheDefault) {
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar, 0));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(workload->plan().decomposition, 64U);
}

TEST(DenseLinearDecomposition, EveryPanelDepthTheSpecificationListsIsAccepted) {
  for (const std::uint32_t depth : kPanelDepths) {
    const auto workload = testing::make(testing::config_for(IsaPath::kScalar, depth));
    ASSERT_NE(workload, nullptr) << depth;
    EXPECT_EQ(workload->plan().decomposition, depth);
  }
}

TEST(DenseLinearDecomposition, EveryOtherDepthIsRefusedAndNamed) {
  for (const std::uint32_t depth : {1U, 8U, 15U, 17U, 48U, 63U, 65U, 255U, 257U, 512U,
                                    std::numeric_limits<std::uint32_t>::max()}) {
    const auto created = create(testing::config_for(IsaPath::kScalar, depth));
    ASSERT_FALSE(created.has_value()) << depth;
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_EQ(created.error().detail,
              "panel depth " + std::to_string(depth) + " is not one of 16, 32, 64, 128, 256");
  }
}

// ---- the ISA path -------------------------------------------------------------

TEST(DenseLinearPath, APathWithoutAKernelIsRefusedNamingItAndWhatTheBuildHas) {
  const simd::IsaSet built = simd::kernel_paths(built_kernels());
  std::string has = "scalar";
  for (const IsaPath path : {IsaPath::kAvx2, IsaPath::kNeon}) {
    if (built.contains(path)) {
      has += ", " + std::string(simd::describe(path));
    }
  }
  std::size_t refused = 0;
  for (const IsaPath path : {IsaPath::kAvx2, IsaPath::kAvx512, IsaPath::kNeon}) {
    if (built.contains(path)) {
      continue;
    }
    const auto created = create(testing::config_for(path));
    ASSERT_FALSE(created.has_value()) << simd::describe(path);
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_EQ(created.error().detail, "no dense_linear kernel for ISA path " +
                                          std::string(simd::describe(path)) + "; this build has " +
                                          has);
    ++refused;
  }
  EXPECT_GE(refused, 2U) << "AVX-512 everywhere, and the other architecture's path";
}

TEST(DenseLinearPath, AValueOutsideTheEnumerationIsRefusedNotReplaced) {
  const auto created = create(testing::config_for(static_cast<IsaPath>(9)));
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
  EXPECT_NE(created.error().detail.find("unrecognised ISA path"), std::string::npos)
      << created.error().detail;
}

// ---- a kernel table the test controls ----------------------------------------

/// What the recording kernel saw: one entry per call.
struct Call {
  std::size_t row;
  std::size_t k0;
  std::size_t depth;
  bool tile_was_zero;  ///< the tile's rows of C were all +0.0 when the call began
};

/// The calls the recording kernel has seen, with the lock that guards them.
struct CallLog {
  std::mutex lock;
  std::vector<Call> calls;  // guarded by lock
};

CallLog& call_log() {
  static CallLog log;
  return log;
}

/// Records its arguments, checks the zeroing that precedes the first panel,
/// and marks each entry of its tile by adding 1, so that the test can see
/// that every panel of every tile reached the output.
void recording_kernel(const Panel& panel) noexcept {
  bool zero = true;
  for (std::size_t i = panel.row; i < panel.row + kTileRows; ++i) {
    for (std::size_t j = 0; j < kN; ++j) {
      double value = 0.0;
      std::memcpy(&value, &panel.c[(i * kN + j) * sizeof(double)], sizeof value);
      zero = zero && testing::bits_of(value) == 0;
      value = value + 1.0;
      std::memcpy(&panel.c[(i * kN + j) * sizeof(double)], &value, sizeof value);
    }
  }
  CallLog& log = call_log();
  const std::scoped_lock lock(log.lock);
  log.calls.push_back(Call{panel.row, panel.k0, panel.depth, zero});
}

std::vector<Call> take_calls() {
  CallLog& log = call_log();
  const std::scoped_lock lock(log.lock);
  std::vector<Call> taken;
  taken.swap(log.calls);
  return taken;
}

TEST(DenseLinearTable, ATableWithoutItsScalarKernelIsRefusedWhateverPathIsAsked) {
  const Kernels no_scalar{nullptr, &recording_kernel, nullptr, &recording_kernel};
  for (const IsaPath path : {IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kNeon}) {
    const auto created = create(testing::config_for(path), no_scalar);
    ASSERT_FALSE(created.has_value()) << simd::describe(path);
    EXPECT_EQ(created.error().fault, WorkloadFault::kConfiguration);
    EXPECT_EQ(created.error().detail,
              "the kernel table has no scalar kernel, which every table must hold");
  }
}

TEST(DenseLinearTable, TheDescriptorListsTheTablesPathsAndTheConfiguredKernelRuns) {
  const Kernels table{&panel_scalar, nullptr, nullptr, &recording_kernel};
  auto created = create(testing::config_for(IsaPath::kNeon, 64), table);
  ASSERT_TRUE(created.has_value()) << created.error().detail;
  const auto workload = std::move(created).value();
  EXPECT_EQ(workload->descriptor().paths, (simd::IsaSet{IsaPath::kScalar, IsaPath::kNeon}));

  const auto refused = create(testing::config_for(IsaPath::kAvx2), table);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().detail,
            "no dense_linear kernel for ISA path avx2; this build has scalar, neon");
}

TEST(DenseLinearDriver, EachTileIsZeroedThenTakesEveryPanelInIncreasingK) {
  for (const std::uint32_t depth : kPanelDepths) {
    const Kernels table{&recording_kernel, nullptr, nullptr, nullptr};
    auto created = create(testing::config_for(IsaPath::kScalar, depth), table);
    ASSERT_TRUE(created.has_value()) << created.error().detail;
    const auto workload = std::move(created).value();
    testing::Serial serial;
    take_calls();
    const Bytes output =
        testing::computed(*workload, 0, Bytes(kInputBytes), serial, std::byte{0xFF});
    const std::vector<Call> seen = take_calls();

    const std::size_t panels = kN / depth;
    ASSERT_EQ(seen.size(), kTiles * panels) << depth;
    for (std::size_t at = 0; at < seen.size(); ++at) {
      const std::size_t tile = at / panels;
      const std::size_t panel = at % panels;
      EXPECT_EQ(seen[at].row, tile * kTileRows) << depth << " call " << at;
      EXPECT_EQ(seen[at].k0, panel * depth) << depth << " call " << at;
      EXPECT_EQ(seen[at].depth, depth);
      EXPECT_EQ(seen[at].tile_was_zero, panel == 0)
          << "the tile is zeroed before its first panel, over the 0xFF the output held";
    }
    for (std::size_t entry = 0; entry < kMatrix; ++entry) {
      ASSERT_EQ(testing::get(output, entry), static_cast<double>(panels))
          << "entry " << entry << " took every panel once, at depth " << depth;
    }
  }
}

TEST(DenseLinearDriver, TheExecutorRunsOneTaskPerTile) {
  /// Counts the tasks it is given and runs them serially.
  class Counting final : public worker::Executor {
   public:
    [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
    void run(std::size_t count, Task task, void* context) noexcept override {
      tasks += count;
      for (std::size_t index = 0; index < count; ++index) {
        task(context, index, 0);
      }
    }
    std::size_t tasks = 0;
  };
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  Counting counting;
  const Bytes input = testing::generated(*workload, 0);
  static_cast<void>(testing::computed(*workload, 0, input, counting));
  EXPECT_EQ(counting.tasks, kTiles);
  EXPECT_EQ(kTiles, 64U);
}

// ---- generate -----------------------------------------------------------------

TEST(DenseLinearGenerate, ElementEIsTheSpecificationsEntryForDrawE) {
  const std::uint64_t seed = 0xC0FFEE;
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar, 0, seed));
  ASSERT_NE(workload, nullptr);
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{7}, ~std::uint64_t{0}}) {
    const Bytes input = testing::generated(*workload, index);
    // Both ends of A, both ends of B, and one inside each.
    for (const std::size_t element :
         {std::size_t{0}, std::size_t{1}, kMatrix / 2, kMatrix - 1, kMatrix, kMatrix + 1,
          kMatrix + kMatrix / 3, 2 * kMatrix - 1}) {
      const double expected = entry_value(rng::u64_of(rng::block_at(seed, index, element)));
      EXPECT_EQ(testing::bits_of(testing::get(input, element)), testing::bits_of(expected))
          << "unit " << index << " element " << element;
    }
  }
}

TEST(DenseLinearGenerate, EachDoubleIsWrittenLittleEndianByteByByte) {
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar, 0, 3));
  ASSERT_NE(workload, nullptr);
  const Bytes input = testing::generated(*workload, 4);
  for (const std::size_t element : {std::size_t{0}, 2 * kMatrix - 1}) {
    const std::uint64_t bits =
        testing::bits_of(entry_value(rng::u64_of(rng::block_at(3, 4, element))));
    for (std::size_t byte = 0; byte < 8; ++byte) {
      EXPECT_EQ(std::to_integer<std::uint64_t>(input[element * 8 + byte]),
                (bits >> (8 * byte)) & 0xFFU)
          << "element " << element << " byte " << byte;
    }
  }
}

TEST(DenseLinearGenerate, EveryEntryIsAMultipleOfTwoToTheMinus52InMinusOneToOne) {
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Bytes input = testing::generated(*workload, 1);
  for (std::size_t element = 0; element < 2 * kMatrix; ++element) {
    const double value = testing::get(input, element);
    ASSERT_GE(value, -1.0) << element;
    ASSERT_LT(value, 1.0) << element;
    const double scaled = value * 4503599627370496.0;  // 2^52: exact
    ASSERT_EQ(scaled, static_cast<double>(static_cast<std::int64_t>(scaled))) << element;
  }
}

TEST(DenseLinearGenerate, TheDecompositionAndThePathDoNotTouchTheInput) {
  const Bytes reference =
      testing::generated(*testing::make(testing::config_for(IsaPath::kScalar, 16, 9)), 2);
  for (const IsaPath path : testing::runnable_paths()) {
    for (const std::uint32_t depth : kPanelDepths) {
      const auto workload = testing::make(testing::config_for(path, depth, 9));
      ASSERT_NE(workload, nullptr);
      EXPECT_EQ(testing::generated(*workload, 2), reference)
          << simd::describe(path) << " at depth " << depth;
    }
  }
}

// ---- compute ------------------------------------------------------------------

TEST(DenseLinearCompute, AnEmptyScratchIsAllItNeeds) {
  const auto workload = testing::make(testing::config_for(IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Bytes input = testing::input_of(
      [](std::size_t i, std::size_t k) { return i == k ? 1.0 : 0.0; },
      [](std::size_t k, std::size_t j) { return static_cast<double>(k * kN + j); });
  Bytes output(kOutputBytes, std::byte{0xAB});
  testing::Serial serial;
  workload->compute(0, input, output, std::span<std::byte>{}, serial);
  for (std::size_t entry = 0; entry < kMatrix; ++entry) {
    ASSERT_EQ(testing::get(output, entry), static_cast<double>(entry))
        << "I B = B, entry " << entry;
  }
}

}  // namespace
}  // namespace loadforge::workloads::dense_linear

namespace loadforge::contracts {
// The registry that links every workload is the integration stream's, and
// runs RegistryContract there.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract);
INSTANTIATE_TYPED_TEST_SUITE_P(DenseLinear, WorkloadContract, workloads::dense_linear::Subjects);
}  // namespace loadforge::contracts
