// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the protected control state (PLAN F18 rule 3, verification.md §6:
// "corrupt a stored seed -> the control-state majority vote must repair or
// reject it, and must not emit a hardware-fault report").
//
// The copies are corrupted in the memory the store holds, reached through the
// mappings the system-call seam handed out, in every way the vote separates:
// a copy whose checksum fails, a copy that verifies but is outvoted, two or
// three copies that fail, three that verify and disagree. Then the same
// corruptions under a running runner, where the question is what the user is
// told: a repaired copy changes no record, and an undecidable one stops the
// run with a VerificationNotPerformed -- never an ExactBitCorruption about a
// unit regenerated from a wrong seed or index.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/crc32.hpp"
#include "unit/verification/integrity/integrity_support.hpp"
#include "verification/integrity/control_state.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"

namespace loadforge::integrity_testing {
namespace {

using verification::integrity::ControlState;
using verification::integrity::ControlWord;
using verification::integrity::ControlWords;
using verification::integrity::kCopies;
using verification::integrity::kCopyBytes;
using verification::integrity::VoteFault;

constexpr std::size_t kSeedByte = 0;    // word 0
constexpr std::size_t kIndexByte = 16;  // word 2
constexpr std::size_t kCrcByte = 48;    // after six words

ControlWords sample() { return ControlWords{0x5EED, 1, 1000, 1, 1, 64}; }

/// A copy holding `words` with a checksum that verifies: what a copy that
/// is wrong but consistent -- a stale value, say -- looks like.
void forge(std::span<std::byte> copy, const ControlWords& words) {
  for (std::size_t index = 0; index < words.size(); ++index) {
    for (std::size_t byte = 0; byte < 8; ++byte) {
      copy[index * 8 + byte] = static_cast<std::byte>(words.at(index) >> (8 * byte));
    }
  }
  const std::uint32_t crc = core::crc32(copy.first(48));
  for (std::size_t byte = 0; byte < 4; ++byte) {
    copy[48 + byte] = static_cast<std::byte>(crc >> (8 * byte));
  }
}

std::vector<std::byte> bytes_of(std::span<const std::byte> copy) {
  return {copy.begin(), copy.begin() + kCopyBytes};
}

struct Store {
  Store() : state(std::move(ControlState::create(syscalls, sample(), {})).value()) {}
  [[nodiscard]] std::span<std::byte> copy(std::size_t which) const {
    return syscalls.mapping(which);
  }
  SteppedSyscalls syscalls;
  ControlState state;
};

// --- the vote, alone --------------------------------------------------------------

TEST(ControlVoteTest, ACopyThatFailsItsChecksumIsExcludedAndRepaired) {
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    for (const std::size_t byte : {kSeedByte, kIndexByte, kCrcByte}) {
      SCOPED_TRACE("copy " + std::to_string(copy) + ", byte " + std::to_string(byte));
      Store store;
      const std::vector<std::byte> clean = bytes_of(store.copy(copy));
      store.copy(copy)[byte] ^= std::byte{0x01};
      const auto read = store.state.read();
      ASSERT_TRUE(read.has_value());
      EXPECT_EQ(read.value(), sample());
      EXPECT_EQ(store.state.repairs(), 1U);
      EXPECT_EQ(bytes_of(store.copy(copy)), clean) << "rewritten from the decided value";
      ASSERT_TRUE(store.state.read().has_value());
      EXPECT_EQ(store.state.repairs(), 1U) << "repaired once, and clean since";
    }
  }
}

TEST(ControlVoteTest, ACopyThatVerifiesButIsOutvotedIsRepaired) {
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    SCOPED_TRACE("copy " + std::to_string(copy));
    Store store;
    const std::vector<std::byte> clean = bytes_of(store.copy(copy));
    ControlWords stale = sample();
    verification::integrity::set_word(stale, ControlWord::kIndex, 999);
    forge(store.copy(copy), stale);
    const auto read = store.state.read();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), sample()) << "the two verified copies decide";
    EXPECT_EQ(store.state.repairs(), 1U);
    EXPECT_EQ(bytes_of(store.copy(copy)), clean);
  }
}

TEST(ControlVoteTest, TwoOrThreeCopiesFailingTheirChecksumsLeaveNoVote) {
  const std::vector<std::vector<std::size_t>> broken = {{0, 1}, {0, 2}, {1, 2}, {0, 1, 2}};
  for (const std::vector<std::size_t>& copies : broken) {
    SCOPED_TRACE("copies broken: " + std::to_string(copies.size()));
    Store store;
    for (const std::size_t copy : copies) {
      store.copy(copy)[kSeedByte] ^= std::byte{0x80};
    }
    std::array<std::vector<std::byte>, kCopies> before{};
    for (std::size_t copy = 0; copy < kCopies; ++copy) {
      before.at(copy) = bytes_of(store.copy(copy));
    }
    const auto read = store.state.read();
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().fault, VoteFault::kTooFewVerified);
    for (std::size_t copy = 0; copy < kCopies; ++copy) {
      const bool is_broken = std::find(copies.begin(), copies.end(), copy) != copies.end();
      EXPECT_EQ(read.error().verified.at(copy), !is_broken) << copy;
      EXPECT_EQ(bytes_of(store.copy(copy)), before.at(copy)) << "nothing is repaired from one copy";
    }
    EXPECT_EQ(store.state.repairs(), 0U);
  }
}

TEST(ControlVoteTest, VerifiedCopiesThatAllDisagreeAreAHardFailureNotAGuess) {
  Store three;
  ControlWords one = sample();
  ControlWords two = sample();
  verification::integrity::set_word(one, ControlWord::kSeed, 1);
  verification::integrity::set_word(two, ControlWord::kSeed, 2);
  forge(three.copy(1), one);
  forge(three.copy(2), two);
  const auto all = three.state.read();
  ASSERT_FALSE(all.has_value());
  EXPECT_EQ(all.error().fault, VoteFault::kDisagreement);
  EXPECT_EQ(all.error().verified, (std::array<bool, kCopies>{true, true, true}));
  EXPECT_EQ(three.state.repairs(), 0U);

  for (std::size_t bad = 0; bad < kCopies; ++bad) {
    SCOPED_TRACE("copy " + std::to_string(bad) + " fails its checksum");
    Store two_left;
    const std::size_t other = (bad + 1) % kCopies;
    forge(two_left.copy(other), one);
    two_left.copy(bad)[kCrcByte] ^= std::byte{0x01};
    const auto read = two_left.state.read();
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().fault, VoteFault::kDisagreement) << "two verified, unequal";
    EXPECT_FALSE(read.error().verified.at(bad));
  }
}

// --- under a running runner ---------------------------------------------------------

/// The rotor workload with its output scripted wrong at one unit, so a record
/// shows what seed and index the runner was using when it made it.
struct Rig {
  Rig() : workload(make_rotor()), tamper(*workload) {
    setup = setup_for(tamper, oracle, executor, syscalls);
  }
  /// Copy `which` of the runner's control state: mappings 1 to 3.
  [[nodiscard]] std::span<std::byte> copy(std::size_t which) const {
    return syscalls.mapping(1 + which);
  }

  std::unique_ptr<workloads::Workload> workload;
  TamperingWorkload tamper;
  RotorOracle oracle;
  SerialExecutor executor;
  SteppedSyscalls syscalls;
  verification::RunnerSetup setup;
};

TEST(ControlStateUnderRunTest, ACorruptedStoredSeedIsRepairedAndNeverReported) {
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    SCOPED_TRACE("copy " + std::to_string(copy));
    Rig rig;
    rig.tamper.after_compute = [](std::uint64_t index, std::span<std::byte> output) {
      if (index == 6) {
        output[3] ^= std::byte{0x20};
      }
    };
    const auto runner = make_runner(rig.setup);
    ASSERT_NE(runner, nullptr);
    EXPECT_TRUE(slice(*runner, 4).records.empty());
    rig.copy(copy)[kSeedByte] ^= std::byte{0x04};  // seed 7 becomes 3 in this copy
    const verification::SliceReport report = slice(*runner, 4);
    ASSERT_EQ(report.records.size(), 1U) << "the scripted wrong output, and nothing else";
    const auto* exact = std::get_if<verification::ExactBitCorruption>(report.records.data());
    ASSERT_NE(exact, nullptr);
    EXPECT_EQ(exact->side, verification::Side::kOutput);
    EXPECT_EQ(exact->provenance.seed, 7U) << "the voted seed, not the corrupted copy's";
    EXPECT_EQ(exact->provenance.iteration, 6U);
    EXPECT_EQ(rig.copy(copy)[kSeedByte], std::byte{0x07}) << "the copy was repaired";
  }
}

TEST(ControlStateUnderRunTest, ACorruptedStoredIndexNeverBecomesAFalseInputFinding) {
  // Unvoted, unit 4 regenerated from index 4 ^ 0x40 would differ from the
  // slot written from index 4, and the bracket would report an input
  // corruption that never happened.
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    SCOPED_TRACE("copy " + std::to_string(copy));
    Rig rig;
    ScriptedOracle seen(rig.oracle);
    rig.setup.oracle = &seen;
    const auto runner = make_runner(rig.setup);
    ASSERT_NE(runner, nullptr);
    slice(*runner, 4);
    rig.copy(copy)[kIndexByte] ^= std::byte{0x40};
    const verification::SliceReport report = slice(*runner, 4);
    EXPECT_TRUE(report.records.empty());
    EXPECT_EQ(report.next_index, 8U);
    EXPECT_EQ(seen.checked, (std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6, 7}));
  }
}

TEST(ControlStateUnderRunTest, AUnitKeepsTheValuesVotedAtItsStartWhateverHappensToTheCopies) {
  // All three copies changed alike while unit 3 is computed -- the one
  // corruption a vote cannot see. Unit 3 still closes on index 3, voted
  // before it began, and the copies are rewritten from those values when it
  // ends: the stored state is consulted at votes and nowhere else.
  Rig rig;
  rig.tamper.during_compute = [&rig](std::uint64_t index, std::span<std::byte>) {
    if (index == 3) {
      for (std::size_t copy = 0; copy < kCopies; ++copy) {
        rig.copy(copy)[kIndexByte] ^= std::byte{0x10};
      }
    }
  };
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 6);
  EXPECT_TRUE(report.records.empty());
  EXPECT_EQ(report.next_index, 6U);
  EXPECT_EQ(rig.copy(0)[kIndexByte], std::byte{6});
}

TEST(ControlStateUnderRunTest, AnUndecidableControlStateStopsTheRunAndSaysItIsTheRuns) {
  const Rig rig;
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  EXPECT_TRUE(slice(*runner, 4).records.empty());
  rig.copy(0)[kIndexByte] ^= std::byte{0x01};
  rig.copy(2)[kSeedByte] ^= std::byte{0x01};
  const verification::SliceReport report = slice(*runner, 10);
  EXPECT_EQ(report.tally, verification::Tally{}) << "no unit runs on an undecided state";
  EXPECT_EQ(report.next_index, 4U);
  ASSERT_EQ(report.records.size(), 1U) << "no hardware finding about any unit";
  EXPECT_FALSE(verification::is_wrong_result(report.records[0]));
  const auto* stopped = std::get_if<verification::VerificationNotPerformed>(report.records.data());
  ASSERT_NE(stopped, nullptr);
  EXPECT_EQ(stopped->what, "control state of the rotor run");
  EXPECT_EQ(stopped->why,
            "fewer than two copies of the control state pass their checksums (copy 0 fails its "
            "checksum, copy 1 verified, copy 2 fails its checksum); the run stops rather than "
            "pick a value");
  EXPECT_FALSE(stopped->provenance.iteration.has_value());
  EXPECT_TRUE(verification::is_valid(report.records[0]));
  EXPECT_EQ(runner->total().units_computed, 4U);

  const auto after = runner->run_slice({1});
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error().fault, verification::RunnerFault::kFinished);
  EXPECT_EQ(after.error().detail.rfind("the run stopped before unit 4: fewer than two", 0), 0U)
      << after.error().detail;

  const auto finished = runner->finish();
  ASSERT_TRUE(finished.has_value()) << "the run still closes, once";
  EXPECT_TRUE(finished.value().records.empty()) << "it compared and bracketed before it stopped";
  EXPECT_EQ(runner->finish().error().fault, verification::RunnerFault::kFinished);
}

TEST(ControlStateUnderRunTest, ThreeVerifiedCopiesThatDisagreeStopTheRunBeforeItsNextUnit) {
  const Rig rig;
  const auto runner = make_runner(rig.setup);
  ASSERT_NE(runner, nullptr);
  slice(*runner, 2);
  const ControlWords one{7, 1, 2, 1, 1, 64};
  const ControlWords two{7, 1, 9, 1, 1, 64};
  forge(rig.copy(1), one);
  forge(rig.copy(2), two);
  const ControlWords zero{7, 1, 5, 1, 1, 64};
  forge(rig.copy(0), zero);
  const verification::SliceReport report = slice(*runner, 3);
  EXPECT_EQ(report.tally, verification::Tally{});
  ASSERT_EQ(report.records.size(), 1U);
  const auto* stopped = std::get_if<verification::VerificationNotPerformed>(report.records.data());
  ASSERT_NE(stopped, nullptr);
  EXPECT_EQ(stopped->why.rfind("the verified copies of the control state disagree", 0), 0U);
  EXPECT_EQ(report.next_index, 2U) << "the progress last confirmed";
}

}  // namespace
}  // namespace loadforge::integrity_testing
