// SPDX-License-Identifier: GPL-3.0-or-later
//
// The runner's setup, its working set, and the ordinary path of a run (P1,
// P2, P3). Its findings are the fault-injection tier's: bracket_test,
// unrun_test and control_state_fault_test under tests/fault_injection/.

#include "verification/runner.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/crc32.hpp"
#include "platform/syscalls.hpp"
#include "unit/verification/integrity/integrity_support.hpp"
#include "verification/integrity/control_state.hpp"

namespace loadforge::integrity_testing {
namespace {

using verification::RunnerFault;
using Call = SteppedSyscalls::Call;

// --- configuration -----------------------------------------------------------

TEST(RunnerSetupTest, EachMissingPartIsRefusedByName) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  const verification::RunnerSetup good = setup_for(*workload, oracle, executor, syscalls);

  verification::RunnerSetup setup = good;
  setup.workload = nullptr;
  EXPECT_EQ(verification::make_slice_runner(setup).error(),
            (verification::RunnerError{RunnerFault::kConfiguration, "no workload"}));
  setup = good;
  setup.oracle = nullptr;
  EXPECT_EQ(verification::make_slice_runner(setup).error(),
            (verification::RunnerError{RunnerFault::kConfiguration, "no oracle"}));
  setup = good;
  setup.executor = nullptr;
  EXPECT_EQ(verification::make_slice_runner(setup).error(),
            (verification::RunnerError{RunnerFault::kConfiguration, "no executor"}));
  setup = good;
  setup.syscalls = nullptr;
  EXPECT_EQ(verification::make_slice_runner(setup).error(),
            (verification::RunnerError{RunnerFault::kConfiguration, "no system-call seam"}));
  EXPECT_EQ(syscalls.fake.live_mappings(), 0U) << "nothing is mapped for a refused setup";
}

TEST(RunnerSetupTest, AnOracleForAnotherWorkloadOrAnotherContractIsRefused) {
  const auto workload = make_rotor();
  SerialExecutor executor;
  SteppedSyscalls syscalls;

  const NamedOracle stranger("mean", verification::Contract::kExact);
  auto made = verification::make_slice_runner(setup_for(*workload, stranger, executor, syscalls));
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error().fault, RunnerFault::kConfiguration);
  EXPECT_EQ(made.error().detail, "the oracle checks 'mean' and the workload is 'rotor'");

  const NamedOracle bounded("rotor", verification::Contract::kBounded);
  made = verification::make_slice_runner(setup_for(*workload, bounded, executor, syscalls));
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error().fault, RunnerFault::kConfiguration);
  EXPECT_EQ(made.error().detail,
            "the workload is checked by the exact contract and the oracle offers the bounded one");
  EXPECT_EQ(syscalls.fake.live_mappings(), 0U);
}

TEST(RunnerSetupTest, ALineSizeOrAPlanTheLayoutRefusesIsRefusedBeforeAnyMapping) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, oracle, executor, syscalls);
  setup.line_bytes = 96;
  EXPECT_EQ(verification::make_slice_runner(setup).error().fault, RunnerFault::kConfiguration);

  const ShapedWorkload empty("rotor", workloads::Plan{8, 8, 0, 0, 1, 0, 0});
  EXPECT_EQ(
      verification::make_slice_runner(setup_for(empty, oracle, executor, syscalls)).error().fault,
      RunnerFault::kConfiguration);

  const ShapedWorkload enormous("rotor", workloads::Plan{1, 1, 0, ~std::uint64_t{0} / 8, 1, 0, 0});
  const auto made =
      verification::make_slice_runner(setup_for(enormous, oracle, executor, syscalls));
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error().fault, RunnerFault::kResources);
  EXPECT_TRUE(syscalls.fake.map_flags().empty()) << "no mapping was asked for";
}

// --- resources: every step of every mapping, refused --------------------------

TEST(RunnerResourcesTest, EveryStepOfEveryMappingRefusedIsResourcesNamingIt) {
  const struct {
    Call call;
    const char* words;
    std::vector<int> errors;
  } calls[] = {
      {Call::kMap, "mapping failed", {EINVAL, ENOMEM, EAGAIN, EPERM}},
      {Call::kBind, "binding to the NUMA node failed", {EFAULT, EINVAL, EIO, ENOMEM, EPERM}},
      {Call::kPopulate,
       "populating failed",
       {EINVAL, ENOMEM, EFAULT, EPERM, EINTR, EAGAIN, EHWPOISON}},
      {Call::kLock, "locking failed", {ENOMEM, EPERM, EAGAIN, EINVAL}},
  };
  // The working set is the first mapping, the control state's copies the
  // next three.
  const char* const owners[] = {"working set: ", "control state, copy 0: ",
                                "control state, copy 1: ", "control state, copy 2: "};
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  for (const auto& call : calls) {
    for (const int error : call.errors) {
      for (int nth = 1; nth <= 4; ++nth) {
        SCOPED_TRACE(std::string(call.words) + ", errno " + std::to_string(error) + ", mapping " +
                     std::to_string(nth));
        SteppedSyscalls syscalls;
        syscalls.fail(call.call, nth, error);
        verification::RunnerSetup setup = setup_for(*workload, oracle, executor, syscalls);
        setup.base.numa_node = 0;
        const auto made = verification::make_slice_runner(setup);
        ASSERT_FALSE(made.has_value());
        EXPECT_EQ(made.error().fault, RunnerFault::kResources);
        const std::string& detail = made.error().detail;
        EXPECT_EQ(detail.rfind(owners[nth - 1], 0), 0U) << detail;
        EXPECT_NE(detail.find(call.words), std::string::npos) << detail;
        EXPECT_NE(detail.find("injected"), std::string::npos)
            << "the kernel's words are carried: " << detail;
        EXPECT_EQ(syscalls.fake.live_mappings(), 0U) << "what was mapped is unmapped";
        EXPECT_EQ(syscalls.fake.locked_bytes(), 0U);
      }
    }
  }
}

// --- the working set ------------------------------------------------------------

TEST(RunnerWorkingSetTest, TheWorkingSetIsPopulatedLockedAndBoundToTheWorkersNode) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, oracle, executor, syscalls);
  setup.base.numa_node = 0;
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  ASSERT_EQ(syscalls.mapped().size(), 4U) << "the working set and three control copies";
  // 8 units: 8 x 256 in, 8 x 256 out, 256 regenerated, 24 bytes of scratch
  // for the one thread rounded to a line, and a line of slack.
  EXPECT_EQ(syscalls.mapped()[0].bytes, 8U * 256 + 8U * 256 + 256 + 64 + 64);
  ASSERT_EQ(syscalls.fake.binds().size(), 4U) << "every mapping bound to node 0";
  EXPECT_EQ(std::count(syscalls.fake.advice_given().begin(), syscalls.fake.advice_given().end(),
                       MADV_POPULATE_WRITE),
            4);
  EXPECT_EQ(syscalls.fake.locked_bytes(),
            syscalls.mapped()[0].bytes + 3 * verification::integrity::kCopyBytes);
}

TEST(RunnerWorkingSetTest, WithoutANodeNothingIsBound) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  const auto runner = make_runner(setup_for(*workload, oracle, executor, syscalls));
  ASSERT_NE(runner, nullptr);
  EXPECT_TRUE(syscalls.fake.binds().empty());
}

TEST(RunnerWorkingSetTest, TheRunnerUnmapsEverythingWhenItGoes) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  {
    const auto runner = make_runner(setup_for(*workload, oracle, executor, syscalls));
    ASSERT_NE(runner, nullptr);
    EXPECT_EQ(syscalls.fake.live_mappings(), 4U);
  }
  EXPECT_EQ(syscalls.fake.live_mappings(), 0U);
}

/// Every slot of every unit, at a line size, through the real kernel: the
/// address each record names is a multiple of the line size.
void expect_slots_aligned(std::uint32_t line_bytes) {
  SCOPED_TRACE("line " + std::to_string(line_bytes));
  const auto workload = make_rotor();
  RotorOracle oracle;
  ScriptedOracle wrong_everywhere(oracle);
  for (std::uint64_t index = 0; index < 8; ++index) {
    wrong_everywhere.answers[index] = [](const verification::UnitEvidence&) {
      return verification::Verdict{verification::NotChecked{"slot", "to see its address", 0}};
    };
  }
  SerialExecutor executor;
  platform::RealSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, wrong_everywhere, executor, syscalls);
  setup.line_bytes = line_bytes;
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 8);
  ASSERT_EQ(report.records.size(), 8U);
  std::vector<std::uint64_t> offsets;
  for (const verification::ErrorRecord& record : report.records) {
    const verification::Provenance& provenance = verification::provenance_of(record);
    ASSERT_TRUE(provenance.buffer_address.has_value());
    EXPECT_EQ(*provenance.buffer_address % line_bytes, 0U);
    offsets.push_back(provenance.buffer_offset.value());
  }
  for (std::size_t unit = 1; unit < offsets.size(); ++unit) {
    EXPECT_EQ(offsets[unit] - offsets[unit - 1], (200 + line_bytes - 1) / line_bytes * line_bytes)
        << "consecutive output slots, one stride apart";
  }
}

TEST(RunnerWorkingSetTest, EverySlotIsLineAlignedAtEveryLineSize) {
  for (const std::uint32_t line : {8U, 64U, 128U, 4096U, 16384U}) {
    expect_slots_aligned(line);
  }
}

TEST(RunnerWorkingSetTest, SlotsAreLineAlignedInMemoryThatIsNotPageAligned) {
  // The fake's memory is the heap's, aligned to 16 at best: the runner must
  // find the line inside its slack rather than assume the mapping is on one.
  const auto workload = make_rotor();
  RotorOracle oracle;
  ScriptedOracle unchecked(oracle);
  unchecked.answers[3] = [](const verification::UnitEvidence&) {
    return verification::Verdict{verification::NotChecked{"slot", "to see its address", 0}};
  };
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, unchecked, executor, syscalls);
  setup.line_bytes = 256;
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 4);
  ASSERT_EQ(report.records.size(), 1U);
  const verification::Provenance& provenance = verification::provenance_of(report.records[0]);
  EXPECT_EQ(*provenance.buffer_address % 256, 0U);
  const auto start = reinterpret_cast<std::uintptr_t>(syscalls.mapped()[0].address);
  EXPECT_EQ(*provenance.buffer_address, start + *provenance.buffer_offset)
      << "the offset is within the mapping that holds the bytes";
}

// --- the ordinary path ----------------------------------------------------------

TEST(RunnerRunTest, SlicesAddUpAndTheTotalIsTheirSum) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  const auto runner = make_runner(setup_for(*workload, oracle, executor, syscalls, {2, 3}));
  ASSERT_NE(runner, nullptr);
  verification::Tally sum;
  std::uint64_t next = 0;
  for (const std::uint64_t units : {0U, 1U, 5U, 13U}) {
    const verification::SliceReport report = slice(*runner, units);
    EXPECT_TRUE(report.records.empty());
    EXPECT_EQ(report.tally.units_computed, units);
    next += units;
    EXPECT_EQ(report.next_index, next);
    sum = sum + report.tally;
    EXPECT_EQ(runner->total(), sum);
  }
  EXPECT_EQ(sum.oracle_checked, 10U) << "even indices of 0..18";
  EXPECT_EQ(sum.integrity_checked, 7U) << "multiples of three in 0..18";
  EXPECT_EQ(sum.oracle_blocks, 10U);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  EXPECT_TRUE(finished.value().records.empty());
  EXPECT_EQ(finished.value().next_index, 19U);
  EXPECT_EQ(finished.value().tally, verification::Tally{});
  EXPECT_EQ(runner->total(), sum) << "finish adds nothing";
}

TEST(RunnerRunTest, MoreUnitsThanSlotsReuseTheSlotsInTurn) {
  const auto workload = make_rotor();
  RotorOracle oracle;
  ScriptedOracle unchecked(oracle);
  for (const std::uint64_t index : {1U, 9U, 17U}) {
    unchecked.answers[index] = [](const verification::UnitEvidence&) {
      return verification::Verdict{verification::NotChecked{"slot", "to see its address", 0}};
    };
  }
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  const auto runner = make_runner(setup_for(*workload, unchecked, executor, syscalls));
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 20);
  ASSERT_EQ(report.records.size(), 3U);
  const auto offset = [&](std::size_t which) {
    return verification::provenance_of(report.records[which]).buffer_offset.value();
  };
  EXPECT_EQ(offset(0), offset(1)) << "units 1 and 9 share slot 1 of 8";
  EXPECT_EQ(offset(1), offset(2));
  EXPECT_EQ(report.tally.units_computed, 20U);
}

TEST(RunnerRunTest, TheIndexWrapsAtTheTopOfItsRange) {
  const auto workload = make_rotor();
  RotorOracle oracle;
  ScriptedOracle observed(oracle);
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, observed, executor, syscalls);
  setup.first_index = ~std::uint64_t{0} - 1;
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 3);
  EXPECT_TRUE(report.records.empty()) << "the last two units and unit 0 are all right";
  EXPECT_EQ(report.next_index, 1U);
  EXPECT_EQ(observed.checked,
            (std::vector<std::uint64_t>{~std::uint64_t{0} - 1, ~std::uint64_t{0}, 0}));
}

TEST(RunnerRunTest, AThreadedExecutorRunsTheSameUnitsWithScratchForEveryThread) {
  for (int round = 0; round < 3; ++round) {
    const auto workload = make_rotor();
    const RotorOracle oracle;
    ThreadedExecutor executor(4);
    platform::RealSyscalls syscalls;
    const auto runner = make_runner(setup_for(*workload, oracle, executor, syscalls));
    ASSERT_NE(runner, nullptr);
    const verification::SliceReport report = slice(*runner, 24);
    EXPECT_TRUE(report.records.empty()) << "round " << round;
    EXPECT_EQ(report.tally.oracle_checked, 24U);
    EXPECT_EQ(report.tally.integrity_checked, 24U);
  }
}

TEST(RunnerRunTest, ARecordNamesTheSetupsProvenanceAndLeavesTheSeedAbsentWhenItWas) {
  const auto workload = make_rotor();
  RotorOracle oracle;
  ScriptedOracle unchecked(oracle);
  unchecked.answers[0] = [](const verification::UnitEvidence&) {
    return verification::Verdict{verification::NotChecked{"unit", "scripted", 0}};
  };
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, unchecked, executor, syscalls, {1, 4});
  setup.base.seed.reset();
  setup.base.numa_node = 0;
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 1);
  ASSERT_EQ(report.records.size(), 1U);
  const verification::Provenance& provenance = verification::provenance_of(report.records[0]);
  EXPECT_FALSE(provenance.seed.has_value()) << "absence reported as absence, never as zero";
  EXPECT_EQ(provenance.logical_cpu, 2U);
  EXPECT_EQ(provenance.physical_core, 1U);
  EXPECT_EQ(provenance.numa_node, 0U);
  EXPECT_EQ(provenance.workload, "rotor");
  EXPECT_EQ(provenance.workload_version, 3U);
  EXPECT_EQ(provenance.isa, simd::IsaPath::kScalar);
  EXPECT_EQ(provenance.duty, (verification::DutyCycle{1, 4}));
  EXPECT_EQ(provenance.iteration, 0U);
  EXPECT_EQ(provenance.input_integrity, verification::InputIntegrity::kIntact);
}

TEST(RunnerRunTest, ASetupThatNamesNoWorkloadGivesRecordsThatNameNone) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup = setup_for(*workload, oracle, executor, syscalls, {0, 0});
  setup.base.workload.reset();
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 2U);
  EXPECT_FALSE(verification::provenance_of(finished.value().records[0]).workload.has_value())
      << "the runner does not fill in what the worker left unknown";
}

TEST(RunnerRunTest, FinishThenNothing) {
  const auto workload = make_rotor();
  const RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  const auto runner = make_runner(setup_for(*workload, oracle, executor, syscalls));
  ASSERT_NE(runner, nullptr);
  slice(*runner, 2);
  ASSERT_TRUE(runner->finish().has_value());
  EXPECT_EQ(runner->run_slice({1}).error(),
            (verification::RunnerError{RunnerFault::kFinished, "run_slice() after finish()"}));
  EXPECT_EQ(runner->finish().error(),
            (verification::RunnerError{RunnerFault::kFinished, "finish() after finish()"}));
  EXPECT_EQ(runner->total().units_computed, 2U);
}

}  // namespace
}  // namespace loadforge::integrity_testing
