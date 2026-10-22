// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_VERIFICATION_RUNNER_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_VERIFICATION_RUNNER_CONTRACT_HPP

// Provider-side suite for src/verification/runner.hpp (ADR 0003, 0004).
//
// SliceRunnerContract<Subject>:
//   Subject::make(const RunnerSetup&) -> core::Result<std::unique_ptr<
//                                         SliceRunner>, RunnerError>
//
// The suite brings its own workload and oracle, scripted to misbehave on
// chosen units in exactly the ways the bracket exists to tell apart: an
// input already wrong when it is read, an input that goes wrong while it is
// read, an output that is wrong, and an oracle that cannot check. The
// workload's tricks (writing through its const input, generating differently
// the second time) are what a test double may do and a workload never may.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "platform/syscalls.hpp"
#include "primitives/rng/philox.hpp"
#include "verification/oracle.hpp"
#include "verification/runner.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::contracts {

namespace runner_suite {

inline constexpr std::size_t kBytes = 64;
inline constexpr std::byte kKey{0x5A};

struct Script {
  std::set<std::uint64_t> wrong_before;  ///< regenerates differently: step 2 fails
  std::set<std::uint64_t> wrong_during;  ///< input flipped inside compute: step 5 fails
  std::set<std::uint64_t> wrong_output;  ///< output flipped after compute: step 4 fails
  std::set<std::uint64_t> unchecked;     ///< the oracle answers NotChecked
};

class ScriptedWorkload final : public workloads::Workload {
 public:
  explicit ScriptedWorkload(Script script)
      : script_(std::move(script)),
        descriptor_{"scripted", 1, workloads::DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_{kBytes, kBytes, 0, 4, 1, 2 * kBytes, kBytes} {}

  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(99, index, input);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (script_.wrong_before.contains(index) && generated_[index]++ > 0) {
      input[5] ^= std::byte{0x01};
    }
  }

  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> /*scratch*/,
               worker::Executor& /*executor*/) const noexcept override {
    if (script_.wrong_during.contains(index)) {
      // The memory under a const view changing: the event, simulated.
      const_cast<std::byte&>(input[10]) ^= std::byte{0x08};
    }
    for (std::size_t at = 0; at < kBytes; ++at) {
      output[at] = input[at] ^ kKey;
    }
    if (script_.wrong_output.contains(index)) {
      output[20] ^= std::byte{0x01};
    }
  }

 private:
  Script script_;
  workloads::Descriptor descriptor_;
  workloads::Plan plan_;
  mutable std::mutex mutex_;
  mutable std::map<std::uint64_t, int> generated_;
};

class ScriptedOracle final : public verification::Oracle {
 public:
  explicit ScriptedOracle(Script script, std::string name = "scripted")
      : script_(std::move(script)), name_(std::move(name)) {}

  [[nodiscard]] std::string_view workload() const noexcept override { return name_; }
  [[nodiscard]] verification::Contract contract() const noexcept override {
    return verification::Contract::kExact;
  }
  [[nodiscard]] verification::Verdict check(
      const verification::UnitEvidence& evidence) const override {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      checked_.push_back(evidence.index);
    }
    if (script_.unchecked.contains(evidence.index)) {
      return verification::NotChecked{"scripted unit", "the script says so", 0};
    }
    for (std::size_t at = 0; at < kBytes; ++at) {
      if ((evidence.input[at] ^ kKey) != evidence.output[at]) {
        verification::Difference difference{
            verification::Domain::kStored, at, at, 1, at / 8 * 8, {}, {}};
        for (std::size_t w = difference.window_offset; w < kBytes; ++w) {
          difference.expected.push_back(evidence.input[w] ^ kKey);
          difference.actual.push_back(evidence.output[w]);
        }
        return verification::ExactMismatch{"output against input XOR key", difference, 1};
      }
    }
    return verification::Agreement{1};
  }

  [[nodiscard]] std::vector<std::uint64_t> checked() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return checked_;
  }

 private:
  Script script_;
  std::string name_;
  mutable std::mutex mutex_;
  mutable std::vector<std::uint64_t> checked_;
};

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// A workload, its oracle, an executor and the real seam, wired into a setup.
struct Rig {
  explicit Rig(const Script& script, verification::DutyCycle duty = {1, 1})
      : workload(script), oracle(script) {
    setup.workload = &workload;
    setup.oracle = &oracle;
    setup.executor = &executor;
    setup.syscalls = &syscalls;
    setup.duty = duty;
    setup.line_bytes = 128;
    setup.base.logical_cpu = 3;
    setup.base.workload = "scripted";
    setup.base.workload_version = 1;
    setup.base.seed = 99;
    setup.base.isa = simd::IsaPath::kScalar;
  }
  ScriptedWorkload workload;
  ScriptedOracle oracle;
  SerialExecutor executor;
  platform::RealSyscalls syscalls;
  verification::RunnerSetup setup;
};

inline std::vector<std::uint64_t> iterations(const verification::SliceReport& report) {
  std::vector<std::uint64_t> out;
  out.reserve(report.records.size());
  for (const verification::ErrorRecord& record : report.records) {
    out.push_back(verification::provenance_of(record).iteration.value_or(~std::uint64_t{0}));
  }
  return out;
}

}  // namespace runner_suite

template <typename Subject>
class SliceRunnerContract : public ::testing::Test {
 protected:
  static std::unique_ptr<verification::SliceRunner> make(runner_suite::Rig& rig) {
    auto made = Subject::make(rig.setup);
    EXPECT_TRUE(made.has_value()) << made.error().detail;
    return made.has_value() ? std::move(made).value() : nullptr;
  }
};
TYPED_TEST_SUITE_P(SliceRunnerContract);

using runner_suite::Rig;
using runner_suite::Script;
using verification::ExactBitCorruption;
using verification::InputIntegrity;
using verification::Side;

TYPED_TEST_P(SliceRunnerContract, ASliceRunsExactlyTheUnitsAskedForAndAdvances) {
  Rig rig(Script{});
  rig.setup.first_index = 10;
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto first = runner->run_slice({5});
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first.value().tally.units_computed, 5U);
  EXPECT_EQ(first.value().next_index, 15U);
  EXPECT_TRUE(first.value().records.empty()) << "a clean slice makes no record";
  const auto empty = runner->run_slice({0});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty.value().tally, verification::Tally{}) << "an empty slice does nothing";
  EXPECT_TRUE(empty.value().records.empty()) << "and is judged over the run, not alone";
  EXPECT_EQ(empty.value().next_index, 15U);
  EXPECT_EQ(rig.oracle.checked(), (std::vector<std::uint64_t>{10, 11, 12, 13, 14}));
}

TYPED_TEST_P(SliceRunnerContract, TheDutyCycleChoosesUnitsByIndex) {
  Rig rig(Script{}, verification::DutyCycle{2, 3});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto report = runner->run_slice({12});
  ASSERT_TRUE(report.has_value());
  const verification::Tally& tally = report.value().tally;
  EXPECT_EQ(tally.units_computed, 12U);
  EXPECT_EQ(tally.oracle_checked, 6U) << "0, 2, 4, 6, 8, 10";
  EXPECT_EQ(tally.oracle_blocks, 6U);
  EXPECT_EQ(tally.integrity_checked, 4U) << "0, 3, 6, 9";
  EXPECT_EQ(tally.oracle_failed + tally.oracle_not_performed + tally.integrity_failed, 0U);
  EXPECT_EQ(rig.oracle.checked(), (std::vector<std::uint64_t>{0, 2, 4, 6, 8, 10}));
  EXPECT_EQ(runner->total(), tally);
}

TYPED_TEST_P(SliceRunnerContract, AWrongOutputIsAnOutputRecordWithFullProvenance) {
  Rig rig(Script{{}, {}, {4}, {}});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto report = runner->run_slice({6});
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report.value().records.size(), 1U);
  const auto* record = std::get_if<ExactBitCorruption>(&report.value().records[0]);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->side, Side::kOutput);
  EXPECT_EQ(record->line_bytes, 128U) << "the setup's line size, never an assumed 64";
  ASSERT_TRUE(record->difference.has_value());
  EXPECT_EQ(record->difference->first_offset, 20U);
  EXPECT_EQ(record->difference->popcount, 1U);
  const verification::Provenance& provenance = record->provenance;
  EXPECT_EQ(provenance.iteration, 4U);
  EXPECT_EQ(provenance.logical_cpu, 3U);
  EXPECT_EQ(provenance.workload, "scripted");
  EXPECT_EQ(provenance.workload_version, 1U);
  EXPECT_EQ(provenance.seed, 99U);
  EXPECT_EQ(provenance.isa, simd::IsaPath::kScalar);
  EXPECT_EQ(provenance.input_integrity, InputIntegrity::kIntact);
  EXPECT_EQ(provenance.duty, (verification::DutyCycle{1, 1}));
  ASSERT_TRUE(provenance.buffer_address.has_value());
  EXPECT_EQ(*provenance.buffer_address % 128, 0U) << "every slot is line-aligned";
  EXPECT_TRUE(provenance.buffer_offset.has_value());
  EXPECT_TRUE(verification::is_valid(report.value().records[0]));
  EXPECT_EQ(report.value().tally.oracle_failed, 1U);
}

TYPED_TEST_P(SliceRunnerContract, AnInputThatWentWrongWhileReadIsCaughtByTheClosingCheck) {
  Rig rig(Script{{}, {6}, {}, {}});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto report = runner->run_slice({8});
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report.value().records.size(), 1U)
      << "the output matches the input it was computed from; only the bracket sees this";
  const auto* record = std::get_if<ExactBitCorruption>(&report.value().records[0]);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->side, Side::kInput);
  EXPECT_EQ(record->provenance.input_integrity, InputIntegrity::kCorruptDuringConsumption);
  EXPECT_EQ(record->provenance.iteration, 6U);
  ASSERT_TRUE(record->difference.has_value());
  EXPECT_EQ(record->difference->domain, verification::Domain::kStored);
  EXPECT_EQ(record->difference->first_offset, 10U);
  EXPECT_EQ(record->difference->last_offset, 10U);
  EXPECT_EQ(record->difference->popcount, 1U);
  EXPECT_EQ(verification::classify(*record), verification::BitClass::kSingleBit);
  EXPECT_TRUE(verification::is_valid(report.value().records[0]));
  EXPECT_EQ(report.value().tally.integrity_failed, 1U);
}

TYPED_TEST_P(SliceRunnerContract, AnInputAlreadyWrongStopsTheUnitBeforeItIsComputed) {
  Rig rig(Script{{7}, {}, {}, {}});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto report = runner->run_slice({9});
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report.value().records.size(), 1U);
  const auto* record = std::get_if<ExactBitCorruption>(&report.value().records[0]);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->side, Side::kInput);
  EXPECT_EQ(record->provenance.input_integrity, InputIntegrity::kCorruptBeforeConsumption);
  EXPECT_EQ(record->provenance.iteration, 7U);
  EXPECT_EQ(report.value().tally.units_computed, 8U) << "unit 7 was not computed";
  EXPECT_EQ(report.value().tally.integrity_failed, 1U);
  for (const std::uint64_t checked : rig.oracle.checked()) {
    EXPECT_NE(checked, 7U) << "an input known wrong is not given to the oracle";
  }
}

TYPED_TEST_P(SliceRunnerContract, AnUncheckedUnitIsNotPerformedNeverAPassOrAFault) {
  Rig rig(Script{{}, {}, {}, {2}});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto report = runner->run_slice({4});
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report.value().records.size(), 1U);
  const auto* record =
      std::get_if<verification::VerificationNotPerformed>(&report.value().records[0]);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->provenance.iteration, 2U);
  EXPECT_FALSE(verification::is_wrong_result(report.value().records[0]));
  EXPECT_EQ(report.value().tally.oracle_not_performed, 1U);
  EXPECT_EQ(report.value().tally.oracle_failed, 0U);
  EXPECT_EQ(report.value().tally.oracle_blocks, 3U) << "the unchecked unit compared nothing";
}

TYPED_TEST_P(SliceRunnerContract, RecordsAreInUnitOrderAndEachUnitsInStepOrder) {
  Rig rig(Script{{1}, {3}, {3, 0}, {}});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto report = runner->run_slice({4});
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(runner_suite::iterations(report.value()), (std::vector<std::uint64_t>{0, 1, 3, 3}));
  ASSERT_EQ(report.value().records.size(), 4U);
  const auto* check = std::get_if<ExactBitCorruption>(&report.value().records[2]);
  const auto* close = std::get_if<ExactBitCorruption>(&report.value().records[3]);
  ASSERT_NE(check, nullptr);
  ASSERT_NE(close, nullptr);
  EXPECT_EQ(check->side, Side::kOutput) << "step 4 before step 5";
  EXPECT_EQ(check->provenance.input_integrity, InputIntegrity::kCorruptDuringConsumption)
      << "an output record says what the bracket found for its unit";
  EXPECT_EQ(close->side, Side::kInput);
}

TYPED_TEST_P(SliceRunnerContract, ARunThatCheckedNothingSaysSoWhenItFinishes) {
  Rig rig(Script{}, verification::DutyCycle{0, 0});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  ASSERT_TRUE(runner->run_slice({5}).has_value());
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 2U);
  for (const verification::ErrorRecord& record : finished.value().records) {
    const auto* unrun = std::get_if<verification::VerificationNotPerformed>(&record);
    ASSERT_NE(unrun, nullptr);
    EXPECT_EQ(unrun->blocks_compared, 0U);
    EXPECT_FALSE(unrun->what.empty());
    EXPECT_FALSE(unrun->why.empty());
  }
  EXPECT_EQ(finished.value().tally.units_computed, 0U);
}

TYPED_TEST_P(SliceRunnerContract, ARunThatComputedNothingAlsoCheckedNothing) {
  Rig rig(Script{});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  EXPECT_EQ(finished.value().records.size(), 2U) << "zero compared is an error, not a sweep";
}

TYPED_TEST_P(SliceRunnerContract, ARunThatCheckedFinishesClean) {
  Rig rig(Script{});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  ASSERT_TRUE(runner->run_slice({3}).has_value());
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  EXPECT_TRUE(finished.value().records.empty());
  EXPECT_EQ(runner->total().units_computed, 3U);
}

TYPED_TEST_P(SliceRunnerContract, NothingRunsAfterFinish) {
  Rig rig(Script{});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  ASSERT_TRUE(runner->finish().has_value());
  const auto slice = runner->run_slice({1});
  ASSERT_FALSE(slice.has_value());
  EXPECT_EQ(slice.error().fault, verification::RunnerFault::kFinished);
  const auto again = runner->finish();
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().fault, verification::RunnerFault::kFinished);
}

TYPED_TEST_P(SliceRunnerContract, AnOracleForAnotherWorkloadIsRefused) {
  Rig rig(Script{});
  runner_suite::ScriptedOracle stranger(Script{}, "other_workload");
  rig.setup.oracle = &stranger;
  const auto made = TypeParam::make(rig.setup);
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error().fault, verification::RunnerFault::kConfiguration);
  rig.setup.oracle = nullptr;
  EXPECT_EQ(TypeParam::make(rig.setup).error().fault, verification::RunnerFault::kConfiguration);
  rig.setup.oracle = &rig.oracle;
  rig.setup.line_bytes = 48;
  EXPECT_EQ(TypeParam::make(rig.setup).error().fault, verification::RunnerFault::kConfiguration);
}

// Contract change of the M2 corrections (2026-10-21): the lock policy.
TYPED_TEST_P(SliceRunnerContract, TheDefaultPolicyLocksAndSaysSo) {
  Rig rig(Script{});
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  EXPECT_EQ(runner->memory_lock().policy, verification::LockPolicy::kRequire);
  EXPECT_EQ(runner->memory_lock().outcome, verification::LockOutcome::kLocked);
  EXPECT_TRUE(runner->memory_lock().detail.empty());
}

TYPED_TEST_P(SliceRunnerContract, NoLockIsNotLockedAndStillRunsEveryUnit) {
  Rig rig(Script{});
  rig.setup.lock = verification::LockPolicy::kNone;
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr);
  EXPECT_EQ(runner->memory_lock().policy, verification::LockPolicy::kNone);
  EXPECT_EQ(runner->memory_lock().outcome, verification::LockOutcome::kNotLocked);
  const auto report = runner->run_slice({4});
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report.value().tally.units_computed, 4U);
  EXPECT_EQ(report.value().tally.oracle_checked, 4U);
}

TYPED_TEST_P(SliceRunnerContract, ATriedLockIsLockedOrFellBackWithTheReasonNeverRefused) {
  Rig rig(Script{});
  rig.setup.lock = verification::LockPolicy::kTry;
  const auto runner = this->make(rig);
  ASSERT_NE(runner, nullptr) << "kTry does not refuse for want of the lock";
  const verification::MemoryLock& lock = runner->memory_lock();
  EXPECT_EQ(lock.policy, verification::LockPolicy::kTry);
  if (lock.outcome == verification::LockOutcome::kFellBack) {
    EXPECT_FALSE(lock.detail.empty());
  } else {
    EXPECT_EQ(lock.outcome, verification::LockOutcome::kLocked);
    EXPECT_TRUE(lock.detail.empty());
  }
}

REGISTER_TYPED_TEST_SUITE_P(
    SliceRunnerContract, TheDefaultPolicyLocksAndSaysSo, NoLockIsNotLockedAndStillRunsEveryUnit,
    ATriedLockIsLockedOrFellBackWithTheReasonNeverRefused,
    ASliceRunsExactlyTheUnitsAskedForAndAdvances, TheDutyCycleChoosesUnitsByIndex,
    AWrongOutputIsAnOutputRecordWithFullProvenance,
    AnInputThatWentWrongWhileReadIsCaughtByTheClosingCheck,
    AnInputAlreadyWrongStopsTheUnitBeforeItIsComputed,
    AnUncheckedUnitIsNotPerformedNeverAPassOrAFault, RecordsAreInUnitOrderAndEachUnitsInStepOrder,
    ARunThatCheckedNothingSaysSoWhenItFinishes, ARunThatComputedNothingAlsoCheckedNothing,
    ARunThatCheckedFinishesClean, NothingRunsAfterFinish, AnOracleForAnotherWorkloadIsRefused);

}  // namespace loadforge::contracts

#endif
