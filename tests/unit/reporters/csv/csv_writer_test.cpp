// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CSV reporter against the result-writer contract, and the table it writes
// read back by a reader that refuses anything that is not RFC 4180.

#include "reporters/csv/csv_writer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "contracts/results/writer_contract.hpp"
#include "unit/controller/results/rich_run.hpp"
#include "unit/reporters/csv/mini_csv.hpp"

namespace loadforge::reporters {

struct CsvSubject {
  static constexpr const char* kFormat = "csv";
  static std::unique_ptr<ResultWriter> make() { return std::make_unique<CsvWriter>(); }
};

namespace {

using results::testing::bare_run;
using results::testing::rich_run;
using testing::CsvRecord;
using testing::parse_csv;

std::string written(const results::RunResult& run) {
  std::ostringstream out;
  const auto wrote = CsvWriter{}.write(run, out);
  EXPECT_TRUE(wrote.has_value());
  return out.str();
}

const CsvRecord* find_row(const std::vector<CsvRecord>& records, const std::string& field) {
  for (const CsvRecord& each : records) {
    if (each[1] == field) {
      return &each;
    }
  }
  return nullptr;
}

TEST(CsvWriterTest, TheSchemaVersionIsTheFirstColumnOfEveryRow) {
  results::RunResult run = rich_run();
  run.schema_version = 4242;
  const auto records = parse_csv(written(run));
  EXPECT_EQ(records[0][0], "schema_version");
  for (std::size_t i = 1; i < records.size(); ++i) {
    EXPECT_EQ(records[i][0], "4242");
  }
  EXPECT_EQ(records[1][1], "schema_version");
}

TEST(CsvWriterTest, TheWholeResultIsValidCsvAndKeepsEverythingItHad) {
  const auto records = parse_csv(written(rich_run()));
  EXPECT_EQ((*find_row(records, "mode"))[2], "stress");
  EXPECT_EQ((*find_row(records, "verdict.code"))[2], "findings");
  EXPECT_EQ((*find_row(records, "fingerprint.cpu_model"))[2], "Example CPU, \"quoted\"");
  EXPECT_EQ((*find_row(records, "workloads[0].tally.oracle_failed"))[2], "2");
  EXPECT_EQ((*find_row(records, "workloads[0].findings[5].record.what"))[2],
            "oracle, with a comma, and \"quotes\"\nand a newline");
  EXPECT_EQ((*find_row(records, "workloads[0].findings[2].record.residual"))[2], "nan");
  EXPECT_EQ((*find_row(records, "workers[1].termination"))[2], "killed by signal 9");
  EXPECT_NE(find_row(records, "workloads[0].findings[0].isolation.steps[1].outcome"), nullptr);
  EXPECT_NE(find_row(records, "workloads[0].findings[0].telemetry[2].reading"), nullptr);
}

TEST(CsvWriterTest, EveryAbsentValueIsAnEmptyCellWithTheReasonBesideIt) {
  const auto records = parse_csv(written(bare_run()));
  for (const char* name : {"git_commit", "cpu_model", "temperature_at_start_c", "fp_environment",
                           "isa_path", "observers.controller_overhead"}) {
    const CsvRecord* row = find_row(records, std::string(name).find('.') == std::string::npos
                                                 ? std::string("fingerprint.") + name
                                                 : std::string(name));
    ASSERT_NE(row, nullptr) << name;
    EXPECT_EQ((*row)[2], "") << name << " is empty, never zero";
    EXPECT_EQ((*row)[3], "absent") << name;
    EXPECT_EQ((*row)[4], "not recorded") << name;
  }
  EXPECT_EQ((*find_row(records, "capabilities"))[3], "empty");
}

TEST(CsvWriterTest, AnUnavailableSummaryAndAnAbsentMetricKeepTheirReasonInTheirColumns) {
  const auto records = parse_csv(written(contracts::results_suite::clean_run()));
  const CsvRecord* summary = find_row(records, "signature.metrics[0].summary");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ((*summary)[2], "");
  EXPECT_EQ((*summary)[3], "denied");
  EXPECT_EQ((*summary)[4], "not permitted");
  EXPECT_EQ((*summary)[5], "/sys/class/powercap/intel-rapl:0/energy_uj");
  EXPECT_EQ((*summary)[6], contracts::results_suite::kDeniedDetail);
  EXPECT_EQ((*summary)[7], contracts::results_suite::kDeniedRemedy);
  const CsvRecord* metric = find_row(records, "capabilities[0].metrics[0].availability");
  ASSERT_NE(metric, nullptr);
  EXPECT_EQ((*metric)[3], "absent");
  EXPECT_EQ((*metric)[7], contracts::results_suite::kAbsentRemedy);
}

TEST(CsvWriterTest, ABoundedFailureHasNoBitFieldsAndAnUndifferedOneNoClassification) {
  const auto records = parse_csv(written(rich_run()));
  EXPECT_EQ(find_row(records, "workloads[0].findings[2].record.side"), nullptr);
  EXPECT_EQ(find_row(records, "workloads[0].findings[2].record.difference"), nullptr);
  EXPECT_EQ((*find_row(records, "workloads[0].findings[1].record.bit_class"))[3], "absent");
  EXPECT_EQ((*find_row(records, "workloads[0].findings[1].record.bit_class"))[2], "");
  EXPECT_EQ((*find_row(records, "workloads[0].findings[0].record.bit_class"))[2], "single bit");
}

TEST(CsvWriterTest, TheStreamIsCheckedBeforeAndAfter) {
  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  const auto before = CsvWriter{}.write(rich_run(), bad);
  ASSERT_FALSE(before.has_value());
  EXPECT_NE(before.error().detail.find("not writable"), std::string::npos);
  EXPECT_TRUE(bad.str().empty());
  contracts::results_suite::RefusingBuffer refusing;
  std::ostream full(&refusing);
  const auto after = CsvWriter{}.write(rich_run(), full);
  ASSERT_FALSE(after.has_value());
  EXPECT_NE(after.error().detail.find("refused"), std::string::npos);
  EXPECT_NE(after.error().detail.find("csv"), std::string::npos);
}

}  // namespace
}  // namespace loadforge::reporters

namespace loadforge::contracts {
// writer_contract.hpp includes results_contract.hpp, which registers VerdictContract: a
// binary that runs only the writer suite must say it means to leave that one out.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VerdictContract);
INSTANTIATE_TYPED_TEST_SUITE_P(Csv, ResultWriterContract, ::testing::Types<reporters::CsvSubject>);
}  // namespace loadforge::contracts
