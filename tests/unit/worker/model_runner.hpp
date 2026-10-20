// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKER_MODEL_RUNNER_HPP
#define LOADFORGE_TESTS_UNIT_WORKER_MODEL_RUNNER_HPP

// The worker's own model of verification/runner.hpp, written from the contract
// and from nothing else: the five steps of one unit, the duty cycle's rule, the
// tally, the records in step order, finish()'s two statements of what the run
// did not check. Not the input-integrity stream's runner and not a copy of its
// tests (ADR 0003): this is what the worker's consumer tests run against, and
// the provider's suite is run against THIS (model_runner_test.cpp), so that a
// disagreement between the two readings of the contract is a failing test.
//
// It keeps its working set in ordinary vectors (no mapping, no locking, no
// control-state vote), because those are the provider's business and a
// consumer's model has none of it.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker_testing {

using verification::DutyCycle;
using verification::ErrorRecord;
using verification::Tally;

class ModelRunner final : public verification::SliceRunner {
 public:
  static core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError> make(
      const verification::RunnerSetup& setup) {
    using verification::RunnerFault;
    if (setup.workload == nullptr || setup.oracle == nullptr || setup.executor == nullptr ||
        setup.syscalls == nullptr) {
      return verification::RunnerError{RunnerFault::kConfiguration, "a missing part"};
    }
    if (setup.oracle->workload() != setup.workload->descriptor().name ||
        setup.oracle->contract() != setup.workload->descriptor().contract) {
      return verification::RunnerError{RunnerFault::kConfiguration, "an oracle for another"};
    }
    if (setup.line_bytes < verification::kWordBytes || !std::has_single_bit(setup.line_bytes)) {
      return verification::RunnerError{RunnerFault::kConfiguration, "a line size"};
    }
    std::unique_ptr<verification::SliceRunner> runner = std::make_unique<ModelRunner>(setup);
    return runner;
  }

  explicit ModelRunner(const verification::RunnerSetup& setup)
      : setup_(setup), next_(setup.first_index) {
    const workloads::Plan& plan = setup.workload->plan();
    stride_ = (plan.input_bytes + plan.output_bytes + 2 * setup.line_bytes + setup.line_bytes - 1) /
              setup.line_bytes * setup.line_bytes;
    store_.assign(stride_ * plan.units + setup.line_bytes, std::byte{0});
    scratch_.assign(plan.scratch_bytes * setup.executor->threads(), std::byte{0});
  }

  core::Result<verification::SliceReport, verification::RunnerError> run_slice(
      const verification::SliceRequest& request) override {
    if (finished_) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "finished"};
    }
    verification::SliceReport report;
    for (std::uint64_t done = 0; done < request.units; ++done) {
      unit(report);
    }
    report.next_index = next_;
    total_ = total_ + report.tally;
    return report;
  }

  core::Result<verification::SliceReport, verification::RunnerError> finish() override {
    if (finished_) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "finished"};
    }
    finished_ = true;
    verification::SliceReport report;
    const std::string name = setup_.workload->descriptor().name;
    if (total_.oracle_blocks == 0) {
      report.records.emplace_back(not_performed(
          setup_.base, "oracle over " + name + " units", "no block compared", 0));
    }
    if (total_.integrity_checked == 0) {
      report.records.emplace_back(
          not_performed(setup_.base, "input-integrity bracket over " + name + " units",
                        "no unit bracketed", 0));
    }
    report.next_index = next_;
    return report;
  }

  [[nodiscard]] const Tally& total() const noexcept override { return total_; }

 private:
  static verification::VerificationNotPerformed not_performed(verification::Provenance where,
                                                              std::string what, std::string why,
                                                              std::uint64_t blocks) {
    verification::VerificationNotPerformed record;
    record.provenance = std::move(where);
    record.what = std::move(what);
    record.why = std::move(why);
    record.blocks_compared = blocks;
    return record;
  }

  [[nodiscard]] std::byte* aligned_base() {
    const auto start = reinterpret_cast<std::uintptr_t>(store_.data());
    const std::uintptr_t line = setup_.line_bytes;
    return store_.data() + ((start + line - 1) / line * line - start);
  }

  static verification::Difference difference_of(std::span<const std::byte> expected,
                                                std::span<const std::byte> actual) {
    verification::Difference difference;
    bool any = false;
    for (std::size_t at = 0; at < expected.size(); ++at) {
      const unsigned delta = std::to_integer<unsigned>(expected[at] ^ actual[at]);
      if (delta == 0) {
        continue;
      }
      if (!any) {
        difference.first_offset = at;
        any = true;
      }
      difference.last_offset = at;
      difference.popcount += static_cast<std::uint64_t>(std::popcount(delta));
    }
    difference.window_offset = difference.first_offset / 8 * 8;
    const std::size_t end = std::min<std::size_t>(expected.size(),
                                                  difference.window_offset + verification::kMaxWindowBytes);
    difference.expected.assign(expected.begin() + static_cast<std::ptrdiff_t>(difference.window_offset),
                               expected.begin() + static_cast<std::ptrdiff_t>(end));
    difference.actual.assign(actual.begin() + static_cast<std::ptrdiff_t>(difference.window_offset),
                             actual.begin() + static_cast<std::ptrdiff_t>(end));
    return difference;
  }

  void unit(verification::SliceReport& report) {
    const workloads::Plan& plan = setup_.workload->plan();
    const std::uint64_t k = next_++;
    const std::uint64_t slot = k % plan.units;
    std::byte* base = aligned_base() + slot * stride_;
    const std::span<std::byte> input{base, plan.input_bytes};
    const std::size_t output_at = (plan.input_bytes + setup_.line_bytes - 1) / setup_.line_bytes *
                                  setup_.line_bytes;
    const std::span<std::byte> output{base + output_at, plan.output_bytes};
    const bool sampled = setup_.duty.oracle_period != 0 && k % setup_.duty.oracle_period == 0;
    const bool bracketed =
        setup_.duty.integrity_period != 0 && k % setup_.duty.integrity_period == 0;

    verification::Provenance where = setup_.base;
    where.iteration = k;
    where.duty = setup_.duty;
    where.buffer_offset = slot * stride_;
    const auto address_of = [](std::span<std::byte> bytes) {
      return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes.data()));
    };

    std::vector<ErrorRecord> made;
    verification::InputIntegrity integrity = bracketed ? verification::InputIntegrity::kIntact
                                                       : verification::InputIntegrity::kNotChecked;
    const auto input_record = [&](const std::vector<std::byte>& regenerated, const char* when) {
      verification::ExactBitCorruption record;
      record.provenance = where;
      record.provenance.buffer_address = address_of(input);
      record.side = verification::Side::kInput;
      record.check = std::string{"input against its regeneration, "} + when;
      record.difference = difference_of(regenerated, input);
      record.line_bytes = setup_.line_bytes;
      return record;
    };

    setup_.workload->generate(k, input);
    std::vector<std::byte> again(plan.input_bytes);
    if (bracketed) {
      ++report.tally.integrity_checked;
      setup_.workload->generate(k, again);
      if (!std::equal(again.begin(), again.end(), input.begin())) {
        ++report.tally.integrity_failed;
        auto record = input_record(again, "before consumption");
        record.provenance.input_integrity = verification::InputIntegrity::kCorruptBeforeConsumption;
        report.records.emplace_back(std::move(record));
        return;
      }
    }
    setup_.workload->compute(k, input, output, scratch_, *setup_.executor);
    ++report.tally.units_computed;
    if (sampled) {
      ++report.tally.oracle_checked;
      const verification::Verdict verdict =
          setup_.oracle->check(verification::UnitEvidence{k, input, output});
      report.tally.oracle_blocks += std::visit([](const auto& held) { return held.blocks_compared; }, verdict);
      if (const auto* mismatch = std::get_if<verification::ExactMismatch>(&verdict)) {
        verification::ExactBitCorruption record;
        record.provenance = where;
        record.provenance.buffer_address = address_of(output);
        record.side = verification::Side::kOutput;
        record.check = mismatch->check;
        record.difference = mismatch->difference;
        record.line_bytes = setup_.line_bytes;
        made.emplace_back(std::move(record));
        ++report.tally.oracle_failed;
      } else if (const auto* bounded = std::get_if<verification::BoundedMismatch>(&verdict)) {
        verification::NumericalVerificationFailure record;
        record.provenance = where;
        record.check = bounded->check;
        record.residual = bounded->residual;
        record.tolerance = bounded->tolerance;
        record.condition_number = bounded->condition_number;
        record.element = bounded->element;
        made.emplace_back(std::move(record));
        ++report.tally.oracle_failed;
      } else if (const auto* unchecked = std::get_if<verification::NotChecked>(&verdict)) {
        made.emplace_back(
            not_performed(where, unchecked->what, unchecked->why, unchecked->blocks_compared));
        ++report.tally.oracle_not_performed;
      }
    }
    if (bracketed) {
      setup_.workload->generate(k, again);
      if (!std::equal(again.begin(), again.end(), input.begin())) {
        ++report.tally.integrity_failed;
        integrity = verification::InputIntegrity::kCorruptDuringConsumption;
        made.emplace_back(input_record(again, "after consumption"));
      }
    }
    for (ErrorRecord& record : made) {
      std::visit([&](auto& held) { held.provenance.input_integrity = integrity; }, record);
      report.records.push_back(std::move(record));
    }
  }

  verification::RunnerSetup setup_;
  std::uint64_t next_;
  std::size_t stride_ = 0;
  std::vector<std::byte> store_;
  std::vector<std::byte> scratch_;
  Tally total_;
  bool finished_ = false;
};

}  // namespace loadforge::worker_testing

#endif
