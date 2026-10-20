// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/results/document.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <limits>
#include <string>

#include "unit/controller/results/rich_run.hpp"

namespace loadforge::results {
namespace {

using testing::bare_run;
using testing::rich_run;

const Node& member(const Node& node, const char* key) {
  const Node* found = node.find(key);
  EXPECT_NE(found, nullptr) << key;
  return *found;
}

TEST(NodeTest, EachKindHoldsItsValue) {
  EXPECT_EQ(Node::null().kind(), NodeKind::kNull);
  EXPECT_EQ(Node{}.kind(), NodeKind::kNull);
  EXPECT_TRUE(Node::boolean(true).as_bool());
  EXPECT_EQ(Node::boolean(false).kind(), NodeKind::kBool);
  EXPECT_EQ(Node::integer(-5).as_integer(), -5);
  EXPECT_EQ(Node::integer(-5).kind(), NodeKind::kInt);
  EXPECT_EQ(Node::unsigned_integer(18446744073709551615ULL).as_unsigned(), 18446744073709551615ULL);
  EXPECT_EQ(Node::unsigned_integer(1).kind(), NodeKind::kUnsigned);
  EXPECT_EQ(Node::real(0.1).as_real(), 0.1);
  EXPECT_EQ(Node::real(0.1).kind(), NodeKind::kReal);
  EXPECT_EQ(Node::text("x").as_text(), "x");
  EXPECT_EQ(Node::text("x").kind(), NodeKind::kText);
  EXPECT_EQ(Node::array().kind(), NodeKind::kArray);
  EXPECT_EQ(Node::object().kind(), NodeKind::kObject);
  const Node absent = Node::absence({"denied", "w", "/p", "d", "r"});
  EXPECT_EQ(absent.kind(), NodeKind::kAbsence);
  EXPECT_EQ(absent.as_absence().remediation, "r");
  EXPECT_EQ(absent.as_absence().kind, "denied");
  EXPECT_EQ(absent.as_absence().path, "/p");
  EXPECT_EQ(absent.as_absence().detail, "d");
}

TEST(NodeTest, ANumberThatIsNotOneIsTextNotNullAndNotZero) {
  EXPECT_EQ(Node::real(std::numeric_limits<double>::quiet_NaN()).as_text(), "nan");
  EXPECT_EQ(Node::real(std::numeric_limits<double>::infinity()).as_text(), "inf");
  EXPECT_EQ(Node::real(-std::numeric_limits<double>::infinity()).as_text(), "-inf");
  EXPECT_EQ(Node::real(-0.0).kind(), NodeKind::kReal);
  EXPECT_EQ(Node::real(std::numeric_limits<double>::max()).kind(), NodeKind::kReal);
}

TEST(NodeTest, MembersAndItemsKeepTheirOrderAndAreFound) {
  Node object = Node::object();
  object.add("b", Node::integer(1)).add("a", Node::integer(2));
  ASSERT_EQ(object.as_object().keys.size(), 2U);
  EXPECT_EQ(object.as_object().keys[0], "b");
  EXPECT_EQ(object.as_object().values[1].as_integer(), 2);
  ASSERT_NE(object.find("a"), nullptr);
  EXPECT_EQ(object.find("a")->as_integer(), 2);
  EXPECT_EQ(object.find("missing"), nullptr);
  Node array = Node::array();
  array.push(Node::text("x")).push(Node::null());
  EXPECT_EQ(array.as_array().items.size(), 2U);
  EXPECT_EQ(array.as_array().items[1].kind(), NodeKind::kNull);
}

TEST(NodeTest, AskingForTheWrongKindThrowsRatherThanGuessing) {
  EXPECT_THROW((void)Node::null().as_bool(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_integer(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_unsigned(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_real(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_text(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_array(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_object(), std::bad_variant_access);
  EXPECT_THROW((void)Node::boolean(true).as_absence(), std::bad_variant_access);
  Node scalar = Node::boolean(true);
  EXPECT_THROW(scalar.add("k", Node::null()), std::bad_variant_access);
  EXPECT_THROW(scalar.push(Node::null()), std::bad_variant_access);
  EXPECT_THROW((void)scalar.find("k"), std::bad_variant_access);
}

TEST(FormatRealTest, TheShortestDecimalThatReadsBack) {
  EXPECT_EQ(format_real(0.1), "0.1");
  EXPECT_EQ(format_real(2.0), "2");
  EXPECT_EQ(format_real(-0.0), "-0");
  EXPECT_EQ(format_real(1e20), "1e+20");
  EXPECT_EQ(format_real(60.25), "60.25");
  for (const double value : {0.1, 1.0 / 3.0, 1e-300, 123456789.123456789, 5e-324}) {
    const std::string text = format_real(value);
    double back = 0.0;
    std::from_chars(text.data(), text.data() + text.size(), back);
    EXPECT_EQ(back, value) << text;
  }
}

TEST(AvailabilityCodeTest, EachStateHasACodeAndAnUnknownOneIsSaid) {
  using topology::Availability;
  EXPECT_EQ(availability_code(Availability::kPresent), "present");
  EXPECT_EQ(availability_code(Availability::kAbsent), "absent");
  EXPECT_EQ(availability_code(Availability::kVanished), "vanished");
  EXPECT_EQ(availability_code(Availability::kDenied), "denied");
  EXPECT_EQ(availability_code(Availability::kMalformed), "malformed");
  EXPECT_EQ(availability_code(Availability::kUnreadable), "unreadable");
  EXPECT_EQ(availability_code(static_cast<Availability>(200)), "unrecognised");
}

TEST(VerdictCodeTest, EachVerdictHasACodeAndAnUnknownOneIsSaid) {
  EXPECT_EQ(verdict_code(Verdict::kClean), "clean");
  EXPECT_EQ(verdict_code(Verdict::kCorrectedErrors), "corrected_errors");
  EXPECT_EQ(verdict_code(Verdict::kIncomplete), "incomplete");
  EXPECT_EQ(verdict_code(Verdict::kFindings), "findings");
  EXPECT_EQ(verdict_code(static_cast<Verdict>(200)), "unrecognised_verdict");
  EXPECT_EQ(describe(static_cast<Verdict>(200)), "unrecognised verdict");
}

TEST(DocumentTest, TheSchemaVersionIsTheFirstMember) {
  const Node document = to_document(rich_run());
  EXPECT_EQ(document.as_object().keys.front(), "schema_version");
  EXPECT_EQ(document.as_object().values.front().as_unsigned(), kSchemaVersion);
  const std::vector<std::string> expected = {
      "schema_version", "mode",      "elapsed_ns", "verdict", "fingerprint",
      "capabilities",   "signature", "workloads",  "workers", "observers"};
  EXPECT_EQ(document.as_object().keys, expected);
}

TEST(DocumentTest, EqualResultsGiveEqualTreesAndADifferentOneDiffers) {
  const Node a = to_document(rich_run());
  const Node b = to_document(rich_run());
  EXPECT_EQ(a.as_object().keys, b.as_object().keys);
  RunResult changed = rich_run();
  changed.elapsed_ns += 1;
  EXPECT_NE(member(to_document(changed), "elapsed_ns").as_integer(),
            member(a, "elapsed_ns").as_integer());
}

TEST(DocumentTest, TheVerdictIsStatedAsCodeAndWords) {
  const Node verdict = member(to_document(rich_run()), "verdict");
  EXPECT_EQ(member(verdict, "code").as_text(), "findings");
  EXPECT_EQ(member(verdict, "words").as_text(), "findings");
  RunResult run = contracts::results_suite::clean_run();
  run.verdict = Verdict::kCorrectedErrors;
  const Node corrected = member(to_document(run), "verdict");
  EXPECT_EQ(member(corrected, "code").as_text(), "corrected_errors");
  EXPECT_EQ(member(corrected, "words").as_text(), "corrected errors");
}

TEST(DocumentTest, TheFingerprintNamesWhatIsAbsent) {
  const Node full = member(to_document(rich_run()), "fingerprint");
  EXPECT_TRUE(member(full, "absent_fields").as_array().items.empty());
  EXPECT_EQ(member(full, "cpu_model").as_text(), "Example CPU, \"quoted\"");
  EXPECT_EQ(member(member(full, "fp_environment"), "default_nan").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(full, "fp_environment"), "denormals_are_zero").as_bool(), false);
  EXPECT_EQ(member(full, "isa_path").as_text(), "avx2");
  EXPECT_EQ(member(full, "temperature_at_start_c").as_real(), 47.5);

  const Node bare = member(to_document(bare_run()), "fingerprint");
  std::vector<std::string> absent;
  for (const Node& name : member(bare, "absent_fields").as_array().items) {
    absent.push_back(name.as_text());
  }
  const std::vector<std::string> expected = {"git_commit",
                                             "cpu_model",
                                             "microcode",
                                             "kernel",
                                             "smt_enabled",
                                             "power_source",
                                             "battery_state",
                                             "cpufreq_governor",
                                             "energy_performance_preference",
                                             "platform_profile",
                                             "turbo_enabled",
                                             "cooling_profile",
                                             "cgroup_memory_limit_bytes",
                                             "huge_page_policy",
                                             "temperature_at_start_c",
                                             "fp_environment",
                                             "isa_path"};
  EXPECT_EQ(absent, expected);
  for (const std::string& name : absent) {
    EXPECT_EQ(member(bare, name.c_str()).kind(), NodeKind::kNull) << name << " is null, never zero";
  }
}

TEST(DocumentTest, AnUnavailableReadingKeepsItsKindPathDetailAndRemediation) {
  const Node document = to_document(contracts::results_suite::clean_run());
  const Node& sources = member(document, "capabilities");
  const Node& absent =
      member(sources.as_array().items[0].as_object().values[1].as_array().items[0], "availability");
  ASSERT_EQ(absent.kind(), NodeKind::kAbsence);
  EXPECT_EQ(absent.as_absence().kind, "absent");
  EXPECT_EQ(absent.as_absence().words, "not offered by this kernel");
  EXPECT_EQ(absent.as_absence().remediation, contracts::results_suite::kAbsentRemedy);
  const Node& summary =
      member(member(document, "signature").as_object().values[1].as_array().items[0], "summary");
  ASSERT_EQ(summary.kind(), NodeKind::kAbsence);
  EXPECT_EQ(summary.as_absence().kind, "denied");
  EXPECT_EQ(summary.as_absence().words, "not permitted");
  EXPECT_EQ(summary.as_absence().detail, contracts::results_suite::kDeniedDetail);
  EXPECT_EQ(summary.as_absence().path, "/sys/class/powercap/intel-rapl:0/energy_uj");
}

TEST(DocumentTest, APresentMetricSaysPresent) {
  const Node sources = member(to_document(rich_run()), "capabilities");
  const Node& thermal = sources.as_array().items[0];
  const Node& second = member(thermal, "metrics").as_array().items[1];
  EXPECT_EQ(member(second, "availability").as_text(), "present");
  EXPECT_EQ(member(member(second, "metric"), "scope_index").as_unsigned(), 2U);
  EXPECT_EQ(member(member(second, "metric"), "modulus").kind(), NodeKind::kNull);
  const Node& edac = sources.as_array().items[1];
  const Node& metric = member(member(edac, "metrics").as_array().items[0], "metric");
  EXPECT_EQ(member(metric, "modulus").as_unsigned(), 4294967296ULL);
  EXPECT_EQ(member(metric, "kind").as_text(), "counter");
  EXPECT_EQ(member(member(second, "metric"), "kind").as_text(), "gauge");
}

TEST(DocumentTest, SummariesCarryTheirOwnFields) {
  const Node signature = member(to_document(rich_run()), "signature");
  const auto& metrics = member(signature, "metrics").as_array().items;
  const Node& gauge = member(metrics[2], "summary");
  EXPECT_EQ(member(gauge, "type").as_text(), "gauge");
  EXPECT_EQ(member(gauge, "stddev").as_real(), 20.125);
  EXPECT_EQ(member(gauge, "slope_per_second").as_real(), 0.0625);
  EXPECT_EQ(member(metrics[2], "unavailable_samples").as_unsigned(), 1U);
  const Node& single = member(metrics[3], "summary");
  EXPECT_EQ(member(single, "stddev").kind(), NodeKind::kNull);
  EXPECT_EQ(member(single, "slope_per_second").kind(), NodeKind::kNull);
  const Node& counter = member(metrics[4], "summary");
  EXPECT_EQ(member(counter, "type").as_text(), "counter");
  EXPECT_EQ(member(counter, "delta").as_unsigned(), 17U);
  EXPECT_EQ(member(counter, "rate_per_second").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(signature, "window"), "end_ns").as_integer(), 3'600'000'000'000);
}

TEST(DocumentTest, AWorkloadCarriesItsDescriptorTallyAndDuty) {
  const Node workload = member(to_document(rich_run()), "workloads").as_array().items[0];
  const Node& descriptor = member(workload, "descriptor");
  EXPECT_EQ(member(descriptor, "name").as_text(), "compression");
  EXPECT_EQ(member(descriptor, "determinism").as_text(), "BitExact");
  EXPECT_EQ(member(descriptor, "contract").as_text(), "exact");
  ASSERT_EQ(member(descriptor, "paths").as_array().items.size(), 2U);
  EXPECT_EQ(member(descriptor, "paths").as_array().items[1].as_text(), "avx2");
  EXPECT_EQ(member(workload, "isa").as_text(), "avx2");
  EXPECT_EQ(member(workload, "decomposition").as_unsigned(), 8U);
  EXPECT_EQ(member(member(workload, "duty"), "integrity_period").as_unsigned(), 8U);
  EXPECT_EQ(member(member(workload, "tally"), "oracle_not_performed").as_unsigned(), 1U);
  EXPECT_EQ(member(workload, "logical_bytes").as_unsigned(), 819200U);
}

TEST(DocumentTest, EveryIsaPathIsNamedWhenTheWorkloadHasIt) {
  RunResult run = rich_run();
  run.workloads[0].descriptor.paths = simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2,
                                                   simd::IsaPath::kAvx512, simd::IsaPath::kNeon};
  const Node descriptor =
      member(member(to_document(run), "workloads").as_array().items[0], "descriptor");
  EXPECT_EQ(member(descriptor, "paths").as_array().items.size(), 4U);
  run.workloads[0].descriptor.paths = simd::IsaSet{};
  EXPECT_TRUE(
      member(member(member(to_document(run), "workloads").as_array().items[0], "descriptor"),
             "paths")
          .as_array()
          .items.empty());
}

TEST(DocumentTest, EachRecordKindHasItsOwnFieldsAndNoOthers) {
  const Node findings =
      member(member(to_document(rich_run()), "workloads").as_array().items[0], "findings");
  const auto& items = findings.as_array().items;
  ASSERT_EQ(items.size(), 6U);

  const Node& exact = member(items[0], "record");
  EXPECT_EQ(member(exact, "kind").as_text(), "ExactBitCorruption");
  EXPECT_EQ(member(exact, "side").as_text(), "input");
  EXPECT_EQ(member(exact, "bit_class").as_text(), "single bit");
  const Node& difference = member(exact, "difference");
  EXPECT_EQ(member(difference, "expected_hex").as_text(), "00ff10");
  EXPECT_EQ(member(difference, "actual_hex").as_text(), "01ff10");
  EXPECT_EQ(member(difference, "domain").as_text(), "stored bytes");
  EXPECT_EQ(exact.find("residual"), nullptr);
  const Node& provenance = member(exact, "provenance");
  EXPECT_EQ(member(provenance, "buffer_address").as_unsigned(), 0x7f0000001000ULL);
  EXPECT_EQ(member(provenance, "seed").as_unsigned(), 18446744073709551615ULL);
  EXPECT_EQ(member(provenance, "input_integrity").as_text(), "corrupt during consumption");
  EXPECT_EQ(member(member(provenance, "duty"), "oracle_period").as_unsigned(), 4U);
  EXPECT_EQ(member(provenance, "isa").as_text(), "avx2");

  const Node& bare = member(items[1], "record");
  EXPECT_EQ(member(bare, "difference").kind(), NodeKind::kNull);
  EXPECT_EQ(member(bare, "bit_class").kind(), NodeKind::kNull)
      << "no difference, no classification";
  EXPECT_EQ(member(member(bare, "provenance"), "duty").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(bare, "provenance"), "isa").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(bare, "provenance"), "input_integrity").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(bare, "provenance"), "logical_cpu").kind(), NodeKind::kNull);

  const Node& numerical = member(items[2], "record");
  EXPECT_EQ(member(numerical, "kind").as_text(), "NumericalVerificationFailure");
  EXPECT_EQ(member(numerical, "residual").as_text(), "nan");
  EXPECT_EQ(member(numerical, "condition_number").as_real(), 1e6);
  EXPECT_EQ(member(numerical, "element").as_unsigned(), 42U);
  EXPECT_EQ(numerical.find("side"), nullptr) << "a bounded failure has no bit fields";
  EXPECT_EQ(numerical.find("difference"), nullptr);
  EXPECT_EQ(numerical.find("bit_class"), nullptr);
  EXPECT_EQ(member(member(items[3], "record"), "residual").as_text(), "-inf");
  EXPECT_EQ(member(member(items[3], "record"), "ratio").as_text(), "-inf");
  EXPECT_EQ(member(member(items[3], "record"), "condition_number").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(items[3], "record"), "element").kind(), NodeKind::kNull);
  EXPECT_EQ(member(member(items[4], "record"), "residual").as_text(), "inf");

  const Node& unrun = member(items[5], "record");
  EXPECT_EQ(member(unrun, "kind").as_text(), "VerificationNotPerformed");
  EXPECT_EQ(member(unrun, "blocks_compared").as_unsigned(), 12U);
  EXPECT_EQ(unrun.find("residual"), nullptr);
  EXPECT_EQ(unrun.find("difference"), nullptr);
}

TEST(DocumentTest, AnIsolationReportAndTheTelemetryAtTheTimeAreKept) {
  const Node finding =
      member(member(to_document(rich_run()), "workloads").as_array().items[0], "findings")
          .as_array()
          .items[0];
  const Node& isolation = member(finding, "isolation");
  ASSERT_EQ(member(isolation, "steps").as_array().items.size(), 2U);
  EXPECT_EQ(member(member(isolation, "steps").as_array().items[0], "rung").as_text(), "same core");
  EXPECT_EQ(member(member(isolation, "steps").as_array().items[1], "outcome").as_text(), "clean");
  EXPECT_EQ(member(member(isolation, "steps").as_array().items[1], "cpu").as_unsigned(), 5U);
  EXPECT_EQ(member(member(isolation, "answers"), "other_core").as_text(), "no");
  EXPECT_EQ(member(member(isolation, "answers"), "single_thread").as_text(), "unknown");
  EXPECT_EQ(member(isolation, "confidence").as_text(), "medium");
  EXPECT_NE(member(isolation, "finding").as_text().find("core-local"), std::string::npos);

  const auto& telemetry = member(finding, "telemetry").as_array().items;
  ASSERT_EQ(telemetry.size(), 3U);
  EXPECT_EQ(member(member(telemetry[0], "reading"), "value").as_real(), 81.5);
  EXPECT_EQ(member(member(telemetry[1], "reading"), "raw").as_unsigned(), 7U);
  EXPECT_EQ(member(telemetry[2], "reading").kind(), NodeKind::kAbsence);
  EXPECT_EQ(member(telemetry[2], "reading").as_absence().kind, "vanished");
  EXPECT_EQ(member(telemetry[2], "timestamp_ns").as_integer(), 1'700'000'000);

  const Node document = to_document(rich_run());
  const Node& second =
      member(member(document, "workloads").as_array().items[0], "findings").as_array().items[1];
  EXPECT_EQ(member(second, "isolation").kind(), NodeKind::kNull);
  EXPECT_TRUE(member(second, "telemetry").as_array().items.empty());
}

TEST(DocumentTest, WorkersAndObserversKeepWhatIsAbsent) {
  const Node document = to_document(rich_run());
  const auto& workers = member(document, "workers").as_array().items;
  ASSERT_EQ(workers.size(), 2U);
  EXPECT_EQ(member(workers[0], "cpu").as_unsigned(), 3U);
  EXPECT_EQ(member(workers[0], "fate").as_text(), "completed");
  EXPECT_EQ(member(workers[1], "cpu").kind(), NodeKind::kNull);
  EXPECT_EQ(member(workers[1], "termination").as_text(), "killed by signal 9");
  const Node& observers = member(document, "observers");
  EXPECT_EQ(member(observers, "journal_flush_interval_ns").as_integer(), 1'000'000'000);
  EXPECT_EQ(member(observers, "controller_overhead").as_real(), 0.015625);
  const Node none = member(to_document(contracts::results_suite::clean_run()), "observers");
  EXPECT_EQ(member(none, "journal_flush_interval_ns").kind(), NodeKind::kNull);
  EXPECT_EQ(member(none, "controller_overhead").kind(), NodeKind::kNull);
}

TEST(DocumentTest, EmptyListsStayLists) {
  const Node document = to_document(bare_run());
  EXPECT_TRUE(member(document, "capabilities").as_array().items.empty());
  EXPECT_TRUE(member(member(document, "signature"), "metrics").as_array().items.empty());
  EXPECT_TRUE(member(document, "workers").as_array().items.empty());
  EXPECT_EQ(member(document, "workloads").as_array().items.size(), 1U);
}

}  // namespace
}  // namespace loadforge::results
