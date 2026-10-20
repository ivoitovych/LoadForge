// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_RESULTS_WRITER_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_RESULTS_WRITER_CONTRACT_HPP

// Provider-side suite for src/reporters/writer.hpp (ADR 0003, 0004).
//
// ResultWriterContract<Subject>:
//   Subject::kFormat   the format's name.
//   Subject::make()    std::unique_ptr<reporters::ResultWriter>.
//
// Results come from results_suite::clean_run() (results_contract.hpp).

#include <gtest/gtest.h>

#include <ios>
#include <memory>
#include <sstream>
#include <string>

#include "contracts/results/results_contract.hpp"
#include "reporters/writer.hpp"

namespace loadforge::contracts {

template <typename Subject>
class ResultWriterContract : public ::testing::Test {
 protected:
  static std::string written(const results::RunResult& run) {
    std::ostringstream out;
    const auto wrote = Subject::make()->write(run, out);
    EXPECT_TRUE(wrote.has_value()) << wrote.error().detail;
    return out.str();
  }
};
TYPED_TEST_SUITE_P(ResultWriterContract);

TYPED_TEST_P(ResultWriterContract, ItNamesItsFormat) {
  EXPECT_EQ(TypeParam::make()->format(), TypeParam::kFormat);
}

TYPED_TEST_P(ResultWriterContract, OutputIsDeterministic) {
  const results::RunResult run = results_suite::clean_run();
  EXPECT_EQ(this->written(run), this->written(run));
  results::RunResult longer = run;
  longer.elapsed_ns += 1;
  EXPECT_NE(this->written(longer), this->written(run));
}

TYPED_TEST_P(ResultWriterContract, TheSchemaVersionIsStated) {
  results::RunResult run = results_suite::clean_run();
  run.schema_version = 4242;
  EXPECT_NE(this->written(run).find("4242"), std::string::npos);
}

TYPED_TEST_P(ResultWriterContract, EveryAbsenceKeepsItsReasonAndRemedy) {
  const std::string text = this->written(results_suite::clean_run());
  EXPECT_NE(text.find(results_suite::kAbsentRemedy), std::string::npos);
  EXPECT_NE(text.find(results_suite::kDeniedRemedy), std::string::npos);
  EXPECT_NE(text.find(results_suite::kDeniedDetail), std::string::npos);
  EXPECT_NE(text.find(topology::describe(topology::Availability::kDenied)), std::string::npos);
}

TYPED_TEST_P(ResultWriterContract, EveryFindingIsNamedByItsKind) {
  results::RunResult run = results_suite::clean_run();
  run.workloads[0].findings = {results_suite::finding(results_suite::exact_record()),
                               results_suite::finding(results_suite::numerical_record()),
                               results_suite::finding(results_suite::unrun_record())};
  run.verdict = Verdict::kFindings;
  const std::string text = this->written(run);
  for (const char* kind :
       {"ExactBitCorruption", "NumericalVerificationFailure", "VerificationNotPerformed"}) {
    EXPECT_NE(text.find(kind), std::string::npos) << kind;
  }
  EXPECT_NE(text.find(results::describe(Verdict::kFindings)), std::string::npos);
  EXPECT_NE(text.find("compression"), std::string::npos);
}

TYPED_TEST_P(ResultWriterContract, AStreamThatCannotTakeTheOutputIsAFailure) {
  const results::RunResult run = results_suite::clean_run();
  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  EXPECT_FALSE(TypeParam::make()->write(run, bad).has_value()) << "not good before writing";
  results_suite::RefusingBuffer refusing;
  std::ostream full(&refusing);
  const auto wrote = TypeParam::make()->write(run, full);
  ASSERT_FALSE(wrote.has_value()) << "output that did not arrive is never a success";
  EXPECT_FALSE(wrote.error().detail.empty());
}

REGISTER_TYPED_TEST_SUITE_P(ResultWriterContract, ItNamesItsFormat, OutputIsDeterministic,
                            TheSchemaVersionIsStated, EveryAbsenceKeepsItsReasonAndRemedy,
                            EveryFindingIsNamedByItsKind, AStreamThatCannotTakeTheOutputIsAFailure);

}  // namespace loadforge::contracts

#endif
