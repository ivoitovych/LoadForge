// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for golden vectors: a vector recorded from a BitExact workload is the
// same at every thread count (1, 2, 3, 4 and 7, non-powers of two included,
// determinism.md §5), on every repeat, at every decomposition, and on every
// machine -- the last by comparing a fresh recording with the vector committed
// under tests/fixtures/golden/, which CI does on x86-64 and on ARM64.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "unit/verification/golden/mock_workload.hpp"
#include "verification/golden/golden.hpp"

namespace loadforge::verification {
namespace {

using golden_test::MockWorkload;
using golden_test::SerialExecutor;
using golden_test::ThreadedExecutor;

constexpr const char* kFixture =
    LOADFORGE_SOURCE_DIR "/tests/fixtures/golden/verification/golden/golden_mock-scalar.golden";

GoldenVector record_with(worker::Executor& executor,
                         const workloads::WorkloadConfig& config = golden_test::fixture_config(),
                         std::uint64_t units = golden_test::kFixtureUnits) {
  auto workload = MockWorkload::create(config);
  EXPECT_TRUE(workload.has_value());
  auto vector = record_golden(*workload.value(), config, units, executor);
  EXPECT_TRUE(vector.has_value());
  return vector.value();
}

TEST(GoldenDeterminism, EveryThreadCountRecordsTheSameVector) {
  SerialExecutor serial;
  const GoldenVector reference = record_with(serial);
  for (const std::size_t threads : {1, 2, 3, 4, 7}) {
    ThreadedExecutor executor(threads);
    EXPECT_EQ(record_with(executor), reference) << threads << " thread(s)";
  }
}

TEST(GoldenDeterminism, ARepeatRecordsTheSameVector) {
  ThreadedExecutor executor(3);
  const GoldenVector first = record_with(executor);
  for (int repeat = 0; repeat < 5; ++repeat) {
    EXPECT_EQ(record_with(executor), first) << repeat;
  }
}

TEST(GoldenDeterminism, ABitExactWorkloadsBitsDoNotDependOnItsDecomposition) {
  SerialExecutor serial;
  const GoldenVector reference = record_with(serial);
  for (const std::uint32_t decomposition : {1U, 2U, 3U, 8U, 64U}) {
    workloads::WorkloadConfig config = golden_test::fixture_config();
    config.decomposition = decomposition;
    ThreadedExecutor executor(4);
    const GoldenVector vector = record_with(executor, config);
    EXPECT_EQ(vector.units, reference.units) << decomposition;
    EXPECT_EQ(vector.digest, reference.digest) << decomposition;
    EXPECT_EQ(vector.key.resolved_decomposition, decomposition);
  }
}

TEST(GoldenDeterminism, TheCommittedVectorIsReproducedHereAtEveryThreadCount) {
  std::ifstream file(kFixture);
  ASSERT_TRUE(file.is_open()) << kFixture;
  std::stringstream text;
  text << file.rdbuf();
  const auto committed = parse_golden(text.str());
  ASSERT_TRUE(committed.has_value()) << describe(committed.error());
  const GoldenVector& vector = committed.value();
  ASSERT_EQ(config_of(vector.key), golden_test::fixture_config());

  SerialExecutor serial;
  std::vector<std::unique_ptr<worker::Executor>> executors;
  for (const std::size_t threads : {1, 2, 3, 4, 7}) {
    executors.push_back(std::make_unique<ThreadedExecutor>(threads));
  }
  const GoldenReport serial_report =
      compare_golden(vector, record_with(serial, config_of(vector.key), vector.key.units));
  EXPECT_EQ(serial_report.verdict, GoldenVerdict::kUnchanged) << render(serial_report, vector.key);
  for (const auto& executor : executors) {
    const GoldenReport report =
        compare_golden(vector, record_with(*executor, config_of(vector.key), vector.key.units));
    EXPECT_EQ(report.verdict, GoldenVerdict::kUnchanged) << render(report, vector.key);
  }
}

TEST(GoldenDeterminism, TheCommittedVectorDetectsAnIntendedChangeAndCallsItNoFault) {
  // Exit criterion 4 against the committed vector itself: the workload's bits
  // change and its version moves, and the report is a declared change -- a
  // stale vector, never an error record and never a hardware verdict.
  const std::ifstream file(kFixture);
  std::stringstream text;
  text << file.rdbuf();
  const auto committed = parse_golden(text.str());
  ASSERT_TRUE(committed.has_value());
  const workloads::WorkloadConfig config = config_of(committed.value().key);
  auto changed = MockWorkload::create(config, 2, golden_test::Flavor::kChanged);
  ASSERT_TRUE(changed.has_value());
  ThreadedExecutor executor(7);
  const auto fresh = record_golden(*changed.value(), config, committed.value().key.units, executor);
  ASSERT_TRUE(fresh.has_value());
  const GoldenReport report = compare_golden(committed.value(), fresh.value());
  EXPECT_EQ(report.verdict, GoldenVerdict::kDeclaredChange);
  EXPECT_EQ(report.changes.size(), 1 + golden_test::kFixtureUnits + 1)
      << "the version, every unit's output, and the digest -- and no input";
  EXPECT_NE(render(report, committed.value().key).find("It is not a finding about the hardware."),
            std::string::npos);
}

// Writes the committed fixture. Run only on purpose -- regeneration is a
// deliberate, reviewed act (verification.md §4) -- with:
//
//   LOADFORGE_GOLDEN_OUT=<path> LOADFORGE_GOLDEN_NOTE=<why, when, where>
//     <this binary> --gtest_also_run_disabled_tests
//     --gtest_filter=GoldenDeterminism.DISABLED_RecordTheCommittedFixture
//
// as one command line.
TEST(GoldenDeterminism, DISABLED_RecordTheCommittedFixture) {
  const char* out = std::getenv("LOADFORGE_GOLDEN_OUT");
  const char* note = std::getenv("LOADFORGE_GOLDEN_NOTE");
  ASSERT_NE(out, nullptr) << "LOADFORGE_GOLDEN_OUT names the file to write";
  ASSERT_NE(note, nullptr) << "LOADFORGE_GOLDEN_NOTE says who recorded it, where, and why";
  SerialExecutor serial;
  std::ofstream file(out);
  file << format_golden(record_with(serial), note);
  ASSERT_TRUE(file.good());
}

}  // namespace
}  // namespace loadforge::verification
