// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the controller decides before it forks: which CPUs each worker takes,
// which ISA path a workload runs on, and the plan of each configured workload
// -- a real one with every choice made, or the null workload of M1 -- or the
// reason the run cannot start.

#include "controller/plan.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "contracts/simd/select_path_contract.hpp"
#include "unit/controller/consumer_models.hpp"
#include "unit/controller/machine_tree.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::contracts {
namespace {

// choose_isa with a workload that has every kernel IS select_path, to the
// contract's letter: the suite of the dispatch contract, run against the
// controller's use of it.
struct ChooseIsaSubject {
  static core::Result<simd::IsaPath, simd::DispatchError> select(
      std::optional<simd::IsaPath> requested, simd::IsaSet supported, simd::IsaSet compiled) {
    return controller::choose_isa(requested, supported, compiled,
                                  simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2,
                                               simd::IsaPath::kAvx512, simd::IsaPath::kNeon});
  }
};
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerChoice, SelectPathContract,
                               ::testing::Types<ChooseIsaSubject>);

}  // namespace
}  // namespace loadforge::contracts

namespace loadforge::controller {
namespace {

using controller_testing::MachineShape;
using controller_testing::MachineTree;
using simd::IsaPath;
using simd::IsaSet;

TEST(AssignCpus, ABlockOfThreadsStartingAtTheWorkersSlotWrappingRound) {
  const std::vector<std::uint32_t> all{0, 1, 2, 3};
  EXPECT_EQ(assign_cpus(0, 1, all), (std::vector<std::uint32_t>{0}));
  EXPECT_EQ(assign_cpus(3, 1, all), (std::vector<std::uint32_t>{3}));
  EXPECT_EQ(assign_cpus(4, 1, all), (std::vector<std::uint32_t>{0})) << "more workers than CPUs";
  EXPECT_EQ(assign_cpus(1, 2, all), (std::vector<std::uint32_t>{2, 3}));
  EXPECT_EQ(assign_cpus(2, 3, all), (std::vector<std::uint32_t>{2, 3, 0})) << "wraps";
}

TEST(AssignCpus, NeverMoreCpusThanThereAreAndNoneWhenThereAreNone) {
  EXPECT_EQ(assign_cpus(0, 9, {5, 6}), (std::vector<std::uint32_t>{5, 6}));
  EXPECT_TRUE(assign_cpus(0, 2, {}).empty());
  EXPECT_EQ(assign_cpus(0, 1, {7}), (std::vector<std::uint32_t>{7}));
}

TEST(ChooseIsa, ARequestIsUsedOrRefusedAndTheWorkloadsKernelsAreAThirdSet) {
  const IsaSet everything{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512};
  const IsaSet no_avx512{IsaPath::kScalar, IsaPath::kAvx2};
  EXPECT_EQ(choose_isa(IsaPath::kAvx2, everything, everything, no_avx512).value(), IsaPath::kAvx2);
  const auto lacks = choose_isa(IsaPath::kAvx512, everything, everything, no_avx512);
  ASSERT_FALSE(lacks.has_value());
  EXPECT_EQ(lacks.error().fault, simd::DispatchFault::kNotCompiled);
  EXPECT_EQ(lacks.error().requested, IsaPath::kAvx512);
  EXPECT_EQ(lacks.error().detail, "the workload has no avx512 kernel");
  const auto unsupported = choose_isa(IsaPath::kAvx512, no_avx512, everything, everything);
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_EQ(unsupported.error().fault, simd::DispatchFault::kNotSupported);
}

TEST(ChooseIsa, WithoutARequestTheWidestPathAllThreeSetsHoldIsChosen) {
  // Journal fragment 06, question 2: select_path alone would choose AVX-512
  // for a workload that has no such kernel.
  const IsaSet everything{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512};
  EXPECT_EQ(choose_isa(std::nullopt, everything, everything, everything).value(), IsaPath::kAvx512);
  EXPECT_EQ(
      choose_isa(std::nullopt, everything, everything, IsaSet{IsaPath::kScalar, IsaPath::kAvx2})
          .value(),
      IsaPath::kAvx2);
  EXPECT_EQ(choose_isa(std::nullopt, everything, everything, IsaSet{IsaPath::kScalar}).value(),
            IsaPath::kScalar);
  EXPECT_EQ(choose_isa(std::nullopt, IsaSet{}, everything, everything).value(), IsaPath::kScalar);
}

TEST(DescribeState, SaysEveryPropertyThatExistsOnTheArchitectureAndNoOtherInTheRequiredStateWords) {
  EXPECT_EQ(describe_state(fpenv::required_state(true, false)),
            "rounding to nearest, ties to even, flush-to-zero off, denormals-are-zero off");
  EXPECT_EQ(describe_state(fpenv::required_state(false, true)),
            "rounding to nearest, ties to even, flush-to-zero off, default-NaN off");
  fpenv::FpState dirty = fpenv::required_state(true, true);
  dirty.flush_to_zero = true;
  dirty.denormals_are_zero = true;
  dirty.default_nan = true;
  dirty.rounding = fpenv::Rounding::kUpward;
  EXPECT_EQ(describe_state(dirty),
            "rounding upward, flush-to-zero on, denormals-are-zero on, default-NaN on");
  EXPECT_EQ(describe_state(fpenv::FpState{}),
            "rounding to nearest, ties to even, flush-to-zero off");
}

// --- plan_worker ---------------------------------------------------------------------

/// The real workloads, with the ability to fail the second creation of a
/// workload (the one made for the chosen path) or its first.
class ScriptedRegistry final : public workloads::Registry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return inner.names(); }
  [[nodiscard]] core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      std::string_view name, const workloads::WorkloadConfig& config) const override {
    ++creations;
    if (fail_from != 0 && creations >= fail_from) {
      return workloads::WorkloadError{workloads::WorkloadFault::kConfiguration, "scripted"};
    }
    return inner.create(name, config);
  }
  workloads::BuiltinRegistry inner;
  mutable int creations = 0;
  int fail_from = 0;
};

config::Workload entry(std::string name, config::Threads threads = config::Threads::count(1),
                       std::optional<core::ByteSize> memory = std::nullopt) {
  return config::Workload{std::move(name), threads, true, memory};
}

struct Planning {
  ScriptedRegistry registry;
  controller_testing::ModelProbe probe{IsaSet{IsaPath::kScalar, IsaPath::kAvx2}};
  VerificationOptions options;
  Planning() {
    options.registry = &registry;
    options.isa_probe = &probe;
    options.line_bytes = 64;
    options.cpus = {0, 1, 2, 3};
    options.footprint_bytes = std::uint64_t{8} << 20U;
  }
};

TEST(PlanWorker, ANameThatIsNotARegisteredWorkloadIsTheNullWorkloadOfM1) {
  Planning planning;
  auto planned = plan_worker(entry("first"), 0, std::nullopt, planning.options, nullptr);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  EXPECT_FALSE(planned.value().args.has_value());
  EXPECT_FALSE(planned.value().work.has_value());
  EXPECT_EQ(planned.value().description, "first: null workload");
  EXPECT_TRUE(planned.value().cpus.empty());

  planning.options.registry = nullptr;
  auto no_registry = plan_worker(entry("compression"), 0, std::nullopt, planning.options, nullptr);
  ASSERT_TRUE(no_registry.has_value());
  EXPECT_FALSE(no_registry.value().args.has_value()) << "without a registry every name is a label";
}

// The workload has a kernel for a path only if the build compiled one: a
// scalar-only build offers the scalar path even to a machine that has more.
IsaPath widest_kernel_of_dense_linear() {
  const workloads::BuiltinRegistry registry;
  const auto made = registry.create(
      "dense_linear", workloads::WorkloadConfig{1, std::uint64_t{8} << 20U, 0, IsaPath::kScalar});
  return made.value()->descriptor().paths.contains(IsaPath::kAvx2) ? IsaPath::kAvx2
                                                                   : IsaPath::kScalar;
}

TEST(PlanWorker, ARealWorkloadGetsEveryChoiceMadeOnceAndSaysWhatTheyAre) {
  const MachineTree tree(MachineShape{4, 64, true});
  const topology::Snapshot machine = tree.accepted();
  Planning planning;
  planning.options.seed = 10;
  planning.options.decomposition = 0;
  planning.options.duty = verification::DutyCycle{2, 3};
  auto planned = plan_worker(entry("dense_linear"), 2, std::nullopt, planning.options, &machine);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  const WorkerPlan& plan = planned.value();
  ASSERT_TRUE(plan.args.has_value());
  const worker::WorkloadArgs& args = *plan.args;
  EXPECT_EQ(args.workload, "dense_linear");
  EXPECT_EQ(args.seed, 10 + 2 * kSeedStride);
  EXPECT_EQ(args.footprint_bytes, std::uint64_t{8} << 20U);
  EXPECT_EQ(args.threads, 1U);
  EXPECT_EQ(args.isa, widest_kernel_of_dense_linear())
      << "the widest this machine and workload both have";
  EXPECT_EQ(args.duty, (verification::DutyCycle{2, 3}));
  EXPECT_EQ(args.line_bytes, 64U);
  EXPECT_EQ(plan.cpus, (std::vector<std::uint32_t>{2}));
  EXPECT_EQ(args.physical_core, 2U);
  EXPECT_EQ(args.numa_node, 0U);
  ASSERT_TRUE(plan.work.has_value());
  EXPECT_EQ(plan.work->descriptor.name, "dense_linear");
  EXPECT_EQ(plan.work->isa, widest_kernel_of_dense_linear());
  EXPECT_EQ(plan.work->threads, 1U);
  EXPECT_EQ(plan.work->cpu, 2U);
  EXPECT_EQ(plan.work->seed, args.seed);
  EXPECT_EQ(plan.work->footprint_bytes, args.footprint_bytes);
  EXPECT_EQ(plan.work->line_bytes, 64U);
  EXPECT_EQ(plan.work->decomposition, 64U) << "the default panel depth the workload resolved";
  EXPECT_GT(plan.work->logical_bytes, 0U);
  EXPECT_NE(plan.description.find("dense_linear: 1 thread(s) on cpu 2, footprint 8MiB"),
            std::string::npos)
      << plan.description;
  EXPECT_NE(plan.description.find("avx2 path"), std::string::npos) << plan.description;
}

TEST(PlanWorker, WhereAWorkerRunsOnSeveralCpusOrNoneNoSingleCoreIsClaimed) {
  const MachineTree tree(MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  Planning planning;
  auto all = plan_worker(entry("compression", config::Threads::all()), 0, std::nullopt,
                         planning.options, &machine);
  ASSERT_TRUE(all.has_value()) << all.error();
  EXPECT_EQ(all.value().threads, 4U);
  EXPECT_EQ(all.value().cpus, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  EXPECT_FALSE(all.value().args->physical_core.has_value());
  EXPECT_FALSE(all.value().work->cpu.has_value());
  EXPECT_NE(all.value().description.find("on cpus 0,1,2,3"), std::string::npos);

  auto two = plan_worker(entry("compression", config::Threads::count(2)), 1, std::nullopt,
                         planning.options, &machine);
  ASSERT_TRUE(two.has_value());
  EXPECT_EQ(two.value().cpus, (std::vector<std::uint32_t>{2, 3}));

  planning.options.cpus.clear();
  auto none = plan_worker(entry("compression", config::Threads::all()), 0, std::nullopt,
                          planning.options, &machine);
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none.value().threads, 1U);
  EXPECT_TRUE(none.value().cpus.empty());
  EXPECT_NE(none.value().description.find("on unpinned"), std::string::npos);
}

TEST(PlanWorker, ASingleCpuWithNoNumaNodeOrCoreInTheSnapshotIsLeftUnsaid) {
  const MachineTree tree(MachineShape{2, 64, false});
  const topology::Snapshot machine = tree.accepted();
  Planning planning;
  auto planned = plan_worker(entry("compression"), 0, std::nullopt, planning.options, &machine);
  ASSERT_TRUE(planned.has_value());
  EXPECT_EQ(planned.value().args->physical_core, 0U);
  EXPECT_FALSE(planned.value().args->numa_node.has_value()) << "the snapshot has no NUMA layout";
  planning.options.cpus = {9};  // A CPU the snapshot does not describe.
  auto elsewhere = plan_worker(entry("compression"), 0, std::nullopt, planning.options, &machine);
  ASSERT_TRUE(elsewhere.has_value());
  EXPECT_FALSE(elsewhere.value().args->physical_core.has_value());
  auto no_snapshot = plan_worker(entry("compression"), 0, std::nullopt, planning.options, nullptr);
  ASSERT_TRUE(no_snapshot.has_value());
  EXPECT_FALSE(no_snapshot.value().args->physical_core.has_value());
}

TEST(PlanWorker, AConfiguredFootprintIsResolvedAgainstTheBudget) {
  const Planning planning;
  const config::MemoryBudget budget{std::uint64_t{64} << 20U, std::nullopt};
  auto absolute = plan_worker(entry("compression", config::Threads::count(1),
                                    core::ByteSize::bytes(std::uint64_t{4} << 20U)),
                              0, budget, planning.options, nullptr);
  ASSERT_TRUE(absolute.has_value()) << absolute.error();
  EXPECT_EQ(absolute.value().args->footprint_bytes, std::uint64_t{4} << 20U);
  auto share = plan_worker(
      entry("compression", config::Threads::count(1), core::ByteSize::percent(50).value()), 0,
      budget, planning.options, nullptr);
  ASSERT_TRUE(share.has_value()) << share.error();
  EXPECT_EQ(share.value().args->footprint_bytes, std::uint64_t{32} << 20U);
  // No budget could be read: an absolute size stands, a share cannot be resolved
  // and the default footprint is used (the banner says the limit is not enforced).
  auto unread_absolute = plan_worker(entry("compression", config::Threads::count(1),
                                           core::ByteSize::bytes(std::uint64_t{2} << 20U)),
                                     0, std::nullopt, planning.options, nullptr);
  ASSERT_TRUE(unread_absolute.has_value());
  EXPECT_EQ(unread_absolute.value().args->footprint_bytes, std::uint64_t{2} << 20U);
  auto unread_share = plan_worker(
      entry("compression", config::Threads::count(1), core::ByteSize::percent(50).value()), 0,
      std::nullopt, planning.options, nullptr);
  ASSERT_TRUE(unread_share.has_value());
  EXPECT_EQ(unread_share.value().args->footprint_bytes, planning.options.footprint_bytes);
  auto too_big = plan_worker(entry("compression", config::Threads::count(1),
                                   core::ByteSize::bytes(std::uint64_t{1} << 40U)),
                             0, budget, planning.options, nullptr);
  ASSERT_FALSE(too_big.has_value());
  EXPECT_NE(too_big.error().find("workload.compression"), std::string::npos) << too_big.error();
}

TEST(PlanWorker, EveryReasonARunCannotStartNamesTheWorkload) {
  {
    Planning planning;
    planning.options.line_bytes = 0;
    auto planned = plan_worker(entry("compression"), 0, std::nullopt, planning.options, nullptr);
    ASSERT_FALSE(planned.has_value());
    EXPECT_NE(planned.error().find("compression: the machine's cache-line size is not known"),
              std::string::npos)
        << planned.error();
    EXPECT_NE(planned.error().find("refusing to assume one"), std::string::npos);
  }
  {
    Planning planning;
    planning.options.footprint_bytes = 1;
    auto planned = plan_worker(entry("compression"), 0, std::nullopt, planning.options, nullptr);
    ASSERT_FALSE(planned.has_value());
    EXPECT_EQ(planned.error().substr(0, 46), "compression: footprint too small for one unit:");
  }
  {
    Planning planning;
    planning.options.requested_isa = IsaPath::kAvx512;
    auto planned = plan_worker(entry("compression"), 0, std::nullopt, planning.options, nullptr);
    ASSERT_FALSE(planned.has_value());
    EXPECT_EQ(planned.error(), "compression: the workload has no avx512 kernel");
  }
  {
    // The workload accepts the scalar probe and refuses the configuration made
    // for the chosen path.
    Planning planning;
    planning.registry.fail_from = 2;
    auto planned = plan_worker(entry("compression"), 0, std::nullopt, planning.options, nullptr);
    ASSERT_FALSE(planned.has_value());
    EXPECT_EQ(planned.error(),
              "compression: configuration outside the workload's domain: scripted");
  }
}

TEST(PlanWorker, WithoutAnIsaProbeOnlyScalarIsOffered) {
  Planning planning;
  planning.options.isa_probe = nullptr;
  auto planned = plan_worker(entry("dense_linear"), 0, std::nullopt, planning.options, nullptr);
  ASSERT_TRUE(planned.has_value()) << planned.error();
  EXPECT_EQ(planned.value().args->isa, IsaPath::kScalar);
}

TEST(VerificationOptionsTest, EqualityLooksAtEveryField) {
  const VerificationOptions a;
  EXPECT_EQ(a, a);
  const workloads::BuiltinRegistry registry;
  const auto changed = [&a](auto change) {
    VerificationOptions other = a;
    change(other);
    return a != other;
  };
  EXPECT_TRUE(changed([&registry](VerificationOptions& o) { o.registry = &registry; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.seed = 9; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.footprint_bytes = 1; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.decomposition = 1; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.requested_isa = IsaPath::kAvx2; }));
  const controller_testing::ModelProbe probe{IsaSet{IsaPath::kScalar}};
  EXPECT_TRUE(changed([&probe](VerificationOptions& o) { o.isa_probe = &probe; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.duty.oracle_period = 4; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.duty.integrity_period = 4; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.line_bytes = 64; }));
  EXPECT_TRUE(changed([](VerificationOptions& o) { o.cpus = {1}; }));
}

}  // namespace
}  // namespace loadforge::controller
