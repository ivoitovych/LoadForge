// SPDX-License-Identifier: GPL-3.0-or-later
//
// The JSON reporter against the result-writer contract, and the document it
// writes read back by a reader that refuses anything that is not JSON.

#include "reporters/json/json_writer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "contracts/results/writer_contract.hpp"
#include "unit/controller/results/rich_run.hpp"
#include "unit/reporters/json/mini_json.hpp"

namespace loadforge::reporters {

struct JsonSubject {
  static constexpr const char* kFormat = "json";
  static std::unique_ptr<ResultWriter> make() { return std::make_unique<JsonWriter>(); }
};

namespace {

using results::testing::bare_run;
using results::testing::rich_run;
using testing::Json;
using testing::JsonReader;

std::string written(const results::RunResult& run) {
  std::ostringstream out;
  const auto wrote = JsonWriter{}.write(run, out);
  EXPECT_TRUE(wrote.has_value());
  return out.str();
}

TEST(JsonWriterTest, TheSchemaVersionIsTheFirstFieldOfTheDocument) {
  const std::string text = written(rich_run());
  EXPECT_EQ(text.rfind("{\n  \"schema_version\": 3,\n", 0), 0U) << text.substr(0, 60);
  results::RunResult run = rich_run();
  run.schema_version = 4242;
  EXPECT_EQ(written(run).rfind("{\n  \"schema_version\": 4242,\n", 0), 0U);
}

TEST(JsonWriterTest, TheWholeResultIsValidJsonAndKeepsEverythingItHad) {
  const Json doc = JsonReader::parse(written(rich_run()));
  EXPECT_EQ(doc.at("mode").text, "stress");
  EXPECT_EQ(doc.at("elapsed_ns").text, "3600000000000");
  EXPECT_EQ(doc.at("verdict").at("code").text, "findings");
  EXPECT_EQ(doc.at("fingerprint").at("cpu_model").text, "Example CPU, \"quoted\"");
  EXPECT_EQ(doc.at("fingerprint").at("git_commit").text,
            "5782b0f3097691f095afb53d39bbf1e95e573e79");
  EXPECT_EQ(doc.at("workloads").items.size(), 1U);
  EXPECT_EQ(doc.at("workloads").at(0).at("findings").items.size(), 6U);
  EXPECT_EQ(doc.at("workers").items.size(), 2U);
  const Json& unrun = doc.at("workloads").at(0).at("findings").at(5).at("record");
  EXPECT_EQ(unrun.at("what").text, "oracle, with a comma, and \"quotes\"\nand a newline");
  EXPECT_NE(unrun.at("why").text.find("\xef\xbf\xbd"), std::string::npos)
      << "the byte that was not UTF-8 is U+FFFD, not dropped";
  EXPECT_NE(unrun.at("why").text.find('\x01'), std::string::npos);
}

TEST(JsonWriterTest, EveryAbsentValueIsNullAndNeverZero) {
  const Json doc = JsonReader::parse(written(bare_run()));
  const Json& fingerprint = doc.at("fingerprint");
  for (const char* name :
       {"git_commit", "cpu_model", "microcode", "kernel", "smt_enabled", "power_source",
        "battery_state", "cpufreq_governor", "energy_performance_preference", "platform_profile",
        "turbo_enabled", "cooling_profile", "cgroup_memory_limit_bytes", "huge_page_policy",
        "temperature_at_start_c", "fp_environment", "isa_path"}) {
    EXPECT_EQ(fingerprint.at(name).kind, Json::Kind::kNull) << name;
  }
  EXPECT_EQ(fingerprint.at("absent_fields").items.size(), 17U);
  EXPECT_EQ(doc.at("observers").at("controller_overhead").kind, Json::Kind::kNull);
  EXPECT_EQ(doc.at("observers").at("journal_flush_interval_ns").kind, Json::Kind::kNull);
}

TEST(JsonWriterTest, AnUnavailableSummaryAndAnAbsentMetricKeepTheirReasonBesideTheNull) {
  const Json doc = JsonReader::parse(written(contracts::results_suite::clean_run()));
  const Json& summary = doc.at("signature").at("metrics").at(0);
  EXPECT_EQ(summary.at("summary").kind, Json::Kind::kNull);
  EXPECT_EQ(summary.at("summary_unavailable").at("kind").text, "denied");
  EXPECT_EQ(summary.at("summary_unavailable").at("remediation").text,
            contracts::results_suite::kDeniedRemedy);
  const Json& metric = doc.at("capabilities").at(0).at("metrics").at(0);
  EXPECT_EQ(metric.at("availability").kind, Json::Kind::kNull);
  EXPECT_EQ(metric.at("availability_unavailable").at("kind").text, "absent");
}

TEST(JsonWriterTest, AFailureThatIsNotANumberIsNotZeroAndNotNull) {
  const Json doc = JsonReader::parse(written(rich_run()));
  const Json& numerical = doc.at("workloads").at(0).at("findings").at(2).at("record");
  EXPECT_EQ(numerical.at("residual").kind, Json::Kind::kString);
  EXPECT_EQ(numerical.at("residual").text, "nan");
  EXPECT_FALSE(numerical.has("difference"));
  EXPECT_FALSE(numerical.has("side"));
  const Json& exact = doc.at("workloads").at(0).at("findings").at(1).at("record");
  EXPECT_EQ(exact.at("difference").kind, Json::Kind::kNull);
  EXPECT_EQ(exact.at("bit_class").kind, Json::Kind::kNull);
}

TEST(JsonWriterTest, TheStreamIsCheckedBeforeAndAfter) {
  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  const auto before = JsonWriter{}.write(rich_run(), bad);
  ASSERT_FALSE(before.has_value());
  EXPECT_NE(before.error().detail.find("not writable"), std::string::npos);
  EXPECT_TRUE(bad.str().empty());
  contracts::results_suite::RefusingBuffer refusing;
  std::ostream full(&refusing);
  const auto after = JsonWriter{}.write(rich_run(), full);
  ASSERT_FALSE(after.has_value());
  EXPECT_NE(after.error().detail.find("refused"), std::string::npos);
  EXPECT_NE(after.error().detail.find("json"), std::string::npos);
}

TEST(JsonWriterTest, TheDocumentEndsWithOneNewline) {
  const std::string text = written(bare_run());
  ASSERT_GE(text.size(), 2U);
  EXPECT_EQ(text.substr(text.size() - 2), "}\n");
  EXPECT_NO_THROW((void)JsonReader::parse(text));
}

}  // namespace
}  // namespace loadforge::reporters

namespace loadforge::contracts {
// writer_contract.hpp includes results_contract.hpp, which registers VerdictContract: a
// binary that runs only the writer suite must say it means to leave that one out.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VerdictContract);
INSTANTIATE_TYPED_TEST_SUITE_P(Json, ResultWriterContract,
                               ::testing::Types<reporters::JsonSubject>);
}  // namespace loadforge::contracts
