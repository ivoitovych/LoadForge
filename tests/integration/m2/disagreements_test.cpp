// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where two components, built apart from one contract, read it differently --
// or where the contract leaves a gap the integration had to fill. Each test
// here states what was OBSERVED when the real components were run together and
// names the clauses that disagree; each is also written up under "Contract
// disagreements found" in docs/journal/11-integration-m2.md. A test here that
// starts failing means a disagreement was fixed: delete the paragraph that
// records it and the test with it.

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "worker/executor/thread_pool.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge {
namespace {

constexpr std::uint64_t kFootprint = std::uint64_t{6} << 20U;

// dispatch.hpp (select_path, "without a request: the first path in kPreference
// that both sets hold") against workload.hpp (Descriptor::paths, "the ISA paths
// it has kernels for") and dense_linear's refusal of a path it has no kernel
// for. Nothing in select_path's signature can know a workload's kernels, so the
// path "chosen once by the worker" is one a workload may refuse. The integration
// intersects the workload's paths in (controller/plan.hpp, choose_isa).
TEST(Disagreements, TheUnrequestedPathSelectIsNotAlwaysOneAWorkloadHas) {
  const auto probe = isa::make_native_probe();
  const auto chosen = simd::select_path(std::nullopt, probe->supported(), simd::compiled_paths());
  ASSERT_TRUE(chosen.has_value());
  if (chosen.value() != simd::IsaPath::kAvx512) {
    GTEST_SKIP() << "this machine's widest path is " << simd::describe(chosen.value())
                 << ", which dense_linear has a kernel for; the disagreement needs AVX-512";
  }
  const workloads::BuiltinRegistry registry;
  const auto created =
      registry.create("dense_linear", workloads::WorkloadConfig{1, kFootprint, 0, chosen.value()});
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kConfiguration);
}

// verification/runner.hpp (kResources, "the working set could not be allocated
// or placed") against the runner's own choice to lock the whole working set:
// RunnerSetup has no memory policy, so a user who may not lock the footprint
// cannot run at all, and the default footprint of the command is larger than
// the 8 MiB an unprivileged process may lock by default.
TEST(Disagreements, AnUnprivilegedProcessCannotLockTheWorkingSetTheRunnerAlwaysLocks) {
  if (::geteuid() != 0) {
    GTEST_SKIP() << "the test drops privileges itself and so needs to start with them";
  }
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    // A process with the lock allowance an ordinary user has (8 MiB) and no
    // capability to exceed it.
    const rlimit limit{std::uint64_t{8} << 20U, std::uint64_t{8} << 20U};
    if (::setrlimit(RLIMIT_MEMLOCK, &limit) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0) {
      ::_exit(90);
    }
    const workloads::BuiltinRegistry workloads;
    const verification::BuiltinOracleRegistry oracles;
    platform::RealSyscalls syscalls;
    worker::PosixThreads threads;
    const fpenv::RealFpControl fp;
    const workloads::WorkloadConfig config{1, std::uint64_t{32} << 20U, 0, simd::IsaPath::kScalar};
    auto workload = workloads.create("compression", config);
    auto oracle = oracles.create("compression", config, workload.value()->plan());
    auto pool =
        worker::make_thread_pool(std::vector<worker::ThreadPlacement>(1), threads, syscalls, fp);
    verification::RunnerSetup setup;
    setup.workload = workload.value().get();
    setup.oracle = oracle.value().get();
    setup.executor = pool.value().get();
    setup.syscalls = &syscalls;
    const auto runner = verification::make_slice_runner(setup);
    const bool refused = !runner.has_value() &&
                         runner.error().fault == verification::RunnerFault::kResources &&
                         runner.error().detail.find("locking failed") != std::string::npos;
    ::_exit(refused ? 0 : 91);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 90) {
    GTEST_SKIP() << "this container will not let the test drop privileges";
  }
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "an unprivileged process was expected to be refused with kResources, 'locking failed'";
}

// verification/isolation.hpp (Rerunner: "the unit is the finding's provenance:
// workload, workload_version, iteration, seed, isa") against workload.hpp
// (decomposition is "part of the determinism contract for
// kBitExactFixedDecomposition"): dense_linear's OUTPUT depends on the panel
// depth, which the provenance does not carry, so a re-run that took the unit
// from the record alone would compute different bits than the finding is about.
TEST(Disagreements, AUnitsOutputDependsOnADecompositionTheProvenanceDoesNotCarry) {
  const workloads::BuiltinRegistry registry;
  const auto fine = registry.create(
      "dense_linear", workloads::WorkloadConfig{9, kFootprint, 16, simd::IsaPath::kScalar});
  const auto coarse = registry.create(
      "dense_linear", workloads::WorkloadConfig{9, kFootprint, 256, simd::IsaPath::kScalar});
  ASSERT_TRUE(fine.has_value() && coarse.has_value());
  const workloads::Plan& plan = fine.value()->plan();
  std::vector<std::byte> input(plan.input_bytes);
  std::vector<std::byte> a(plan.output_bytes);
  std::vector<std::byte> b(plan.output_bytes);
  std::vector<std::byte> scratch(plan.scratch_bytes);
  class Serial final : public worker::Executor {
   public:
    [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
    void run(std::size_t count, Task task, void* context) noexcept override {
      for (std::size_t index = 0; index < count; ++index) {
        task(context, index, 0);
      }
    }
  } executor;
  fine.value()->generate(3, input);
  fine.value()->compute(3, input, a, scratch, executor);
  coarse.value()->compute(3, input, b, scratch, executor);
  EXPECT_NE(a, b) << "the same unit, two panel depths: different bits, both within tolerance";
}

}  // namespace
}  // namespace loadforge
