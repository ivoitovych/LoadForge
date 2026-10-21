// SPDX-License-Identifier: GPL-3.0-or-later
// T5 and T5b. The Monte Carlo workload has no verifier of its own: the check
// of spec.hpp is the oracle stream's. What this module owes is that the
// bytes it writes are what that check can classify, so these tests inject
// corruption into a unit's output, into its input and into the work itself,
// and assert which verdict the specification's check (written out in
// replay.hpp) gives -- including the floor below which a flipped bit is NOT
// seen, which the specification's tolerance sets and which is asserted
// rather than hoped for. T5b makes the check impossible and asserts it says
// so (NotChecked, nothing compared), never Agreement.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "oracle/workloads/monte_carlo/replay.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "worker/executor.hpp"
#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::workloads::monte_carlo {
namespace {

using replay::Verdict;
using testing::compute_unit;
using testing::generate_unit;
using testing::make;
using testing::native_path;
using testing::SerialExecutor;

constexpr std::uint64_t kIndex = 0;  // samples batches 0, 16, 32 and 48

struct Unit {
  std::vector<std::byte> input;
  std::vector<std::byte> output;
  replay::Reference reference;
};

Unit healthy_unit() {
  SerialExecutor serial;
  const auto workload = make(0xFA17, 256, native_path());
  Unit unit;
  unit.input = generate_unit(*workload, kIndex);
  unit.output = compute_unit(*workload, kIndex, unit.input, serial);
  unit.reference = replay::reference_for(kIndex, unit.input);
  return unit;
}

void put_double(std::vector<std::byte>& output, std::uint64_t batch, double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t byte = 0; byte < 8; ++byte) {
    output[batch * 8 + byte] = static_cast<std::byte>((bits >> (8 * byte)) & 0xFF);
  }
}

double get_double(const std::vector<std::byte>& output, std::uint64_t batch) {
  return replay::load_double(std::span<const std::byte>(output).subspan(batch * 8, 8));
}

TEST(OutputCorruption, AHealthyUnitAgreesAndSaysHowMuchItCompared) {
  const Unit unit = healthy_unit();
  const replay::Outcome outcome = replay::judge(unit.reference, unit.output);
  EXPECT_EQ(outcome.verdict, Verdict::kAgreement);
  EXPECT_EQ(outcome.blocks_compared, kSampledBatches);
}

TEST(OutputCorruption, AFlippedBitAboveTheToleranceIsClassifiedAsABoundedMismatch) {
  const Unit unit = healthy_unit();
  for (const std::uint64_t batch : {std::uint64_t{0}, std::uint64_t{16}, std::uint64_t{48}}) {
    // Mantissa bit 15 is the lowest the tolerance 2^-38 always sees; above it
    // come the rest of the mantissa, the exponent and the sign.
    for (unsigned bit = 15; bit < 64; ++bit) {
      std::vector<std::byte> output = unit.output;
      put_double(output, batch,
                 std::bit_cast<double>(std::bit_cast<std::uint64_t>(get_double(output, batch)) ^
                                       (std::uint64_t{1} << bit)));
      const replay::Outcome outcome = replay::judge(unit.reference, output);
      ASSERT_EQ(outcome.verdict, Verdict::kBoundedMismatch) << "batch " << batch << " bit " << bit;
      EXPECT_EQ(outcome.batch, batch);
      EXPECT_FALSE(outcome.residual <= outcome.tolerance);
      EXPECT_EQ(outcome.blocks_compared, kSampledBatches);
    }
  }
}

TEST(OutputCorruption, AFlippedBitBelowTheToleranceIsNotSeenAndTheFloorIsPinned) {
  const Unit unit = healthy_unit();
  for (unsigned bit = 0; bit <= 13; ++bit) {
    std::vector<std::byte> output = unit.output;
    put_double(output, 16,
               std::bit_cast<double>(std::bit_cast<std::uint64_t>(get_double(output, 16)) ^
                                     (std::uint64_t{1} << bit)));
    EXPECT_EQ(replay::judge(unit.reference, output).verdict, Verdict::kAgreement)
        << "mantissa bit " << bit << " moves the sum by less than 2^-38 of it";
  }
}

TEST(OutputCorruption, TheToleranceIsBreachedJustAboveAndHeldJustBelow) {
  const Unit unit = healthy_unit();
  const double reference = unit.reference.sum[1];  // batch 16
  std::vector<std::byte> output = unit.output;
  put_double(output, 16, reference * (1.0 + 0x1p-37));
  EXPECT_EQ(replay::judge(unit.reference, output).verdict, Verdict::kBoundedMismatch);
  put_double(output, 16, reference * (1.0 - 0x1p-37));
  EXPECT_EQ(replay::judge(unit.reference, output).verdict, Verdict::kBoundedMismatch);
  put_double(output, 16, reference * (1.0 + 0x1p-39));
  EXPECT_EQ(replay::judge(unit.reference, output).verdict, Verdict::kAgreement);
  put_double(output, 16, reference);
  EXPECT_EQ(replay::judge(unit.reference, output).verdict, Verdict::kAgreement);
}

TEST(OutputCorruption, NotANumberAndInfinityAreMismatchesNotPasses) {
  const Unit unit = healthy_unit();
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), 0.0, -0.0}) {
    std::vector<std::byte> output = unit.output;
    put_double(output, 32, bad);
    const replay::Outcome outcome = replay::judge(unit.reference, output);
    EXPECT_EQ(outcome.verdict, Verdict::kBoundedMismatch) << bad;
    EXPECT_EQ(outcome.batch, 32U);
  }
}

TEST(OutputCorruption, TheSmallestFailingBatchNumberIsReportedNotTheFirstSampled) {
  // Unit 60 samples 60, 12, 28, 44 in that order; corrupt 60 and 28 and 44.
  SerialExecutor serial;
  const auto workload = make(0xFA17, 0, native_path());
  const std::vector<std::byte> input = generate_unit(*workload, 60);
  std::vector<std::byte> output = compute_unit(*workload, 60, input, serial);
  const replay::Reference reference = replay::reference_for(60, input);
  ASSERT_EQ(reference.batch[0], 60U);
  for (const std::uint64_t batch : {std::uint64_t{60}, std::uint64_t{28}, std::uint64_t{44}}) {
    put_double(output, batch, get_double(output, batch) + 1.0);
  }
  const replay::Outcome outcome = replay::judge(reference, output);
  EXPECT_EQ(outcome.verdict, Verdict::kBoundedMismatch);
  EXPECT_EQ(outcome.batch, 28U);
  EXPECT_EQ(outcome.blocks_compared, kSampledBatches) << "every sampled batch is replayed";
}

TEST(OutputCorruption, ABatchNobodySamplesIsNotCheckedInThatUnit) {
  const Unit unit = healthy_unit();
  std::vector<std::byte> output = unit.output;
  put_double(output, 1, -1.0);
  EXPECT_EQ(replay::judge(unit.reference, output).verdict, Verdict::kAgreement);
  // Across units it is: unit 1 samples batch 1.
  EXPECT_EQ(sampled_batch(1, 0), 1U);
}

TEST(InputCorruption, ACorruptedKeyIsComputedOnAndCaughtOnlyAgainstTheRegeneratedInput) {
  SerialExecutor serial;
  const auto workload = make(0xFA17, 256, native_path());
  const std::vector<std::byte> input = generate_unit(*workload, kIndex);
  std::vector<std::byte> corrupted = input;
  corrupted[16 * 8 + 3] ^= std::byte{0x40};
  const std::vector<std::byte> output = compute_unit(*workload, kIndex, corrupted, serial);

  // Judged against the input it was given, the output is right: the oracle
  // judges the output against the input, and the input is the runner's.
  EXPECT_EQ(replay::check(kIndex, corrupted, output).verdict, Verdict::kAgreement);
  // Judged against the input the workload would regenerate, it is wrong.
  const replay::Outcome against_truth =
      replay::check(kIndex, generate_unit(*workload, kIndex), output);
  EXPECT_EQ(against_truth.verdict, Verdict::kBoundedMismatch);
  EXPECT_EQ(against_truth.batch, 16U);
  // And regeneration is what shows the input itself changed.
  EXPECT_NE(corrupted, generate_unit(*workload, kIndex));
}

/// A faulty executor: drops one task, as a lost wake-up or a dead thread
/// would, and runs the rest.
class DroppingExecutor final : public worker::Executor {
 public:
  explicit DroppingExecutor(std::size_t dropped) noexcept : dropped_(dropped) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      if (index != dropped_) {
        task(context, index, 0);
      }
    }
  }

 private:
  std::size_t dropped_;
};

TEST(WorkCorruption, ALostBatchLeavesStaleBytesAndASampledOneIsCaught) {
  const auto workload = make(0xFA17, 256, native_path());
  const std::vector<std::byte> input = generate_unit(*workload, kIndex);
  const replay::Reference reference = replay::reference_for(kIndex, input);
  DroppingExecutor drops_sampled(32);
  const std::vector<std::byte> lost = compute_unit(*workload, kIndex, input, drops_sampled);
  const replay::Outcome caught = replay::judge(reference, lost);
  EXPECT_EQ(caught.verdict, Verdict::kBoundedMismatch);
  EXPECT_EQ(caught.batch, 32U);

  DroppingExecutor drops_unsampled(33);
  EXPECT_EQ(
      replay::judge(reference, compute_unit(*workload, kIndex, input, drops_unsampled)).verdict,
      Verdict::kAgreement)
      << "batch 33 is sampled by other units, not this one";
}

TEST(UnrunCheck, EvidenceOfTheWrongSizeIsNotCheckedAndComparesNothing) {
  const Unit unit = healthy_unit();
  const std::span<const std::byte> input = unit.input;
  const std::span<const std::byte> output = unit.output;
  struct Case {
    std::span<const std::byte> input;
    std::span<const std::byte> output;
  };
  const std::vector<std::byte> empty;
  for (const Case& c :
       {Case{input.first(kInputBytes - 1), output}, Case{input, output.first(kOutputBytes - 1)},
        Case{empty, empty}, Case{input, empty}, Case{empty, output}}) {
    const replay::Outcome outcome = replay::check(kIndex, c.input, c.output);
    EXPECT_EQ(outcome.verdict, Verdict::kNotChecked);
    EXPECT_EQ(outcome.blocks_compared, 0U);
  }
  const replay::Outcome whole = replay::check(kIndex, input, output);
  EXPECT_EQ(whole.verdict, Verdict::kAgreement);
  EXPECT_EQ(whole.blocks_compared, kSampledBatches);
}

TEST(UnrunCheck, AnAgreementAlwaysCountsTheBatchesItReplayed) {
  const Unit unit = healthy_unit();
  for (std::uint64_t index = 0; index < 3; ++index) {
    const replay::Outcome outcome =
        replay::judge(replay::reference_for(index, unit.input), unit.output);
    if (outcome.verdict == Verdict::kAgreement) {
      EXPECT_EQ(outcome.blocks_compared, kSampledBatches);
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::monte_carlo
