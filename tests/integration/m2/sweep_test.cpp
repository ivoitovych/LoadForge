// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: every workload of the build, created by the registry with every
// decomposition it lists and on every ISA path this machine can run, handed to
// the oracle registry for the plan it resolved, and run through the real
// input-integrity runner on one thread and on three, at unit indices from 0 up
// to the last that fit in 64 bits (the second unit of that run wraps). Streams wrote the workloads, the oracles and the runner apart, from
// the specifications and the contracts; this is where their readings of one
// configuration meet, and nowhere else do they.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/runner.hpp"
#include "worker/executor/thread_pool.hpp"
#include "workloads/dense_linear/spec.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/registry/registry.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge {
namespace {

constexpr std::uint64_t kFootprint = std::uint64_t{8} << 20U;

/// Every decomposition value a workload lists, and 0 for its default.
std::vector<std::uint32_t> decompositions_of(const std::string& name) {
  std::vector<std::uint32_t> values{0};
  if (name == "dense_linear") {
    values.insert(values.end(), workloads::dense_linear::kPanelDepths.begin(),
                  workloads::dense_linear::kPanelDepths.end());
  } else if (name == "sort") {
    values.insert(values.end(), workloads::sort::kChunkCounts.begin(),
                  workloads::sort::kChunkCounts.end());
  } else if (name == "monte_carlo") {
    values.insert(values.end(), workloads::monte_carlo::kChunkLengths.begin(),
                  workloads::monte_carlo::kChunkLengths.end());
  } else {
    values.push_back(1);
  }
  return values;
}

TEST(Sweep, EveryWorkloadItsOracleAndTheRunnerAgreeOnEveryConfigurationTheWorkloadAccepts) {
  const workloads::BuiltinRegistry workloads;
  const verification::BuiltinOracleRegistry oracles;
  const auto probe = isa::make_native_probe();
  platform::RealSyscalls syscalls;
  worker::PosixThreads threads;
  fpenv::RealFpControl fp;
  std::size_t configurations = 0;
  for (const std::string& name : workloads.names()) {
    for (const std::uint32_t decomposition : decompositions_of(name)) {
      for (const simd::IsaPath path :
           {simd::IsaPath::kScalar, simd::IsaPath::kAvx2, simd::IsaPath::kAvx512,
            simd::IsaPath::kNeon}) {
        if (!probe->supported().intersect(simd::compiled_paths()).contains(path)) {
          continue;
        }
        const workloads::WorkloadConfig config{0xC0FFEE, kFootprint, decomposition, path};
        auto created = workloads.create(name, config);
        if (!created.has_value()) {
          // A path the workload has no kernel for is its refusal to make, and
          // it must say so as a configuration fault and not as anything else.
          EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kConfiguration)
              << name << " " << simd::describe(path) << ": " << created.error().detail;
          continue;
        }
        const workloads::Workload& workload = *created.value();
        auto oracle = oracles.create(name, config, workload.plan());
        ASSERT_TRUE(oracle.has_value())
            << name << " decomposition " << decomposition << " " << simd::describe(path)
            << ": the workload accepts it and its oracle does not: " << oracle.error().detail;
        EXPECT_EQ(oracle.value()->contract(), workload.descriptor().contract) << name;
        // Three threads for the default decomposition; the others, one: the
        // thread count is the executor's and the decomposition is not.
        for (const std::size_t thread_count : {std::size_t{1}, std::size_t{3}}) {
          if (thread_count > 1 && decomposition != 0) {
            continue;
          }
          std::vector<worker::ThreadPlacement> placements(thread_count);
          auto pool = worker::make_thread_pool(placements, threads, syscalls, fp);
          ASSERT_TRUE(pool.has_value());
          for (const std::uint64_t first :
               {std::uint64_t{0}, ~std::uint64_t{0} - 1U}) {
            verification::RunnerSetup setup;
            setup.workload = &workload;
            setup.oracle = oracle.value().get();
            setup.executor = pool.value().get();
            setup.syscalls = &syscalls;
            setup.line_bytes = 64;
            setup.base.workload = name;
            setup.base.workload_version = workload.descriptor().version;
            setup.base.seed = config.seed;
            setup.base.isa = path;
            setup.first_index = first;
            auto runner = verification::make_slice_runner(setup);
            ASSERT_TRUE(runner.has_value()) << name << ": " << runner.error().detail;
            auto slice = runner.value()->run_slice(verification::SliceRequest{2});
            ASSERT_TRUE(slice.has_value());
            const std::string where = name + " decomposition " + std::to_string(decomposition) +
                                      " " + std::string{simd::describe(path)} + " threads " +
                                      std::to_string(thread_count) + " from unit " +
                                      std::to_string(first);
            EXPECT_TRUE(slice.value().records.empty()) << where;
            EXPECT_EQ(slice.value().tally.units_computed, 2U) << where;
            EXPECT_EQ(slice.value().tally.oracle_failed, 0U) << where;
            EXPECT_EQ(slice.value().tally.oracle_not_performed, 0U) << where;
            EXPECT_GT(slice.value().tally.oracle_blocks, 0U) << where;
            auto last = runner.value()->finish();
            ASSERT_TRUE(last.has_value());
            EXPECT_TRUE(last.value().records.empty()) << where;
            ++configurations;
          }
        }
      }
    }
  }
  EXPECT_GT(configurations, 20U);
}

}  // namespace
}  // namespace loadforge
