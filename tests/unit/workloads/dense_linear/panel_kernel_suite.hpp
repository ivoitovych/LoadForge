// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_DENSE_LINEAR_PANEL_KERNEL_SUITE_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_DENSE_LINEAR_PANEL_KERNEL_SUITE_HPP

// The suites every panel kernel is run against, written from panel.hpp's
// statement of what a kernel computes and never from a kernel: the scalar
// kernel's tests (tests/unit/workloads/dense_linear/), the AVX2 kernel's
// (tests/*/primitives/simd/x86/) and the NEON kernel's
// (tests/*/primitives/simd/arm/) instantiate them. panel.hpp is the interface
// between the workload and its kernels, and this is its provider-side suite in
// the sense of ADR 0003: one statement, three implementations held to it.
//
// Subject::kernel(panel)  the kernel under test.
// Subject::kFused         whether its multiply-add is one rounding (the vector
//                         paths) or two (scalar).
// Subject::runs_here()    whether this machine can execute it; a suite on a
//                         machine that cannot fails, saying so, rather than
//                         skipping: an unrun kernel is an untested one.
//
// THE MODEL
// ---------
// One entry, one loop: p = +0.0; for k in [k0, k0 + depth): p <- p + a_ik b_kj,
// the product rounded and then the sum (two roundings), or std::fma (one);
// c_ij <- c_ij + p. std::fma is correctly rounded by definition, whatever
// instruction or library routine implements it on the machine running the
// test, so the fused model is the same function on x86-64 and AArch64 -- and a
// kernel equal to it bit for bit on each is equal to the other kernel too.
//
// PanelKernelSuite (T1): the model bit for bit over the first and last tile,
// the first and last panel, depth 1 and all of k; C added to, not overwritten;
// rows outside the tile and the inputs untouched; buffers at an odd address;
// fused or not, as declared; a subnormal kept; NaN and infinity carried; zero
// products of both signs.
//
// PanelKernelDeterminism (T6): a whole product, driven panel by panel as the
// workload drives it, equals the whole-product model at every depth the
// specification lists; the same rows give the same bits wherever the tile
// sits; and repeated runs give the same bits.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear::testing {

namespace panel_suite {

inline constexpr std::size_t kN = kOrder;
inline constexpr std::size_t kElements = kN * kN;

using Bytes = std::vector<std::byte>;

inline double get(const Bytes& bytes, std::size_t offset, std::size_t index) {
  double value = 0.0;
  std::memcpy(&value, &bytes.at(offset + index * sizeof(double)), sizeof value);
  return value;
}

inline void put(Bytes& bytes, std::size_t offset, std::size_t index, double value) {
  std::memcpy(&bytes.at(offset + index * sizeof(double)), &value, sizeof value);
}

inline std::uint64_t bits(double value) {
  std::uint64_t out = 0;
  std::memcpy(&out, &value, sizeof out);
  return out;
}

/// A, B and C, each `offset` bytes into a buffer of its own, so that a kernel
/// can be given matrices at an address that is not even 8-byte aligned.
struct Matrices {
  explicit Matrices(std::size_t at = 0) : offset(at) {}

  std::size_t offset;
  Bytes a = Bytes(offset + kElements * sizeof(double));
  Bytes b = Bytes(offset + kElements * sizeof(double));
  Bytes c = Bytes(offset + kElements * sizeof(double));

  [[nodiscard]] double a_at(std::size_t i, std::size_t k) const {
    return get(a, offset, i * kN + k);
  }
  [[nodiscard]] double b_at(std::size_t k, std::size_t j) const {
    return get(b, offset, k * kN + j);
  }
  [[nodiscard]] double c_at(std::size_t i, std::size_t j) const {
    return get(c, offset, i * kN + j);
  }
  void set_a(std::size_t i, std::size_t k, double v) { put(a, offset, i * kN + k, v); }
  void set_b(std::size_t k, std::size_t j, double v) { put(b, offset, k * kN + j, v); }
  void set_c(std::size_t i, std::size_t j, double v) { put(c, offset, i * kN + j, v); }

  Panel panel(std::size_t row, std::size_t k0, std::size_t depth) {
    const std::size_t size = kElements * sizeof(double);
    return Panel{std::span<const std::byte>(a).subspan(offset, size),
                 std::span<const std::byte>(b).subspan(offset, size),
                 std::span<std::byte>(c).subspan(offset, size),
                 row,
                 k0,
                 depth};
  }
};

/// Entries of the generator's domain from Philox stream `stream`; C too.
inline Matrices random_matrices(std::uint64_t stream, std::size_t offset = 0) {
  Matrices m(offset);
  for (std::size_t e = 0; e < kElements; ++e) {
    put(m.a, offset, e, entry_value(rng::u64_of(rng::block_at(1, stream, e))));
    put(m.b, offset, e, entry_value(rng::u64_of(rng::block_at(1, stream, kElements + e))));
    put(m.c, offset, e, entry_value(rng::u64_of(rng::block_at(2, stream, e))));
  }
  return m;
}

/// The model's panel sum for one entry.
inline double panel_sum(const Matrices& m, std::size_t i, std::size_t j, std::size_t k0,
                        std::size_t depth, bool fused) {
  double p = 0.0;
  for (std::size_t k = k0; k < k0 + depth; ++k) {
    if (fused) {
      p = std::fma(m.a_at(i, k), m.b_at(k, j), p);
    } else {
      const double product = m.a_at(i, k) * m.b_at(k, j);
      p = p + product;
    }
  }
  return p;
}

/// The model's whole product at panel depth `depth`: c from +0.0, each panel's
/// sum added in increasing k. Over plain doubles, every entry at once.
inline std::vector<double> product(const Matrices& m, std::size_t depth, bool fused) {
  std::vector<double> a(kElements);
  std::vector<double> b(kElements);
  for (std::size_t e = 0; e < kElements; ++e) {
    a[e] = get(m.a, m.offset, e);
    b[e] = get(m.b, m.offset, e);
  }
  std::vector<double> c(kElements, 0.0);
  std::vector<double> p(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    for (std::size_t k0 = 0; k0 < kN; k0 += depth) {
      std::fill(p.begin(), p.end(), 0.0);
      for (std::size_t k = k0; k < k0 + depth; ++k) {
        // Raw pointers, because an unoptimised build calls operator[] for
        // every element, and this loop is 2^24 steps per depth.
        const double aik = a[i * kN + k];
        const double* row = b.data() + k * kN;
        double* sums = p.data();
        for (std::size_t j = 0; j < kN; ++j) {
          if (fused) {
            sums[j] = std::fma(aik, row[j], sums[j]);
          } else {
            const double term = aik * row[j];
            sums[j] = sums[j] + term;
          }
        }
      }
      for (std::size_t j = 0; j < kN; ++j) {
        c[i * kN + j] = c[i * kN + j] + p[j];
      }
    }
  }
  return c;
}

/// Six panels: each corner of the (row, k0, depth) space the workload uses.
struct Case {
  std::size_t row;
  std::size_t k0;
  std::size_t depth;
};
inline constexpr Case kCases[] = {{0, 0, 64},  {kN - kTileRows, kN - 16, 16},
                                  {128, 0, 1}, {4, kN - 1, 1},
                                  {8, 0, kN},  {252, 128, 128}};

}  // namespace panel_suite

template <typename Subject>
class PanelKernelSuite : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Subject::runs_here())
        << "this machine cannot execute the kernel under test, so it cannot be tested here";
  }
};
TYPED_TEST_SUITE_P(PanelKernelSuite);

TYPED_TEST_P(PanelKernelSuite, EveryEntryOfTheTileIsTheModelBitForBit) {
  using namespace panel_suite;
  for (const Case& at : kCases) {
    Matrices m = random_matrices(at.row + at.k0 + at.depth);
    const Matrices before = m;
    TypeParam::kernel(m.panel(at.row, at.k0, at.depth));
    for (std::size_t i = 0; i < kN; ++i) {
      const bool in_tile = i >= at.row && i < at.row + kTileRows;
      for (std::size_t j = 0; j < kN; ++j) {
        const double expected = in_tile ? before.c_at(i, j) + panel_sum(before, i, j, at.k0,
                                                                        at.depth, TypeParam::kFused)
                                        : before.c_at(i, j);
        ASSERT_EQ(bits(m.c_at(i, j)), bits(expected))
            << "row " << at.row << " k0 " << at.k0 << " depth " << at.depth << ": c(" << i << ", "
            << j << ")" << (in_tile ? "" : ", outside the tile");
      }
    }
    EXPECT_EQ(m.a, before.a) << "A is only read";
    EXPECT_EQ(m.b, before.b) << "B is only read";
  }
}

TYPED_TEST_P(PanelKernelSuite, MatricesAtAnyAddressAreReadAndWrittenAlike) {
  using namespace panel_suite;
  Matrices aligned = random_matrices(77);
  TypeParam::kernel(aligned.panel(64, 32, 32));
  for (const std::size_t offset : {std::size_t{1}, std::size_t{3}, std::size_t{8}}) {
    Matrices shifted = random_matrices(77, offset);
    TypeParam::kernel(shifted.panel(64, 32, 32));
    for (std::size_t e = 0; e < kElements; ++e) {
      ASSERT_EQ(bits(get(shifted.c, offset, e)), bits(get(aligned.c, 0, e)))
          << "offset " << offset << " element " << e;
    }
  }
}

/// a0 b0 = -(1 + 2^-29) exactly, and a1 b1 = 1 + 2^-29 + 2^-60, whose rounding
/// loses the 2^-60. Product, then sum: exactly +0.0. Fused: 2^-60.
TYPED_TEST_P(PanelKernelSuite, TheMultiplyAddIsFusedExactlyWhenTheKernelSaysSo) {
  using namespace panel_suite;
  Matrices m;
  const double x = 1.0 + std::ldexp(1.0, -30);
  m.set_a(0, 0, -1.0);
  m.set_b(0, 0, 1.0 + std::ldexp(1.0, -29));
  m.set_a(0, 1, x);
  m.set_b(1, 0, x);
  TypeParam::kernel(m.panel(0, 0, 2));
  EXPECT_EQ(m.c_at(0, 0), TypeParam::kFused ? std::ldexp(1.0, -60) : 0.0)
      << (TypeParam::kFused ? "a fused kernel keeps the 2^-60"
                            : "the product is rounded before the sum: -ffp-contract=off holds");
}

TYPED_TEST_P(PanelKernelSuite, ASubnormalIsKeptNotFlushed) {
  using namespace panel_suite;
  Matrices m;
  const double tiny = std::numeric_limits<double>::denorm_min();
  m.set_a(0, 0, tiny);
  m.set_b(0, 0, 1.0);
  m.set_a(1, 1, 0.5);
  m.set_b(1, 1, std::numeric_limits<double>::min());
  TypeParam::kernel(m.panel(0, 0, kN));
  EXPECT_EQ(m.c_at(0, 0), tiny);
  EXPECT_EQ(m.c_at(1, 1), std::numeric_limits<double>::min() / 2);
}

TYPED_TEST_P(PanelKernelSuite, NaNAndInfinityAreCarriedNotDropped) {
  using namespace panel_suite;
  Matrices m;
  m.set_a(0, 0, std::numeric_limits<double>::quiet_NaN());
  m.set_a(1, 0, std::numeric_limits<double>::infinity());
  m.set_b(0, 0, 1.0);  // b(0, 1) stays 0
  TypeParam::kernel(m.panel(0, 0, kN));
  EXPECT_TRUE(std::isnan(m.c_at(0, 0))) << "NaN times 1";
  EXPECT_TRUE(std::isnan(m.c_at(0, 1))) << "NaN times 0";
  EXPECT_EQ(m.c_at(1, 0), std::numeric_limits<double>::infinity());
  EXPECT_TRUE(std::isnan(m.c_at(1, 1))) << "infinity times 0";
  EXPECT_EQ(bits(m.c_at(2, 0)), 0U) << "row 2 met no special value";
}

TYPED_TEST_P(PanelKernelSuite, ZeroProductsOfEitherSignLeaveCAsItWas) {
  using namespace panel_suite;
  Matrices m;
  for (std::size_t k = 0; k < kN; ++k) {
    m.set_a(0, k, -0.0);
    m.set_b(k, 0, 0.75);
  }
  m.set_c(0, 0, -0.5);
  m.set_c(0, 1, -0.0);
  TypeParam::kernel(m.panel(0, 0, kN));
  EXPECT_EQ(m.c_at(0, 0), -0.5);
  EXPECT_EQ(bits(m.c_at(0, 1)), 0U) << "p is +0.0, and -0.0 + +0.0 is +0.0";
}

REGISTER_TYPED_TEST_SUITE_P(PanelKernelSuite, EveryEntryOfTheTileIsTheModelBitForBit,
                            MatricesAtAnyAddressAreReadAndWrittenAlike,
                            TheMultiplyAddIsFusedExactlyWhenTheKernelSaysSo,
                            ASubnormalIsKeptNotFlushed, NaNAndInfinityAreCarriedNotDropped,
                            ZeroProductsOfEitherSignLeaveCAsItWas);

template <typename Subject>
class PanelKernelDeterminism : public PanelKernelSuite<Subject> {
 protected:
  /// C = A B as the workload drives a kernel: each tile zeroed, then every
  /// panel of it in increasing k. Tiles in the order given.
  static void multiply(panel_suite::Matrices& m, std::size_t depth,
                       const std::vector<std::size_t>& tiles) {
    for (const std::size_t tile : tiles) {
      for (std::size_t j = 0; j < panel_suite::kN; ++j) {
        for (std::size_t r = 0; r < kTileRows; ++r) {
          m.set_c(tile * kTileRows + r, j, 0.0);
        }
      }
      for (std::size_t k0 = 0; k0 < panel_suite::kN; k0 += depth) {
        Subject::kernel(m.panel(tile * kTileRows, k0, depth));
      }
    }
  }

  static std::vector<std::size_t> every_tile(bool reversed) {
    std::vector<std::size_t> tiles;
    for (std::size_t tile = 0; tile < kTiles; ++tile) {
      tiles.push_back(reversed ? kTiles - 1 - tile : tile);
    }
    return tiles;
  }
};
TYPED_TEST_SUITE_P(PanelKernelDeterminism);

TYPED_TEST_P(PanelKernelDeterminism, AWholeProductIsTheModelAtEveryPanelDepth) {
  using namespace panel_suite;
  const Matrices input = random_matrices(5);
  for (const std::uint32_t depth : kPanelDepths) {
    Matrices m = input;
    this->multiply(m, depth, this->every_tile(depth % 32 == 0));
    const std::vector<double> expected = product(input, depth, TypeParam::kFused);
    for (std::size_t e = 0; e < kElements; ++e) {
      ASSERT_EQ(bits(get(m.c, 0, e)), bits(expected[e]))
          << "depth " << depth << ": c(" << e / kN << ", " << e % kN << ")";
    }
  }
}

TYPED_TEST_P(PanelKernelDeterminism, TheSameRowsGiveTheSameBitsInAnyTile) {
  using namespace panel_suite;
  Matrices m = random_matrices(6);
  // Row block 0 copied into the last tile: the kernel cannot know where it is.
  for (std::size_t r = 0; r < kTileRows; ++r) {
    for (std::size_t k = 0; k < kN; ++k) {
      m.set_a(kN - kTileRows + r, k, m.a_at(r, k));
    }
  }
  this->multiply(m, 32, {0, kTiles - 1});
  for (std::size_t r = 0; r < kTileRows; ++r) {
    for (std::size_t j = 0; j < kN; ++j) {
      ASSERT_EQ(bits(m.c_at(kN - kTileRows + r, j)), bits(m.c_at(r, j))) << r << ", " << j;
    }
  }
}

TYPED_TEST_P(PanelKernelDeterminism, RepeatedRunsGiveTheSameBits) {
  using namespace panel_suite;
  const Matrices input = random_matrices(8);
  Matrices first = input;
  Matrices second = input;
  this->multiply(first, 16, this->every_tile(false));
  this->multiply(second, 16, this->every_tile(true));
  EXPECT_EQ(first.c, second.c);
}

REGISTER_TYPED_TEST_SUITE_P(PanelKernelDeterminism, AWholeProductIsTheModelAtEveryPanelDepth,
                            TheSameRowsGiveTheSameBitsInAnyTile, RepeatedRunsGiveTheSameBits);

}  // namespace loadforge::workloads::dense_linear::testing

#endif
