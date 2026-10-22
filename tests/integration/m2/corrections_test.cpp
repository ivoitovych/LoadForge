// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract corrections of the M2 integration: the five disagreements recorded
// under "Contract disagreements found" in docs/journal/11-integration-m2.md, each
// decided in docs/journal/m2-contract-corrections-*.md, and each shown fixed here
// against the real components. (Until the corrections these tests stated what was
// OBSERVED; each now states what the corrected contract guarantees.)
//
//   1. the path is chosen from the workload's own paths;
//   2. the runner's lock policy, run as a process that may not lock;
//   3. a unit's output depends on a decomposition the record now carries;
//   4 and 5. are in the supervision tier (tests/supervision/ladder_test.cpp), which
//      has the real re-runs they are about, and in the unit tests of the plan.

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "controller/plan.hpp"
#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/integrity/layout.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "worker/executor/thread_pool.hpp"
#include "workloads/registry/registry.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kFootprint = std::uint64_t{6} << 20U;

// --- 1: select_path against a workload's paths ---------------------------------------

// dispatch.hpp's select_path knows no workload: without a request it picks the widest
// path both of ITS sets hold. On an AVX-512 machine that is one dense_linear has no
// kernel for, and it refuses (the observation of journal 11). The registry now says what
// a workload has, without making one, and the controller chooses from that.
TEST(Corrections, SelectPathAloneStillKnowsNoWorkloadAndTheRegistryGivesItsPathsInstead) {
  const auto probe = isa::make_native_probe();
  const workloads::BuiltinRegistry registry;
  const auto chosen = simd::select_path(std::nullopt, probe->supported(), simd::compiled_paths());
  ASSERT_TRUE(chosen.has_value());
  if (chosen.value() == simd::IsaPath::kAvx512) {
    const auto created = registry.create(
        "dense_linear", workloads::WorkloadConfig{1, kFootprint, 0, chosen.value()});
    ASSERT_FALSE(created.has_value()) << "why the three-set choice exists";
    EXPECT_EQ(created.error().fault, workloads::WorkloadFault::kConfiguration);
  }
  for (const std::string& name : registry.names()) {
    const auto described = registry.descriptor(name);
    ASSERT_TRUE(described.has_value()) << name;
    const auto path = controller::choose_isa(std::nullopt, probe->supported(),
                                             simd::compiled_paths(), described.value().paths);
    ASSERT_TRUE(path.has_value()) << name;
    EXPECT_TRUE(described.value().paths.contains(path.value())) << name;
    const auto created =
        registry.create(name, workloads::WorkloadConfig{1, kFootprint, 0, path.value()});
    ASSERT_TRUE(created.has_value())
        << name << " on " << simd::describe(path.value()) << ": " << created.error().detail;
    EXPECT_EQ(created.value()->descriptor(), described.value()) << name;
  }
}

// --- 2: the lock policy, as a process that may not lock --------------------------------

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

constexpr int kCannotDrop = 90;

/// Runs `body` in a child that has the lock allowance an ordinary user has (8 MiB) and
/// no capability to exceed it, and returns its exit status; skips when the container will
/// not let the test drop privileges.
int as_an_ordinary_user(const std::function<int()>& body) {
  const pid_t child = ::fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    const rlimit limit{std::uint64_t{8} << 20U, std::uint64_t{8} << 20U};
    if (::setrlimit(RLIMIT_MEMLOCK, &limit) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0) {
      ::_exit(kCannotDrop);
    }
    ::_exit(body());
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

/// A runner for `workload` under `policy`, in the state of an ordinary user: what it said it
/// did about locking, and whether two units ran. 0 is "as the contract says".
int run_under(const std::string& name, std::uint64_t footprint, std::size_t threads,
              verification::LockPolicy policy, std::optional<verification::LockOutcome> expected) {
  const workloads::BuiltinRegistry workloads;
  const verification::BuiltinOracleRegistry oracles;
  platform::RealSyscalls syscalls;
  worker::PosixThreads posix;
  const fpenv::RealFpControl fp;
  const workloads::WorkloadConfig config{1, footprint, 0, simd::IsaPath::kScalar};
  auto workload = workloads.create(name, config);
  if (!workload.has_value()) {
    return 80;
  }
  auto oracle = oracles.create(name, config, workload.value()->plan());
  auto pool =
      worker::make_thread_pool(std::vector<worker::ThreadPlacement>(threads), posix, syscalls, fp);
  if (!oracle.has_value() || !pool.has_value()) {
    return 81;
  }
  verification::RunnerSetup setup;
  setup.workload = workload.value().get();
  setup.oracle = oracle.value().get();
  setup.executor = pool.value().get();
  setup.syscalls = &syscalls;
  setup.lock = policy;
  const auto runner = verification::make_slice_runner(setup);
  if (policy == verification::LockPolicy::kRequire &&
      expected == verification::LockOutcome::kFellBack) {
    // What an unprivileged process met before the policy: a refusal, naming the lock.
    return !runner.has_value() && runner.error().fault == verification::RunnerFault::kResources &&
                   runner.error().detail.find("locking failed") != std::string::npos
               ? 0
               : 91;
  }
  if (!runner.has_value()) {
    return 82;
  }
  const verification::MemoryLock& lock = runner.value()->memory_lock();
  if (lock.policy != policy || (expected.has_value() && lock.outcome != *expected)) {
    return 83;
  }
  if ((lock.outcome == verification::LockOutcome::kFellBack) == lock.detail.empty()) {
    return 84;
  }
  auto slice = runner.value()->run_slice(verification::SliceRequest{2});
  if (!slice.has_value() || slice.value().tally.units_computed != 2 ||
      !slice.value().records.empty()) {
    return 85;
  }
  return 0;
}

/// What the lock does under this test: a sanitizer's runtime answers mlock with success
/// without calling it, so under one the allowance is never consulted.
constexpr verification::LockOutcome kRefusedOutcome =
    kSanitized ? verification::LockOutcome::kLocked : verification::LockOutcome::kFellBack;

TEST(Corrections, AnOrdinaryUserCannotLockTheDefaultFootprintAndNowHasThreeWaysToGoOn) {
  if (::geteuid() != 0) {
    GTEST_SKIP() << "the test drops privileges itself and so needs to start with them";
  }
  constexpr std::uint64_t kDefault = std::uint64_t{32} << 20U;
  // The policy the runner always had: refused, and saying why.
  if (!kSanitized) {
    EXPECT_EQ(as_an_ordinary_user([] {
                return run_under("compression", kDefault, 1, verification::LockPolicy::kRequire,
                                 verification::LockOutcome::kFellBack);
              }),
              0);
  }
  // Try: runs, and says the pages were not locked.
  const int tried = as_an_ordinary_user([] {
    return run_under("compression", kDefault, 1, verification::LockPolicy::kTry, kRefusedOutcome);
  });
  if (tried == kCannotDrop) {
    GTEST_SKIP() << "this container will not let the test drop privileges";
  }
  EXPECT_EQ(tried, 0);
  // None: runs, and says it did not ask.
  EXPECT_EQ(as_an_ordinary_user([] {
              return run_under("compression", kDefault, 1, verification::LockPolicy::kNone,
                               verification::LockOutcome::kNotLocked);
            }),
            0);
}

TEST(Corrections, EveryWorkloadRunsAtItsSmallestFootprintForAnOrdinaryUserWhoTriesToLock) {
  if (::geteuid() != 0) {
    GTEST_SKIP() << "the test drops privileges itself and so needs to start with them";
  }
  // Sort's unit is 2.5 MiB, and on three threads its working set is 8.05 MiB: more than the
  // 8 MiB allowance with nothing else in the process, and the case the sweep could not run.
  for (const std::string name : {"compression", "dense_linear", "monte_carlo", "sort"}) {
    const std::uint64_t footprint = name == "sort" ? std::uint64_t{3} << 20U : kFootprint;
    // A workload whose working set fits the allowance locks, one that does not falls back:
    // either is a run, and either says which it was.
    const int status = as_an_ordinary_user([&] {
      return run_under(name, footprint, 3, verification::LockPolicy::kTry, std::nullopt);
    });
    if (status == kCannotDrop) {
      GTEST_SKIP() << "this container will not let the test drop privileges";
    }
    EXPECT_EQ(status, 0) << name;
  }
}

// "Sort's working set is about 3.2 units" -- checked. A unit of sort is 2.5 MiB (1 MiB in,
// 1.5 MiB out). The runner maps the units that fit the footprint, plus ONE more input (the
// bracket's regeneration), plus the scratch of every executor thread (sort's is 1.5 MiB and
// a histogram), plus a line. At one unit that is 2.0 units on one thread and 3.2 on three:
// the figure was the three-thread one. It is right, and it is not in the footprint: the
// footprint counts what the units hold, as workload.hpp now says.
TEST(Corrections, SortsWorkingSetIsItsUnitsPlusOneInputPlusScratchForEveryThread) {
  const workloads::BuiltinRegistry registry;
  const auto sort = registry.create(
      "sort", workloads::WorkloadConfig{1, std::uint64_t{3} << 20U, 0, simd::IsaPath::kScalar});
  ASSERT_TRUE(sort.has_value());
  const workloads::Plan& plan = sort.value()->plan();
  ASSERT_EQ(plan.units, 1U);
  EXPECT_EQ(plan.input_bytes + plan.output_bytes, 2621440U);
  const auto one = verification::integrity::plan_layout(plan, 1, 64);
  const auto three = verification::integrity::plan_layout(plan, 3, 64);
  ASSERT_TRUE(one.has_value() && three.has_value());
  const std::uint64_t unit = plan.input_bytes + plan.output_bytes;
  EXPECT_EQ(one.value().bytes, unit + plan.input_bytes + plan.scratch_bytes);
  EXPECT_EQ(three.value().bytes, unit + plan.input_bytes + 3 * plan.scratch_bytes);
  EXPECT_NEAR(static_cast<double>(one.value().mapping_bytes) / static_cast<double>(unit), 2.0,
              0.02);
  EXPECT_NEAR(static_cast<double>(three.value().mapping_bytes) / static_cast<double>(unit), 3.2,
              0.02);
  EXPECT_GT(three.value().mapping_bytes, std::uint64_t{8} << 20U)
      << "more than an ordinary user may lock, with the control state still to come";
}

// The result says what was chosen and what came of it, through the real binary.
std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(Corrections, TheResultRecordsTheLockPolicyTheControllerChoseAndWhatCameOfIt) {
  const fs::path directory =
      fs::temp_directory_path() / ("loadforge-m2-lock-" + std::to_string(::getpid()));
  fs::create_directories(directory);
  for (const std::string policy : {"none", "require"}) {
    const fs::path out = directory / policy;
    fs::create_directories(out);
    const std::string command = std::string{"'"} + LOADFORGE_BINARY +
                                "' stress --test compression --duration 1s --workers 1 "
                                "--footprint 4MiB --lock " +
                                policy + " --output json --output-dir '" + out.string() + "' > '" +
                                (out / "console.txt").string() + "' 2>&1";
    const int status = std::system(command.c_str());
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << policy << ": " << slurp(out / "console.txt");
    const std::string json = slurp(out / "run.json");
    EXPECT_NE(json.find("\"lock_policy\": \"" + policy + "\""), std::string::npos) << policy;
    const std::string outcome = policy == "none" ? "not locked" : "locked";
    EXPECT_NE(json.find("\"memory_lock\": {\n        \"outcome\": \"" + outcome + "\""),
              std::string::npos)
        << policy << "\n"
        << json.substr(json.find("\"lock_policy\"") - 20, 400);
  }
  std::error_code ignored;
  fs::remove_all(directory, ignored);
}

// --- 3: a unit's output depends on a decomposition the provenance now carries -----------

// workload.hpp: the decomposition is "part of the determinism contract for
// kBitExactFixedDecomposition": dense_linear's OUTPUT depends on the panel depth. Before the
// correction the provenance did not carry it, and a re-run that took the unit from the record
// alone and used the default would have checked different bits from the finding's. The test
// is the reason the record carries it (record.hpp, Provenance::decomposition); the tests
// that it does are the worker's (prepare) and the re-runner's.
TEST(Corrections, AUnitsOutputDependsOnADecompositionTheProvenanceCarriesSoARerunKeepsIt) {
  const workloads::BuiltinRegistry registry;
  const auto fine = registry.create(
      "dense_linear", workloads::WorkloadConfig{9, kFootprint, 16, simd::IsaPath::kScalar});
  const auto coarse = registry.create(
      "dense_linear", workloads::WorkloadConfig{9, kFootprint, 256, simd::IsaPath::kScalar});
  ASSERT_TRUE(fine.has_value() && coarse.has_value());
  EXPECT_EQ(fine.value()->plan().decomposition, 16U) << "the plan says what was resolved";
  const auto resolved = registry.create(
      "dense_linear", workloads::WorkloadConfig{9, kFootprint, 0, simd::IsaPath::kScalar});
  ASSERT_TRUE(resolved.has_value());
  EXPECT_NE(resolved.value()->plan().decomposition, 0U) << "never the 0 that asks for a default";
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
