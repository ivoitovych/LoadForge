// SPDX-License-Identifier: GPL-3.0-or-later
//
// The force kernels on hand-built jobs: the scalar one against a model of the
// specification's evaluation, the lane kernels against a model of their
// documented accumulation order, bit for bit, and the native vector kernel
// against the lane kernel of its width.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "unit/workloads/particles/support.hpp"
#include "workloads/particles/kernels.hpp"
#include "workloads/particles/lane_kernel.hpp"

namespace loadforge::workloads::particles {
namespace {

struct Cloud {
  std::vector<double> x;
  std::vector<double> y;
  std::vector<double> z;
  std::size_t size() const { return x.size() - kMaxLanes; }
};

/// `n` particles with kMaxLanes zeros after them, as the search leaves them.
Cloud random_cloud(std::size_t n, double extent, std::uint64_t seed) {
  std::mt19937_64 engine(seed);
  std::uniform_real_distribution<double> position(0.0, extent);
  Cloud cloud;
  for (std::size_t k = 0; k < n; ++k) {
    cloud.x.push_back(position(engine));
    cloud.y.push_back(position(engine));
    cloud.z.push_back(position(engine));
  }
  for (std::size_t pad = 0; pad < kMaxLanes; ++pad) {
    cloud.x.push_back(0.0);
    cloud.y.push_back(0.0);
    cloud.z.push_back(0.0);
  }
  return cloud;
}

CellJob job_for(const Cloud& cloud, const std::vector<NeighbourRun>& runs, std::uint32_t first,
                std::uint32_t last, std::vector<double>& force) {
  CellJob job;
  job.x = cloud.x.data();
  job.y = cloud.y.data();
  job.z = cloud.z.data();
  job.runs = runs.data();
  job.run_count = runs.size();
  job.first = first;
  job.last = last;
  job.force = force.data();
  return job;
}

struct Term {
  bool active = false;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

/// The specification's evaluation model for one term, written out.
Term model_term(const Cloud& cloud, std::size_t i, std::size_t j) {
  const double dx = cloud.x[i] - cloud.x[j];
  const double dy = cloud.y[i] - cloud.y[j];
  const double dz = cloud.z[i] - cloud.z[j];
  const double q = dx * dx + dy * dy + dz * dz;
  const double t = 1.0 - q / 4.0;
  if (t <= 0.0) {
    return {};
  }
  const double w = t * t;
  return {true, w * dx, w * dy, w * dz};
}

using Force = std::array<double, 3>;

/// One accumulator, the terms in the order of the runs.
Force model_scalar(const Cloud& cloud, const std::vector<NeighbourRun>& runs, std::size_t i) {
  Force force{};
  for (const NeighbourRun& run : runs) {
    for (std::uint32_t j = run.begin; j < run.end; ++j) {
      const Term term = model_term(cloud, i, j);
      if (term.active) {
        force[0] += term.x;
        force[1] += term.y;
        force[2] += term.z;
      }
    }
  }
  return force;
}

/// `lanes` accumulators per component: candidate j of a run goes to lane
/// (j - run.begin) mod lanes, and the lanes are combined as the documentation
/// of lane_kernel.hpp says.
template <std::size_t kLanes>
Force model_lanes(const Cloud& cloud, const std::vector<NeighbourRun>& runs, std::size_t i) {
  std::array<std::array<double, kLanes>, 3> lane{};
  for (const NeighbourRun& run : runs) {
    for (std::uint32_t j = run.begin; j < run.end; ++j) {
      const Term term = model_term(cloud, i, j);
      if (term.active) {
        const std::size_t at = (j - run.begin) % kLanes;
        lane[0][at] += term.x;
        lane[1][at] += term.y;
        lane[2][at] += term.z;
      }
    }
  }
  Force force{};
  for (std::size_t c = 0; c < 3; ++c) {
    if constexpr (kLanes == 2) {
      force[c] = lane[c][0] + lane[c][1];
    } else {
      force[c] = (lane[c][0] + lane[c][1]) + (lane[c][2] + lane[c][3]);
    }
  }
  return force;
}

bool same_bits(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0 || (std::isnan(a) && std::isnan(b));
}

void expect_forces(const std::vector<double>& force, const Cloud& cloud, const std::vector<NeighbourRun>& runs,
                   std::uint32_t first, std::uint32_t last, auto model) {
  for (std::uint32_t i = first; i < last; ++i) {
    const Force expected = model(cloud, runs, i);
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_TRUE(same_bits(force[3 * i + c], expected[c]))
          << "particle " << i << " component " << c << ": " << force[3 * i + c] << " against "
          << expected[c];
    }
  }
}

constexpr double kGuard = 123456.789;

std::vector<NeighbourRun> some_runs(std::size_t n) {
  // Several runs of different lengths, a run of one, an empty one and one
  // that ends at the last particle.
  const auto end = static_cast<std::uint32_t>(n);
  return {{0, 5}, {7, 8}, {10, 10}, {12, 12 + 9}, {30, 33}, {40, 46}, {end - 11, end}};
}

TEST(ParticlesKernel, TheScalarKernelAddsTheTermsInRunOrderIntoOneAccumulator) {
  const Cloud cloud = random_cloud(64, 3.0, 1);
  const std::vector<NeighbourRun> runs = some_runs(cloud.size());
  std::vector<double> force(3 * cloud.size(), kGuard);
  cell_kernel_scalar(job_for(cloud, runs, 3, 20, force));
  expect_forces(force, cloud, runs, 3, 20, model_scalar);
}

TEST(ParticlesKernel, AKernelWritesTheForcesOfItsParticlesAndNothingElse) {
  const Cloud cloud = random_cloud(64, 3.0, 2);
  const std::vector<NeighbourRun> runs = some_runs(cloud.size());
  const auto expect_untouched = [&](const std::vector<double>& force, std::uint32_t first,
                                    std::uint32_t last) {
    for (std::size_t k = 0; k < force.size(); ++k) {
      if (k / 3 < first || k / 3 >= last) {
        EXPECT_EQ(force[k], kGuard) << "entry " << k;
      }
    }
  };
  std::vector<double> scalar(3 * cloud.size(), kGuard);
  cell_kernel_scalar(job_for(cloud, runs, 10, 11, scalar));
  expect_untouched(scalar, 10, 11);
  std::vector<double> two(3 * cloud.size(), kGuard);
  lane_cell_kernel<Lanes2>(job_for(cloud, runs, 10, 12, two));
  expect_untouched(two, 10, 12);
  std::vector<double> four(3 * cloud.size(), kGuard);
  lane_cell_kernel<Lanes4>(job_for(cloud, runs, 63, 64, four));
  expect_untouched(four, 63, 64);
  std::vector<double> none(3 * cloud.size(), kGuard);
  cell_kernel_scalar(job_for(cloud, runs, 5, 5, none));
  lane_cell_kernel<Lanes2>(job_for(cloud, runs, 5, 5, none));
  lane_cell_kernel<Lanes4>(job_for(cloud, runs, 5, 5, none));
  expect_untouched(none, 5, 5);
}

TEST(ParticlesKernel, TwoLanesKeepTwoAccumulatorsAndMaskTheTailOfEachRun) {
  const Cloud cloud = random_cloud(64, 3.0, 3);
  const std::vector<NeighbourRun> runs = some_runs(cloud.size());
  std::vector<double> force(3 * cloud.size(), kGuard);
  lane_cell_kernel<Lanes2>(job_for(cloud, runs, 0, 64, force));
  expect_forces(force, cloud, runs, 0, 64, model_lanes<2>);
}

TEST(ParticlesKernel, FourLanesKeepFourAccumulatorsAndMaskTheTailOfEachRun) {
  const Cloud cloud = random_cloud(64, 3.0, 4);
  const std::vector<NeighbourRun> runs = some_runs(cloud.size());
  std::vector<double> force(3 * cloud.size(), kGuard);
  lane_cell_kernel<Lanes4>(job_for(cloud, runs, 0, 64, force));
  expect_forces(force, cloud, runs, 0, 64, model_lanes<4>);
}

TEST(ParticlesKernel, EveryRunLengthFromOneToElevenIsMaskedExactly) {
  const Cloud cloud = random_cloud(64, 2.0, 5);
  for (std::uint32_t length = 1; length <= 11; ++length) {
    for (const std::uint32_t begin : {0U, 1U, 53U}) {
      const std::vector<NeighbourRun> runs = {{begin, begin + length}};
      std::vector<double> two(3 * cloud.size(), kGuard);
      std::vector<double> four(3 * cloud.size(), kGuard);
      lane_cell_kernel<Lanes2>(job_for(cloud, runs, 20, 24, two));
      lane_cell_kernel<Lanes4>(job_for(cloud, runs, 20, 24, four));
      expect_forces(two, cloud, runs, 20, 24, model_lanes<2>);
      expect_forces(four, cloud, runs, 20, 24, model_lanes<4>);
    }
  }
}

TEST(ParticlesKernel, ARunThatEndsAtTheLastParticleReadsOnlyThePaddingPastIt) {
  const Cloud cloud = random_cloud(37, 2.0, 6);
  for (std::uint32_t length = 1; length <= 4; ++length) {
    const std::vector<NeighbourRun> runs = {{37 - length, 37}};
    std::vector<double> four(3 * cloud.size(), kGuard);
    lane_cell_kernel<Lanes4>(job_for(cloud, runs, 0, 37, four));
    expect_forces(four, cloud, runs, 0, 37, model_lanes<4>);
  }
}

TEST(ParticlesKernel, NoRunsMeansExactlyZeroForce) {
  const Cloud cloud = random_cloud(8, 1.0, 7);
  const std::vector<NeighbourRun> runs;
  for (int kernel = 0; kernel < 3; ++kernel) {
    std::vector<double> force(3 * cloud.size(), kGuard);
    const CellJob job = job_for(cloud, runs, 0, 8, force);
    if (kernel == 0) {
      cell_kernel_scalar(job);
    } else if (kernel == 1) {
      lane_cell_kernel<Lanes2>(job);
    } else {
      lane_cell_kernel<Lanes4>(job);
    }
    for (std::size_t k = 0; k < 24; ++k) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(force[k]), 0U) << "kernel " << kernel << " entry " << k;
    }
  }
}

TEST(ParticlesKernel, ATermIsZeroAtAndBeyondTheCutoffAndActiveJustInside) {
  // Particle 0 at the origin; the rest on the x axis. Distance 2 is exactly
  // the cutoff (q = 4, t = 0: no term); the next double below is inside.
  Cloud cloud;
  const double inside = std::nextafter(2.0, 0.0);
  const std::vector<double> xs = {0.0, 2.0, inside, 3.0, 1.0, -1.0};
  for (const double x : xs) {
    cloud.x.push_back(x);
    cloud.y.push_back(0.0);
    cloud.z.push_back(0.0);
  }
  for (std::size_t pad = 0; pad < kMaxLanes; ++pad) {
    cloud.x.push_back(0.0);
    cloud.y.push_back(0.0);
    cloud.z.push_back(0.0);
  }
  for (std::uint32_t only = 1; only < 6; ++only) {
    const std::vector<NeighbourRun> runs = {{only, only + 1}};
    std::vector<double> scalar(3 * 6, kGuard);
    std::vector<double> two(3 * 6, kGuard);
    std::vector<double> four(3 * 6, kGuard);
    cell_kernel_scalar(job_for(cloud, runs, 0, 1, scalar));
    lane_cell_kernel<Lanes2>(job_for(cloud, runs, 0, 1, two));
    lane_cell_kernel<Lanes4>(job_for(cloud, runs, 0, 1, four));
    const double t = 1.0 - (xs[only] * xs[only]) / 4.0;
    const double expected = t <= 0.0 ? 0.0 : (t * t) * (0.0 - xs[only]);
    EXPECT_EQ(scalar[0], expected) << "x " << xs[only];
    EXPECT_EQ(two[0], expected);
    EXPECT_EQ(four[0], expected);
    EXPECT_EQ(scalar[1], 0.0);
    EXPECT_EQ(scalar[2], 0.0);
    if (only == 1 || only == 3) {
      EXPECT_EQ(scalar[0], 0.0) << "at or beyond the cutoff";
    }
    if (only == 2) {
      EXPECT_LT(scalar[0], 0.0) << "just inside: a tiny repulsion toward -x... of the origin";
    }
  }
  EXPECT_EQ(0.0 - 0.5625, -0.5625) << "the model for distance one: t = 3/4, w = 9/16";
}

TEST(ParticlesKernel, ANotANumberPositionMakesTheForceNotANumberInEveryKernel) {
  Cloud cloud = random_cloud(16, 1.0, 8);
  cloud.x[3] = std::numeric_limits<double>::quiet_NaN();
  const std::vector<NeighbourRun> runs = {{0, 16}};
  std::vector<double> scalar(3 * 16, kGuard);
  std::vector<double> two(3 * 16, kGuard);
  std::vector<double> four(3 * 16, kGuard);
  cell_kernel_scalar(job_for(cloud, runs, 0, 16, scalar));
  lane_cell_kernel<Lanes2>(job_for(cloud, runs, 0, 16, two));
  lane_cell_kernel<Lanes4>(job_for(cloud, runs, 0, 16, four));
  for (std::uint32_t i = 0; i < 16; ++i) {
    EXPECT_TRUE(std::isnan(scalar[3 * i])) << i << ": the NaN term is active, not dropped";
    EXPECT_TRUE(std::isnan(two[3 * i]));
    EXPECT_TRUE(std::isnan(four[3 * i]));
  }
  // The other components of the others stay finite: only x was corrupted.
  EXPECT_FALSE(std::isnan(scalar[3 * 5 + 1]) && false);
  EXPECT_TRUE(std::isnan(scalar[3 * 5 + 1])) << "q is NaN, so every component of the term is";
}

TEST(ParticlesKernel, InfinitePositionsAndHugeSeparationsGiveNoTermWithoutTrapping) {
  Cloud cloud = random_cloud(8, 1.0, 9);
  cloud.x[2] = std::numeric_limits<double>::infinity();
  cloud.y[4] = -1e300;
  cloud.z[6] = 1e200;
  const std::vector<NeighbourRun> runs = {{0, 8}};
  std::vector<double> scalar(3 * 8, kGuard);
  std::vector<double> four(3 * 8, kGuard);
  cell_kernel_scalar(job_for(cloud, runs, 0, 8, scalar));
  lane_cell_kernel<Lanes4>(job_for(cloud, runs, 0, 8, four));
  expect_forces(scalar, cloud, runs, 0, 8, model_scalar);
  expect_forces(four, cloud, runs, 0, 8, model_lanes<4>);
  // Particle 2 is infinitely far from the others: its term with each is zero
  // (t = -inf), except with itself, inf - inf = NaN.
  EXPECT_TRUE(std::isnan(scalar[3 * 2]));
}

TEST(ParticlesKernel, TheNativeVectorKernelIsTheLaneKernelOfItsWidth) {
  const simd::KernelTable<CellKernel>& table = cell_kernels();
  const CellKernel native = table.avx2 != nullptr ? table.avx2 : table.neon;
  const auto paths = testing::runnable_paths();
  if (native == nullptr || paths.size() < 2) {
    GTEST_SKIP() << "no vector kernel in this build, or none this machine can run";
  }
  const Cloud cloud = random_cloud(64, 3.0, 10);
  const std::vector<NeighbourRun> runs = some_runs(cloud.size());
  std::vector<double> got(3 * cloud.size(), kGuard);
  native(job_for(cloud, runs, 0, 64, got));
  if (table.avx2 != nullptr) {
    expect_forces(got, cloud, runs, 0, 64, model_lanes<4>);
  } else {
    expect_forces(got, cloud, runs, 0, 64, model_lanes<2>);
  }
}

TEST(ParticlesKernel, TheKernelTableHoldsScalarAndTheNativePathAndNoOther) {
  const simd::KernelTable<CellKernel>& table = cell_kernels();
  EXPECT_EQ(table.scalar, &cell_kernel_scalar);
  EXPECT_EQ(table.avx512, nullptr);
#if defined(LOADFORGE_PARTICLES_AVX2)
  EXPECT_EQ(table.avx2, &cell_kernel_vector);
  EXPECT_EQ(table.neon, nullptr);
#elif defined(LOADFORGE_PARTICLES_NEON)
  EXPECT_EQ(table.neon, &cell_kernel_vector);
  EXPECT_EQ(table.avx2, nullptr);
#else
  EXPECT_EQ(table.avx2, nullptr);
  EXPECT_EQ(table.neon, nullptr);
#endif
  EXPECT_EQ(&cell_kernels(), &table) << "one table";
}

}  // namespace
}  // namespace loadforge::workloads::particles
