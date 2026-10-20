// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference slice runner, run against the runner suite, and the
// definitions src/verification/runner.hpp carries. The input-integrity stream
// writes the real runner, with its working set mapped through
// platform/memory; this one allocates with aligned operator new and exists to
// prove the suite can be passed.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/verification/runner_contract.hpp"
#include "verification/runner.hpp"

namespace loadforge::contracts {
namespace {

using verification::Difference;
using verification::ErrorRecord;
using verification::ExactBitCorruption;
using verification::InputIntegrity;
using verification::Provenance;
using verification::RunnerError;
using verification::RunnerFault;
using verification::RunnerSetup;
using verification::SliceReport;
using verification::SliceRequest;
using verification::Tally;

struct AlignedFree {
  std::size_t alignment;
  void operator()(std::byte* data) const { ::operator delete(data, std::align_val_t{alignment}); }
};
using Aligned = std::unique_ptr<std::byte[], AlignedFree>;

Aligned aligned(std::size_t bytes, std::size_t alignment) {
  return Aligned(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{alignment})),
                 AlignedFree{alignment});
}

std::size_t round_up(std::uint64_t bytes, std::size_t line) {
  return static_cast<std::size_t>((bytes + line - 1) / line * line);
}

std::optional<Difference> compare(std::span<const std::byte> expected,
                                  std::span<const std::byte> actual) {
  std::optional<std::uint64_t> first;
  std::uint64_t last = 0;
  std::uint64_t popcount = 0;
  for (std::size_t at = 0; at < expected.size(); ++at) {
    const auto delta = std::to_integer<unsigned>(expected[at] ^ actual[at]);
    if (delta != 0) {
      first = first.value_or(at);
      last = at;
      popcount += static_cast<std::uint64_t>(std::popcount(delta));
    }
  }
  if (!first.has_value()) {
    return std::nullopt;
  }
  Difference difference{
      verification::Domain::kStored, *first, last, popcount, *first / 8 * 8, {}, {}};
  const std::size_t end = std::min<std::size_t>(expected.size(), difference.window_offset + 64);
  for (std::size_t at = difference.window_offset; at < end; ++at) {
    difference.expected.push_back(expected[at]);
    difference.actual.push_back(actual[at]);
  }
  return difference;
}

class ReferenceRunner final : public verification::SliceRunner {
 public:
  explicit ReferenceRunner(const RunnerSetup& setup)
      : setup_(setup),
        plan_(setup.workload->plan()),
        in_stride_(round_up(plan_.input_bytes, setup.line_bytes)),
        out_stride_(round_up(plan_.output_bytes, setup.line_bytes)),
        inputs_(aligned(in_stride_ * plan_.units, setup.line_bytes)),
        outputs_(aligned(out_stride_ * plan_.units, setup.line_bytes)),
        regenerated_(plan_.input_bytes),
        scratch_(plan_.scratch_bytes * setup.executor->threads()),
        next_(setup.first_index) {}

  core::Result<SliceReport, RunnerError> run_slice(const SliceRequest& request) override {
    if (finished_) {
      return RunnerError{RunnerFault::kFinished, "run_slice after finish"};
    }
    SliceReport report;
    for (std::uint64_t n = 0; n < request.units; ++n) {
      unit(next_++, report);
    }
    report.next_index = next_;
    total_ = total_ + report.tally;
    return report;
  }

  core::Result<SliceReport, RunnerError> finish() override {
    if (finished_) {
      return RunnerError{RunnerFault::kFinished, "finish after finish"};
    }
    finished_ = true;
    SliceReport report;
    report.next_index = next_;
    const std::string name = setup_.workload->descriptor().name;
    if (total_.oracle_blocks == 0) {
      report.records.emplace_back(verification::VerificationNotPerformed{
          base(), "oracle over " + name + " units",
          "no block was compared over " + std::to_string(total_.units_computed) + " units", 0});
    }
    if (total_.integrity_checked == 0) {
      report.records.emplace_back(verification::VerificationNotPerformed{
          base(), "input-integrity bracket over " + name + " units", "no unit was bracketed", 0});
    }
    return report;
  }

  [[nodiscard]] const Tally& total() const noexcept override { return total_; }

 private:
  [[nodiscard]] Provenance base() const {
    Provenance provenance = setup_.base;
    provenance.duty = setup_.duty;
    return provenance;
  }

  [[nodiscard]] Provenance at(std::uint64_t index, const std::byte* buffer, std::size_t offset,
                              InputIntegrity integrity) const {
    Provenance provenance = base();
    provenance.iteration = index;
    provenance.buffer_address = reinterpret_cast<std::uintptr_t>(buffer);
    provenance.buffer_offset = offset;
    provenance.input_integrity = integrity;
    return provenance;
  }

  void unit(std::uint64_t index, SliceReport& report) {
    const std::size_t slot = static_cast<std::size_t>(index % plan_.units);
    const std::span<std::byte> input(inputs_.get() + slot * in_stride_, plan_.input_bytes);
    const std::span<std::byte> output(outputs_.get() + slot * out_stride_, plan_.output_bytes);
    const bool bracketed =
        setup_.duty.integrity_period != 0 && index % setup_.duty.integrity_period == 0;
    const bool sampled = setup_.duty.oracle_period != 0 && index % setup_.duty.oracle_period == 0;
    const auto input_record = [&](const Difference& difference, InputIntegrity integrity) {
      return ExactBitCorruption{at(index, input.data(), slot * in_stride_, integrity),
                                verification::Side::kInput, "input against its regeneration",
                                difference, setup_.line_bytes};
    };

    setup_.workload->generate(index, input);
    InputIntegrity integrity = InputIntegrity::kNotChecked;
    if (bracketed) {
      ++report.tally.integrity_checked;
      setup_.workload->generate(index, regenerated_);
      if (auto difference = compare(regenerated_, input)) {
        ++report.tally.integrity_failed;
        report.records.emplace_back(
            input_record(*difference, InputIntegrity::kCorruptBeforeConsumption));
        return;
      }
    }
    setup_.workload->compute(index, input, output, scratch_, *setup_.executor);
    ++report.tally.units_computed;

    std::optional<ErrorRecord> checked;
    if (sampled) {
      ++report.tally.oracle_checked;
      const verification::Verdict verdict =
          setup_.oracle->check(verification::UnitEvidence{index, input, output});
      report.tally.oracle_blocks += verification::blocks_of(verdict);
      const Provenance where = at(index, output.data(), slot * out_stride_, integrity);
      if (const auto* exact = std::get_if<verification::ExactMismatch>(&verdict)) {
        ++report.tally.oracle_failed;
        checked = ExactBitCorruption{where, verification::Side::kOutput, exact->check,
                                     exact->difference, setup_.line_bytes};
      } else if (const auto* bounded = std::get_if<verification::BoundedMismatch>(&verdict)) {
        ++report.tally.oracle_failed;
        checked = verification::NumericalVerificationFailure{where,
                                                             bounded->check,
                                                             bounded->residual,
                                                             bounded->tolerance,
                                                             bounded->condition_number,
                                                             bounded->element};
      } else if (const auto* unchecked = std::get_if<verification::NotChecked>(&verdict)) {
        ++report.tally.oracle_not_performed;
        checked = verification::VerificationNotPerformed{where, unchecked->what, unchecked->why,
                                                         unchecked->blocks_compared};
      }
    }

    std::optional<ErrorRecord> closed;
    if (bracketed) {
      setup_.workload->generate(index, regenerated_);
      if (auto difference = compare(regenerated_, input)) {
        ++report.tally.integrity_failed;
        integrity = InputIntegrity::kCorruptDuringConsumption;
        closed = input_record(*difference, integrity);
      } else {
        integrity = InputIntegrity::kIntact;
      }
    }
    if (checked.has_value()) {
      std::visit([integrity](auto& record) { record.provenance.input_integrity = integrity; },
                 *checked);
      report.records.push_back(*checked);
    }
    if (closed.has_value()) {
      report.records.push_back(*closed);
    }
  }

  RunnerSetup setup_;
  workloads::Plan plan_;
  std::size_t in_stride_;
  std::size_t out_stride_;
  Aligned inputs_;
  Aligned outputs_;
  std::vector<std::byte> regenerated_;
  std::vector<std::byte> scratch_;
  std::uint64_t next_;
  Tally total_;
  bool finished_ = false;
};

struct ReferenceSliceRunner {
  static core::Result<std::unique_ptr<verification::SliceRunner>, RunnerError> make(
      const RunnerSetup& setup) {
    if (setup.workload == nullptr || setup.oracle == nullptr || setup.executor == nullptr ||
        setup.syscalls == nullptr) {
      return RunnerError{RunnerFault::kConfiguration, "a borrowed part is missing"};
    }
    if (setup.oracle->workload() != setup.workload->descriptor().name) {
      return RunnerError{RunnerFault::kConfiguration, "the oracle checks another workload"};
    }
    if (setup.line_bytes < 8 || !std::has_single_bit(setup.line_bytes)) {
      return RunnerError{RunnerFault::kConfiguration, "line size is not a power of two >= 8"};
    }
    return std::unique_ptr<verification::SliceRunner>(std::make_unique<ReferenceRunner>(setup));
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, SliceRunnerContract,
                               ::testing::Types<ReferenceSliceRunner>);

TEST(RunnerDefinition, TalliesAddFieldByFieldAndCompareMemberByMember) {
  const Tally one{1, 2, 3, 4, 5, 6, 7};
  const Tally ten{10, 20, 30, 40, 50, 60, 70};
  EXPECT_EQ(one + ten, (Tally{11, 22, 33, 44, 55, 66, 77}));
  expect_memberwise_equality(
      one, {[](Tally& t) { t.units_computed = 0; }, [](Tally& t) { t.oracle_checked = 0; },
            [](Tally& t) { t.oracle_blocks = 0; }, [](Tally& t) { t.oracle_failed = 0; },
            [](Tally& t) { t.oracle_not_performed = 0; }, [](Tally& t) { t.integrity_checked = 0; },
            [](Tally& t) { t.integrity_failed = 0; }});
}

TEST(RunnerDefinition, FaultNamesAndErrorEquality) {
  EXPECT_EQ(verification::describe(RunnerFault::kConfiguration), "runner configuration");
  EXPECT_EQ(verification::describe(RunnerFault::kResources), "runner resources");
  EXPECT_EQ(verification::describe(RunnerFault::kFinished), "runner already finished");
  EXPECT_EQ(verification::describe(static_cast<RunnerFault>(9)), "unrecognised runner fault");
  expect_memberwise_equality(RunnerError{RunnerFault::kResources, "d"},
                             {[](RunnerError& e) { e.fault = RunnerFault::kFinished; },
                              [](RunnerError& e) { e.detail = "e"; }});
}

}  // namespace
}  // namespace loadforge::contracts
