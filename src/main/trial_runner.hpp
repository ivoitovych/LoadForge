// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_TRIAL_RUNNER_HPP
#define LOADFORGE_MAIN_TRIAL_RUNNER_HPP

// The provider of explore::TrialRunner over the controller (docs/PLAN.md §4.4,
// IMPLEMENTATION M6): one candidate, run as a controlled experiment.
//
// WHAT MAKES A TRIAL A CONTROLLED EXPERIMENT
// ------------------------------------------
//   * a controlled starting state: when the candidate asks for a settle phase,
//     nothing runs until the settle metric's least-squares slope over the last
//     window is within the bound, or the phase has used its time. The phase is
//     the cool-down between trials, because the previous trial's workers are
//     gone before it begins;
//   * fixed affinity: the candidate's CPUs are what its workers are placed on
//     (round robin, one per worker), and a CPU the process may not use refuses
//     the candidate before anything starts;
//   * the same seed on every trial, so two replicates run the same units and
//     what differs between them is the machine;
//   * one verification duty cycle, the candidate's, held by explore across a
//     search (F17);
//   * its own telemetry rig per trial: sources are probed once per rig, a
//     timeline holds one trial and is dropped with it, so a search of hours
//     does not grow without bound.
//
// THE LOAD WINDOW
// ---------------
// The controller marks the phase "stop" on its timeline at the instant the
// duration ends, and takes a round there. The signature's window is
// [stop + 1 - duration_ns, stop + 1): exactly duration_ns wide, and holding the
// round on the deadline (statistics::kClosingMargin says why a window closes one
// nanosecond past a reading it must hold). The configured duration is whole
// milliseconds, so the controller's own run lasts the next whole millisecond at
// most beyond the window.
//
// A run that ended before its deadline (no "stop" mark) is windowed by its last
// round instead, and its verdict says it did not finish.
//
// A WORKER THAT COULD NOT EXEC
// ----------------------------
// Controller::run returns in the forked child when a worker's exec fails
// (orchestration.hpp). The trial then answers kRun, and worker_exit_code() holds
// the code the process must exit with; the caller exits at once.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "core/result.hpp"
#include "explore/trial.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "controller/stop_signals.hpp"
#include "primitives/simd/dispatch.hpp"
#include "resources/cgroup_oom_record.hpp"
#include "statistics/signature.hpp"
#include "statistics/summarizer.hpp"
#include "telemetry/source.hpp"
#include "topology/snapshot.hpp"
#include "verification/runner.hpp"
#include "workloads/workload.hpp"

namespace loadforge::cli {

/// What a trial runner is given beyond the machine and the registry.
///
///   executable       the worker binary: this one, resolved from /proc/self/exe.
///   roots            where the machine's files are.
///   sources          how the telemetry sources are made.
///   summarizer       what summarises the timeline; null: the real one.
///   isa              the ISA path asked for; none: the best the machine and
///                    each workload offer.
///   seed, footprint_bytes, lock   as for a stress run (StressOptions).
///   sample_interval_ns   how often the sampler reads every source in the load.
///   log              where a line per trial goes; null: nowhere. Not owned.
///   stop_signals     the signals that end a trial and the search; null: none.
struct TrialOptions {
  std::string executable;
  MachineRoots roots;
  SourceFactory sources = make_sources;
  const statistics::Summarizer* summarizer = nullptr;
  std::optional<simd::IsaPath> isa;
  std::uint64_t seed = 1;
  std::uint64_t footprint_bytes = std::uint64_t{32} << 20U;
  verification::LockPolicy lock = verification::LockPolicy::kTry;
  std::int64_t sample_interval_ns = 1'000'000'000;
  /// The temperature ceiling of every trial, in degrees Celsius. A ceiling
  /// that no sensor can be read for refuses the runner (create); a trial that
  /// reaches it fails (run), because a run cut short is not a measurement.
  std::optional<std::int64_t> max_temperature_c;
  std::ostream* log = nullptr;
  /// The signals that ask for the end (controller/stop_signals.hpp), installed
  /// by the caller and kept for the search's own look between trials. Every
  /// trial's run is given it, and the settle wait ends on it: a trial a signal
  /// ended fails (kRun), as one the ceiling ended does, and has no score.
  /// Not owned; it must outlive the runner. Null: no signal is caught.
  controller::StopSignals* stop_signals = nullptr;
};

class ControllerTrialRunner final : public explore::TrialRunner {
 public:
  /// Probes the telemetry once, for capabilities(), and reads the CPUs the
  /// process may use. `syscalls`, `machine` and `registry` must
  /// outlive the runner. kRun when the telemetry cannot be set up.
  [[nodiscard]] static core::Result<std::unique_ptr<ControllerTrialRunner>, explore::TrialError>
  create(platform::Syscalls& syscalls, const topology::Snapshot& machine,
         const workloads::Registry& registry, TrialOptions options);

  /// Use create(): a runner without a probed report has nothing to say for
  /// capabilities().
  ControllerTrialRunner(platform::Syscalls& syscalls, const topology::Snapshot& machine,
                        const workloads::Registry& registry, TrialOptions options,
                        telemetry::CapabilityReport capabilities);

  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const override;
  [[nodiscard]] core::Result<explore::TrialOutcome, explore::TrialError> run(
      const explore::Candidate& candidate) override;

  /// Set when a trial's run came back in a forked worker whose exec failed:
  /// the exit code this process must end with, at once.
  [[nodiscard]] std::optional<int> worker_exit_code() const noexcept;

  /// How many trials have been started (a refused candidate is not one).
  [[nodiscard]] std::size_t trials_started() const noexcept;

 private:
  struct Settled {
    bool met = false;
    std::int64_t ns = 0;
  };

  /// The settle phase: rounds every `interval_ns` on the rig's sampler until the
  /// metric's slope over the last window is within the bound, or max_ns.
  [[nodiscard]] core::Result<Settled, explore::TrialError> settle(
      Rig& rig, const statistics::Summarizer& summarizer, const explore::Settle& phase,
      std::int64_t interval_ns);

  platform::Syscalls* syscalls_;
  const topology::Snapshot* machine_;
  const workloads::Registry* registry_;
  TrialOptions options_;
  telemetry::CapabilityReport capabilities_;
  platform::FileSystem files_;
  resources::CgroupOomKillRecord oom_record_;
  std::unique_ptr<simd::IsaProbe> isa_probe_;
  statistics::CompensatedSummarizer real_summarizer_;
  std::vector<std::uint32_t> allowed_cpus_;
  std::ostream discard_{nullptr};
  std::optional<int> worker_exit_code_;
  std::size_t trials_ = 0;
};

}  // namespace loadforge::cli

#endif
