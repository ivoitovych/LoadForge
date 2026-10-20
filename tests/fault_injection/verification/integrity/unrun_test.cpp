// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5b for the slice runner (verification.md §5, §8, §10): every way a check
// can be prevented is made to happen, and each test asserts a
// VerificationNotPerformed -- never a pass, never a wrong result that fault
// isolation would take (is_wrong_result is false for every one).
//
//   * the oracle answers NotChecked;
//   * the oracle throws, a std::exception or anything else;
//   * the oracle answers a verdict its contract forbids;
//   * the duty cycle samples nothing, or brackets nothing;
//   * every unit the oracle would have checked was stopped by the bracket;
//   * a run that computed nothing at all.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "unit/verification/integrity/integrity_support.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"

namespace loadforge::integrity_testing {
namespace {

using verification::VerificationNotPerformed;

struct Rig {
  explicit Rig(verification::DutyCycle duty = {1, 1})
      : workload(make_rotor()), tamper(*workload), oracle(inner_oracle) {
    setup = setup_for(tamper, oracle, executor, syscalls, duty);
  }
  std::unique_ptr<workloads::Workload> workload;
  TamperingWorkload tamper;
  RotorOracle inner_oracle;
  ScriptedOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup;
};

const VerificationNotPerformed& unrun(const verification::ErrorRecord& record) {
  EXPECT_FALSE(verification::is_wrong_result(record)) << "never routed to fault isolation";
  EXPECT_TRUE(verification::is_valid(record));
  const auto* found = std::get_if<VerificationNotPerformed>(&record);
  EXPECT_NE(found, nullptr) << verification::kind_name(record);
  static const VerificationNotPerformed kNone{};
  return found != nullptr ? *found : kNone;
}

ScriptedOracle::Answer answer(const verification::Verdict& verdict) {
  return [verdict](const verification::UnitEvidence&) { return verdict; };
}

TEST(UnrunTest, NotCheckedIsAFailureOfTheRunWithTheOraclesWordsAndBlocks) {
  Rig rig;
  rig.oracle.answers[1] = answer(verification::NotChecked{"rotor unit", "sample was empty", 2});
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 3);
  ASSERT_EQ(report.records.size(), 1U);
  const VerificationNotPerformed& record = unrun(report.records[0]);
  EXPECT_EQ(record.what, "rotor unit");
  EXPECT_EQ(record.why, "sample was empty");
  EXPECT_EQ(record.blocks_compared, 2U);
  EXPECT_EQ(record.provenance.iteration, 1U);
  EXPECT_EQ(record.provenance.input_integrity, verification::InputIntegrity::kIntact);
  ASSERT_TRUE(record.provenance.buffer_address.has_value()) << "the output slot it was about";
  EXPECT_EQ(report.tally.oracle_not_performed, 1U);
  EXPECT_EQ(report.tally.oracle_failed, 0U);
  EXPECT_EQ(report.tally.oracle_blocks, 4U) << "two agreements and the two blocks it compared";
}

TEST(UnrunTest, AnOracleThatThrowsHasNotCheckedAnything) {
  Rig rig;
  rig.oracle.answers[0] = [](const verification::UnitEvidence&) -> verification::Verdict {
    throw std::runtime_error("scratch allocation failed");
  };
  rig.oracle.answers[2] = [](const verification::UnitEvidence&) -> verification::Verdict {
    throw 42;
  };
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 3);
  ASSERT_EQ(report.records.size(), 2U);
  const VerificationNotPerformed& first = unrun(report.records[0]);
  EXPECT_EQ(first.what, "oracle check of rotor unit 0");
  EXPECT_EQ(first.why, "the oracle threw: scratch allocation failed");
  EXPECT_EQ(first.blocks_compared, 0U);
  const VerificationNotPerformed& second = unrun(report.records[1]);
  EXPECT_EQ(second.why, "the oracle threw something that is not a std::exception");
  EXPECT_EQ(report.tally.oracle_not_performed, 2U);
  EXPECT_EQ(report.tally.oracle_blocks, 1U);
  EXPECT_EQ(report.tally.units_computed, 3U);
}

TEST(UnrunTest, AVerdictTheOracleContractForbidsIsBelievedNeitherWay) {
  verification::Difference broken;
  broken.popcount = 0;
  const std::vector<verification::Verdict> forbidden = {
      verification::Agreement{0},
      verification::ExactMismatch{"", std::nullopt, 1},
      verification::ExactMismatch{"output", std::nullopt, 0},
      verification::ExactMismatch{"output", broken, 1},
      verification::BoundedMismatch{"norm", 2.0, 1.0, std::nullopt, std::nullopt, 1},
      verification::NotChecked{"", "why", 0},
  };
  for (std::size_t which = 0; which < forbidden.size(); ++which) {
    SCOPED_TRACE("verdict " + std::to_string(which));
    Rig rig;
    rig.oracle.answers[0] = answer(forbidden[which]);
    const auto runner = make_runner(rig.setup);
    ASSERT_NE(runner, nullptr);
    const verification::SliceReport report = slice(*runner, 1);
    ASSERT_EQ(report.records.size(), 1U);
    const VerificationNotPerformed& record = unrun(report.records[0]);
    EXPECT_EQ(record.why, "the oracle's verdict (alternative " +
                              std::to_string(forbidden[which].index()) +
                              ") breaks the oracle contract");
    EXPECT_EQ(record.blocks_compared, 0U);
    EXPECT_EQ(report.tally.oracle_blocks, 0U) << "the blocks it claims are not counted";
    EXPECT_EQ(report.tally.oracle_failed, 0U);
    EXPECT_EQ(report.tally.oracle_not_performed, 1U);
  }
}

TEST(UnrunTest, ADutyCycleOfNothingFinishesWithBothChecksUnmade) {
  const Rig rig({0, 0});
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 10);
  EXPECT_TRUE(report.records.empty()) << "judged over the run, not a slice";
  EXPECT_TRUE(rig.oracle.checked.empty());
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 2U);
  const VerificationNotPerformed& oracle = unrun(finished.value().records[0]);
  EXPECT_EQ(oracle.what, "oracle over rotor units");
  EXPECT_EQ(oracle.why,
            "no block was compared: an oracle period of 0 over 10 units computed gave 0 to the "
            "oracle");
  EXPECT_EQ(oracle.blocks_compared, 0U);
  EXPECT_FALSE(oracle.provenance.iteration.has_value()) << "about the run, not a unit";
  EXPECT_FALSE(oracle.provenance.buffer_address.has_value());
  EXPECT_EQ(oracle.provenance.duty, (verification::DutyCycle{0, 0}));
  EXPECT_EQ(oracle.provenance.workload, "rotor");
  const VerificationNotPerformed& bracket = unrun(finished.value().records[1]);
  EXPECT_EQ(bracket.what, "input-integrity bracket over rotor units");
  EXPECT_EQ(bracket.why, "no unit was bracketed: an integrity period of 0 over 10 units computed");
}

TEST(UnrunTest, APeriodNoUnitOfTheRunReachesSamplesNothing) {
  Rig rig({100, 1});
  rig.setup.first_index = 1;
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  slice(*runner, 50);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 1U) << "the bracket ran; the oracle never did";
  EXPECT_EQ(unrun(finished.value().records[0]).what, "oracle over rotor units");
}

TEST(UnrunTest, AnOracleThatOnlyEverAnsweredNotCheckedComparedNothing) {
  Rig rig;
  for (std::uint64_t index = 0; index < 4; ++index) {
    rig.oracle.answers[index] = answer(verification::NotChecked{"rotor unit", "no reference", 0});
  }
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  EXPECT_EQ(slice(*runner, 4).records.size(), 4U);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 1U)
      << "four units given to the oracle and zero blocks compared is not a clean sweep";
  EXPECT_EQ(unrun(finished.value().records[0]).why,
            "no block was compared: an oracle period of 1 over 4 units computed gave 4 to the "
            "oracle");
}

TEST(UnrunTest, WhenTheBracketStoppedEverySampledUnitTheOracleComparedNothing) {
  Rig rig({2, 1});
  rig.tamper.after_write = [](std::uint64_t index, std::span<std::byte> input) {
    if (index % 2 == 0) {
      input[0] ^= std::byte{0x01};
    }
  };
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 6);
  EXPECT_EQ(report.records.size(), 3U) << "three inputs caught opening";
  EXPECT_TRUE(rig.oracle.checked.empty());
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 1U);
  EXPECT_EQ(unrun(finished.value().records[0]).what, "oracle over rotor units");
}

TEST(UnrunTest, ARunThatComputedNothingCheckedNothing) {
  const Rig rig;
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 2U);
  for (const verification::ErrorRecord& record : finished.value().records) {
    EXPECT_EQ(unrun(record).blocks_compared, 0U);
  }
}

TEST(UnrunTest, TextTooLongForARecordIsCutToFitRatherThanMakingItInvalid) {
  const std::string name(1100, 'w');
  const ShapedWorkload workload(name, workloads::Plan{64, 64, 0, 2, 1, 0, 0});
  const NamedOracle named(name, verification::Contract::kExact);
  ScriptedOracle oracle(named);
  oracle.answers[0] = [](const verification::UnitEvidence&) -> verification::Verdict {
    throw std::runtime_error(std::string(2000, 'x'));
  };
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  const verification::RunnerSetup setup = setup_for(workload, oracle, executor, syscalls, {1, 0});
  const auto runner = make_runner(setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 1);
  ASSERT_EQ(report.records.size(), 1U);
  const VerificationNotPerformed& thrown = unrun(report.records[0]);
  EXPECT_EQ(thrown.what.size(), verification::kMaxTextBytes);
  EXPECT_EQ(thrown.why.size(), verification::kMaxTextBytes);
  EXPECT_EQ(thrown.why.rfind("the oracle threw: xxx", 0), 0U);
  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished.value().records.size(), 2U);
  for (const verification::ErrorRecord& record : finished.value().records) {
    EXPECT_EQ(unrun(record).what.size(), verification::kMaxTextBytes);
  }
}

}  // namespace
}  // namespace loadforge::integrity_testing
