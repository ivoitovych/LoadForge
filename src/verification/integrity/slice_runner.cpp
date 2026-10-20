// SPDX-License-Identifier: GPL-3.0-or-later
//
// The provider of verification/runner.hpp: a workload's units run one at a
// time, each inside the input-integrity bracket of verification.md §4a, in a
// working set mapped through platform/memory, with the values verification
// depends on kept in the triplicated control state (control_state.hpp).
//
// UNITS RUN ONE AFTER ANOTHER
// ---------------------------
// The executor is handed to each unit's compute(), which spreads the unit
// over it; the runner itself starts no thread and runs no two units at once.
// So there is no concurrency in this module to argue about (IMPLEMENTATION
// §2.4: prefer not to have the path), and the slot a unit occupies is never
// touched by anything but that unit between its write and its closing check,
// which is the §4a temporal invariant held by construction.
//
// THE CONTROL STATE IS VOTED ONCE PER UNIT
// ----------------------------------------
// At the start of every unit the runner votes, and every step of that unit
// uses the decided values: the index it writes, regenerates, computes, checks
// and regenerates again; the duty cycle that chooses its checks; the seed
// and line size its records carry. After the unit the next index is written
// to all three copies. A vote that decides nothing stops the run: the slice
// reports a VerificationNotPerformed -- the run's failure, never the
// machine's -- and every later run_slice() answers kFinished.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "platform/memory.hpp"
#include "verification/integrity/control_state.hpp"
#include "verification/integrity/difference.hpp"
#include "verification/integrity/layout.hpp"
#include "verification/runner.hpp"

namespace loadforge::verification {
namespace {

using integrity::ControlState;
using integrity::ControlWord;
using integrity::ControlWords;
using integrity::Layout;

/// Text fields are at most kMaxTextBytes (record.hpp); a longer one would
/// make the record invalid and undecodable, so it is cut, not dropped.
std::string bounded(std::string text) {
  if (text.size() > kMaxTextBytes) {
    text.resize(kMaxTextBytes);
  }
  return text;
}

/// A record built member by member from values the caller already made, one
/// allocation per statement (journal §1.13): an aggregate whose members
/// allocate as it is built carries a cleanup for the half-built record that
/// no test can enter.
VerificationNotPerformed not_performed(Provenance where, std::string what, std::string why,
                                       std::uint64_t blocks) noexcept {
  VerificationNotPerformed record;
  record.provenance = std::move(where);
  record.what = std::move(what);
  record.why = std::move(why);
  record.blocks_compared = blocks;
  return record;
}

/// Everything one unit's steps share, decided by one vote.
struct Unit {
  std::uint64_t index = 0;
  DutyCycle duty;
  std::uint32_t line_bytes = 0;
  std::optional<std::uint64_t> seed;
  std::span<std::byte> input;
  std::span<std::byte> output;
  std::size_t input_offset = 0;   ///< Of the input, within the mapping.
  std::size_t output_offset = 0;  ///< Of the output, within the mapping.
};

class IntegrityRunner final : public SliceRunner {
 public:
  IntegrityRunner(const RunnerSetup& setup, const Layout& layout, platform::Mapping working_set,
                  ControlState control)
      : workload_(setup.workload),
        oracle_(setup.oracle),
        executor_(setup.executor),
        base_(setup.base),
        duty_(setup.duty),
        layout_(layout),
        working_set_(std::move(working_set)),
        control_(std::move(control)),
        next_index_(setup.first_index) {
    // The workload's name is the setup's, and a record cannot carry one
    // longer than kMaxTextBytes; it is cut here once rather than in every
    // record.
    if (base_.workload.has_value()) {
      base_.workload = bounded(std::move(*base_.workload));
    }
    // Line-aligned inside the one line of slack the layout leaves.
    const auto start = std::bit_cast<std::uintptr_t>(working_set_.address());
    const std::uintptr_t aligned = (start + layout_.line_bytes - 1) & ~(layout_.line_bytes - 1);
    slack_ = aligned - start;
  }

  core::Result<SliceReport, RunnerError> run_slice(const SliceRequest& request) override {
    if (finished_) {
      return RunnerError{RunnerFault::kFinished, "run_slice() after finish()"};
    }
    if (halted_) {
      return RunnerError{RunnerFault::kFinished, halt_detail_};
    }
    SliceReport report;
    for (std::uint64_t done = 0; done < request.units; ++done) {
      if (!run_unit(report)) {
        break;
      }
    }
    report.next_index = next_index_;
    total_ = total_ + report.tally;
    return report;
  }

  core::Result<SliceReport, RunnerError> finish() override {
    if (finished_) {
      return RunnerError{RunnerFault::kFinished, "finish() after finish()"};
    }
    finished_ = true;
    SliceReport report;
    report.next_index = next_index_;
    const std::string& name = workload_->descriptor().name;
    if (total_.oracle_blocks == 0) {
      Provenance where = run_provenance();
      std::string what = bounded("oracle over " + name + " units");
      std::string why = bounded("no block was compared: an oracle period of " +
                                std::to_string(duty_.oracle_period) + " over " +
                                std::to_string(total_.units_computed) + " units computed gave " +
                                std::to_string(total_.oracle_checked) + " to the oracle");
      report.records.emplace_back(
          not_performed(std::move(where), std::move(what), std::move(why), 0));
    }
    if (total_.integrity_checked == 0) {
      Provenance where = run_provenance();
      std::string what = bounded("input-integrity bracket over " + name + " units");
      std::string why = bounded("no unit was bracketed: an integrity period of " +
                                std::to_string(duty_.integrity_period) + " over " +
                                std::to_string(total_.units_computed) + " units computed");
      report.records.emplace_back(
          not_performed(std::move(where), std::move(what), std::move(why), 0));
    }
    return report;
  }

  [[nodiscard]] const Tally& total() const noexcept override { return total_; }

 private:
  /// The provenance of a finding about the run rather than a unit: no
  /// iteration, no buffer -- absent, because there is none.
  [[nodiscard]] Provenance run_provenance() const {
    Provenance provenance = base_;
    provenance.duty = duty_;
    return provenance;
  }

  /// The provenance of a finding about `unit`, in the buffer at `offset`.
  [[nodiscard]] Provenance at(const Unit& unit, std::size_t offset,
                              InputIntegrity integrity) const {
    Provenance provenance = base_;
    provenance.iteration = unit.index;
    provenance.buffer_address = std::bit_cast<std::uintptr_t>(working_set_.address()) + offset;
    provenance.buffer_offset = offset;
    provenance.seed = unit.seed;
    provenance.input_integrity = integrity;
    provenance.duty = unit.duty;
    return provenance;
  }

  [[nodiscard]] ErrorRecord input_record(const Unit& unit, Difference difference,
                                         InputIntegrity integrity) const {
    ExactBitCorruption record;
    record.provenance = at(unit, unit.input_offset, integrity);
    record.side = Side::kInput;
    record.check = "input against its regeneration";
    record.difference = std::move(difference);
    record.line_bytes = unit.line_bytes;
    return record;
  }

  /// The whole mapping; every region is a subspan of it, at the slack plus
  /// the region's offset in the layout.
  [[nodiscard]] std::span<std::byte> memory() const noexcept {
    return {static_cast<std::byte*>(working_set_.address()), layout_.mapping_bytes};
  }

  /// The decided control state as one unit's working values.
  [[nodiscard]] Unit unit_of(const ControlWords& words) const noexcept {
    Unit unit;
    unit.index = integrity::word(words, ControlWord::kIndex);
    unit.duty.oracle_period =
        static_cast<std::uint32_t>(integrity::word(words, ControlWord::kOraclePeriod));
    unit.duty.integrity_period =
        static_cast<std::uint32_t>(integrity::word(words, ControlWord::kIntegrityPeriod));
    unit.line_bytes = static_cast<std::uint32_t>(integrity::word(words, ControlWord::kLineBytes));
    if (integrity::word(words, ControlWord::kSeedPresent) != 0) {
      unit.seed = integrity::word(words, ControlWord::kSeed);
    }
    const std::size_t slot = unit.index % layout_.units;
    unit.input_offset = slack_ + slot * layout_.input_stride;
    unit.output_offset = slack_ + layout_.outputs + slot * layout_.output_stride;
    unit.input = memory().subspan(unit.input_offset, layout_.input_bytes);
    unit.output = memory().subspan(unit.output_offset, layout_.output_bytes);
    return unit;
  }

  /// Runs one unit. False when the control state could not be decided and
  /// the run has stopped.
  bool run_unit(SliceReport& report) {
    auto voted = control_.read();
    if (!voted.has_value()) {
      halt(report, voted.error());
      return false;
    }
    ControlWords words = voted.value();
    const Unit unit = unit_of(words);
    const bool bracketed =
        unit.duty.integrity_period != 0 && unit.index % unit.duty.integrity_period == 0;
    const bool sampled = unit.duty.oracle_period != 0 && unit.index % unit.duty.oracle_period == 0;
    const std::span<std::byte> regenerated =
        memory().subspan(slack_ + layout_.regenerated, layout_.input_bytes);
    const std::span<std::byte> scratch =
        memory().subspan(slack_ + layout_.scratch, layout_.scratch_bytes);

    // 1. write.
    workload_->generate(unit.index, unit.input);

    // 2. open.
    InputIntegrity integrity = InputIntegrity::kNotChecked;
    if (bracketed) {
      ++report.tally.integrity_checked;
      workload_->generate(unit.index, regenerated);
      if (auto difference = integrity::compare_stored(regenerated, unit.input)) {
        ++report.tally.integrity_failed;
        report.records.push_back(
            input_record(unit, std::move(*difference), InputIntegrity::kCorruptBeforeConsumption));
        advance(words, unit.index);
        return true;
      }
    }

    // 3. compute.
    workload_->compute(unit.index, unit.input, unit.output, scratch, *executor_);
    ++report.tally.units_computed;

    // 4. check.
    std::optional<ErrorRecord> checked;
    if (sampled) {
      checked = check(unit, report.tally);
    }

    // 5. close.
    std::optional<ErrorRecord> closed;
    if (bracketed) {
      workload_->generate(unit.index, regenerated);
      if (auto difference = integrity::compare_stored(regenerated, unit.input)) {
        ++report.tally.integrity_failed;
        integrity = InputIntegrity::kCorruptDuringConsumption;
        closed = input_record(unit, std::move(*difference), integrity);
      } else {
        integrity = InputIntegrity::kIntact;
      }
    }

    if (checked.has_value()) {
      std::visit([integrity](auto& record) { record.provenance.input_integrity = integrity; },
                 *checked);
      report.records.push_back(std::move(*checked));
    }
    if (closed.has_value()) {
      report.records.push_back(std::move(*closed));
    }
    advance(words, unit.index);
    return true;
  }

  /// Step 4: the oracle's verdict on the unit, as a record when it is one.
  std::optional<ErrorRecord> check(const Unit& unit, Tally& tally) {
    ++tally.oracle_checked;
    const Provenance where = at(unit, unit.output_offset, InputIntegrity::kNotChecked);
    std::string what = bounded("oracle check of " + workload_->descriptor().name + " unit " +
                               std::to_string(unit.index));

    // A tool failure inside a check is a check not made (runner.hpp), and
    // check() is the one call of the oracle's that is allowed to throw.
    Verdict verdict;
    std::optional<std::string> thrown;
    try {
      verdict = oracle_->check(UnitEvidence{unit.index, unit.input, unit.output});
    } catch (const std::exception& error) {
      thrown = std::string("the oracle threw: ") + error.what();
    } catch (...) {
      thrown = "the oracle threw something that is not a std::exception";
    }
    if (thrown.has_value()) {
      ++tally.oracle_not_performed;
      std::string why = bounded(std::move(*thrown));
      return not_performed(where, std::move(what), std::move(why), 0);
    }

    // A verdict that breaks the oracle contract cannot be believed either
    // way: not an agreement that compared nothing, and not a mismatch to
    // send to fault isolation as the hardware's. The blocks it claims are
    // not counted.
    if (!is_valid(verdict, oracle_->contract())) {
      ++tally.oracle_not_performed;
      std::string why = "the oracle's verdict (alternative " + std::to_string(verdict.index()) +
                        ") breaks the oracle contract";
      return not_performed(where, std::move(what), std::move(why), 0);
    }

    tally.oracle_blocks += blocks_of(verdict);
    if (std::holds_alternative<Agreement>(verdict)) {
      return std::nullopt;
    }
    if (auto* exact = std::get_if<ExactMismatch>(&verdict)) {
      ++tally.oracle_failed;
      return ExactBitCorruption{where, Side::kOutput, std::move(exact->check),
                                std::move(exact->difference), unit.line_bytes};
    }
    if (auto* numerical = std::get_if<BoundedMismatch>(&verdict)) {
      ++tally.oracle_failed;
      return NumericalVerificationFailure{where,
                                          std::move(numerical->check),
                                          numerical->residual,
                                          numerical->tolerance,
                                          numerical->condition_number,
                                          numerical->element};
    }
    auto* unchecked = std::get_if<NotChecked>(&verdict);
    ++tally.oracle_not_performed;
    return VerificationNotPerformed{where, std::move(unchecked->what), std::move(unchecked->why),
                                    unchecked->blocks_compared};
  }

  /// The unit is done: the next one is index + 1, in all three copies.
  void advance(ControlWords& words, std::uint64_t index) noexcept {
    integrity::set_word(words, ControlWord::kIndex, index + 1);
    control_.write(words);
    next_index_ = index + 1;
  }

  /// The control state decided nothing: say so, and run no further unit.
  void halt(SliceReport& report, const integrity::VoteFailure& failure) {
    halted_ = true;
    const std::string why = integrity::describe(failure);
    halt_detail_ = "the run stopped before unit " + std::to_string(next_index_) + ": " + why;
    Provenance where = run_provenance();
    std::string what = bounded("control state of the " + workload_->descriptor().name + " run");
    std::string stated = bounded(why + "; the run stops rather than pick a value");
    report.records.emplace_back(
        not_performed(std::move(where), std::move(what), std::move(stated), 0));
  }

  const workloads::Workload* workload_;
  const Oracle* oracle_;
  worker::Executor* executor_;
  Provenance base_;
  DutyCycle duty_;
  Layout layout_;
  platform::Mapping working_set_;
  std::size_t slack_ = 0;
  ControlState control_;
  Tally total_;
  /// The run's progress as last confirmed, for the report's next_index only;
  /// nothing is regenerated from it, so it needs no protection.
  std::uint64_t next_index_;
  bool finished_ = false;
  bool halted_ = false;
  std::string halt_detail_;
};

}  // namespace

core::Result<std::unique_ptr<SliceRunner>, RunnerError> make_slice_runner(
    const RunnerSetup& setup) {
  if (setup.workload == nullptr) {
    return RunnerError{RunnerFault::kConfiguration, "no workload"};
  }
  if (setup.oracle == nullptr) {
    return RunnerError{RunnerFault::kConfiguration, "no oracle"};
  }
  if (setup.executor == nullptr) {
    return RunnerError{RunnerFault::kConfiguration, "no executor"};
  }
  if (setup.syscalls == nullptr) {
    return RunnerError{RunnerFault::kConfiguration, "no system-call seam"};
  }
  const workloads::Descriptor& descriptor = setup.workload->descriptor();
  if (setup.oracle->workload() != descriptor.name) {
    return RunnerError{RunnerFault::kConfiguration,
                       "the oracle checks '" + std::string(setup.oracle->workload()) +
                           "' and the workload is '" + descriptor.name + "'"};
  }
  if (setup.oracle->contract() != descriptor.contract) {
    return RunnerError{RunnerFault::kConfiguration,
                       "the workload is checked by the " +
                           std::string(describe(descriptor.contract)) +
                           " contract and the oracle offers the " +
                           std::string(describe(setup.oracle->contract())) + " one"};
  }
  auto layout =
      integrity::plan_layout(setup.workload->plan(), setup.executor->threads(), setup.line_bytes);
  if (!layout.has_value()) {
    return layout.error();
  }

  platform::MappingRequest request;
  request.bytes = layout.value().mapping_bytes;
  request.populate = true;
  request.lock = true;
  request.node = setup.base.numa_node;
  platform::Memory memory(*setup.syscalls);
  auto working_set = memory.map(request);
  if (!working_set.has_value()) {
    return RunnerError{RunnerFault::kResources,
                       "working set: " + platform::describe(working_set.error())};
  }

  ControlWords initial{};
  integrity::set_word(initial, ControlWord::kSeed, setup.base.seed.value_or(0));
  integrity::set_word(initial, ControlWord::kSeedPresent, setup.base.seed.has_value() ? 1 : 0);
  integrity::set_word(initial, ControlWord::kIndex, setup.first_index);
  integrity::set_word(initial, ControlWord::kOraclePeriod, setup.duty.oracle_period);
  integrity::set_word(initial, ControlWord::kIntegrityPeriod, setup.duty.integrity_period);
  integrity::set_word(initial, ControlWord::kLineBytes, setup.line_bytes);
  std::vector<std::uint32_t> nodes;
  if (setup.base.numa_node.has_value()) {
    nodes.push_back(*setup.base.numa_node);
  }
  auto control = ControlState::create(*setup.syscalls, initial, nodes);
  if (!control.has_value()) {
    return RunnerError{RunnerFault::kResources, "control state, copy " +
                                                    std::to_string(control.error().copy) + ": " +
                                                    platform::describe(control.error().refused)};
  }

  return std::unique_ptr<SliceRunner>(std::make_unique<IntegrityRunner>(
      setup, layout.value(), std::move(working_set).value(), std::move(control).value()));
}

}  // namespace loadforge::verification
