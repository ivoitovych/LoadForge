// SPDX-License-Identifier: GPL-3.0-or-later
// The chunk kernels (src/workloads/monte_carlo/kernels.hpp), each against the
// scalar replay of the order its path defines.

#include "workloads/monte_carlo/kernels.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <initializer_list>

#include "oracle/workloads/monte_carlo/replay.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::workloads::monte_carlo {
namespace {

constexpr std::uint64_t kKeys[] = {0, 1, 0x0123456789ABCDEFULL, ~std::uint64_t{0}};

TEST(KernelTable, HoldsScalarAndTheNativePathOfThisBuild) {
  const simd::KernelTable<ChunkKernel> table = chunk_kernels();
  EXPECT_NE(table.scalar, nullptr);
  EXPECT_EQ(table.avx512, nullptr) << "no AVX-512 kernel: the journal says why";
  const simd::IsaSet paths = simd::kernel_paths(table);
  EXPECT_TRUE(paths.contains(simd::IsaPath::kScalar));
#if defined(LOADFORGE_SIMD_SCALAR)
  EXPECT_EQ(paths, simd::IsaSet{simd::IsaPath::kScalar});
#elif defined(__x86_64__)
  EXPECT_EQ(paths, (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2}));
  EXPECT_EQ(table.neon, nullptr);
#elif defined(__aarch64__)
  EXPECT_EQ(paths, (simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kNeon}));
  EXPECT_EQ(table.avx2, nullptr);
#endif
  EXPECT_TRUE(paths.intersect(simd::compiled_paths()) == paths) << "never more than compiled";
}

TEST(KernelTable, AnswersNothingForAPathOutsideTheEnumeration) {
  const simd::KernelTable<ChunkKernel> table = chunk_kernels();
  EXPECT_EQ(simd::kernel_at(table, static_cast<simd::IsaPath>(200)), nullptr);
  EXPECT_EQ(simd::kernel_at(table, simd::IsaPath::kScalar), table.scalar);
  EXPECT_EQ(simd::kernel_at(table, simd::IsaPath::kAvx2), table.avx2);
  EXPECT_EQ(simd::kernel_at(table, simd::IsaPath::kAvx512), table.avx512);
  EXPECT_EQ(simd::kernel_at(table, simd::IsaPath::kNeon), table.neon);
}

TEST(Kernels, SumChunksInTheOrderOfTheirPathWhateverTheChunkAndKey) {
  const simd::KernelTable<ChunkKernel> table = chunk_kernels();
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, testing::native_path()}) {
    const ChunkKernel kernel = simd::kernel_at(table, path);
    ASSERT_NE(kernel, nullptr) << simd::describe(path);
    const unsigned lanes = testing::lanes_of(path);
    for (const std::uint64_t key : kKeys) {
      for (const std::uint32_t length : {64U, 256U}) {
        for (const std::uint64_t first :
             {std::uint64_t{0}, std::uint64_t{length}, kSamplesPerBatch - length}) {
          EXPECT_EQ(std::bit_cast<std::uint64_t>(kernel(key, first, length)),
                    std::bit_cast<std::uint64_t>(replay::ordered_chunk(key, lanes, first, length)))
              << simd::describe(path) << " key " << key << " first " << first << " length "
              << length;
        }
      }
    }
  }
}

TEST(Kernels, ASingleLaneIsThePlainLeftToRightSum) {
  const ChunkKernel kernel = chunk_kernels().scalar;
  double sum = 0.0;
  for (std::uint64_t k = 0; k < 64; ++k) {
    sum += replay::f(7, 128 + k);
  }
  EXPECT_EQ(kernel(7, 128, 64), sum);
}

TEST(Kernels, LanesOfDifferentWidthsDifferInBitsButNotInValue) {
  // Same samples, different trees: equal to within the rounding bound.
  const simd::IsaPath native = testing::native_path();
  if (native == simd::IsaPath::kScalar) {
    GTEST_SKIP() << "this build or machine has no vector path";
  }
  const simd::KernelTable<ChunkKernel> table = chunk_kernels();
  const double scalar = table.scalar(5, 0, 4096);
  const double vector = simd::kernel_at(table, native)(5, 0, 4096);
  EXPECT_NEAR(scalar, vector, 1e-9 * scalar);
}

}  // namespace
}  // namespace loadforge::workloads::monte_carlo
