// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ISOLATION_HPP
#define LOADFORGE_VERIFICATION_ISOLATION_HPP

// CONTRACT (ADR 0004). Fault isolation, docs/verification.md §7: the re-run
// ladder that turns "this unit came out wrong" into what that is consistent
// with. Frozen: changed only by a contract change, never inside a stream.
//
//   Provider of Isolator: the isolation stream (src/verification/isolation/).
//   Provider of Rerunner: the integration stream, which can start a worker on
//     a chosen CPU with a chosen thread count and run one unit in it.
//   Consumer of IsolationReport: results (controller/results/result.hpp),
//     which attaches it to the finding beside its record.
//
// ONLY WRONG RESULTS ENTER
// ------------------------
// VerificationNotPerformed is not routed here (§7): a check that could not be
// made has no wrong answer to re-run, and every re-run of it would "pass" and
// land on the most alarming classification there is. The ladder takes a
// WrongResult, a type that cannot hold one, so the rule is kept by the
// compiler rather than by every caller.
//
// THE LADDER
// ----------
//   1. kSameCore      the original CPU, the original thread count.
//                     Clean: a non-reproducible transient; stop.
//                     Not performed: inconclusive; stop.
//   2. kOtherCore     a CPU on another physical core, the original threads.
//                     Skipped when the placement offers no other core.
//   3. kSingleThread  the original CPU, one thread. Skipped when the original
//                     ran one thread already: step 1 was this step.
// Every finding is evidence CONSISTENT WITH something, reported with a
// confidence, never a verdict on the hardware (§7).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "verification/record.hpp"

namespace loadforge::verification {

/// The two record kinds that state a wrong result.
using WrongResult = std::variant<ExactBitCorruption, NumericalVerificationFailure>;

/// A record as a WrongResult, or nothing for a VerificationNotPerformed.
[[nodiscard]] inline std::optional<WrongResult> as_wrong_result(const ErrorRecord& record) {
  if (const auto* exact = std::get_if<ExactBitCorruption>(&record)) {
    return WrongResult{*exact};
  }
  if (const auto* numerical = std::get_if<NumericalVerificationFailure>(&record)) {
    return WrongResult{*numerical};
  }
  return std::nullopt;
}

enum class Rung : std::uint8_t { kSameCore = 0, kOtherCore = 1, kSingleThread = 2 };

[[nodiscard]] constexpr std::string_view describe(Rung rung) noexcept {
  switch (rung) {
    case Rung::kSameCore:
      return "same core";
    case Rung::kOtherCore:
      return "another core";
    case Rung::kSingleThread:
      return "single thread";
  }
  return "unrecognised rung";
}

/// One re-run: which rung, on which logical CPU, with how many threads.
struct RerunRequest {
  Rung rung = Rung::kSameCore;
  std::uint32_t cpu = 0;
  std::uint32_t threads = 1;
};

[[nodiscard]] inline bool operator==(const RerunRequest& lhs, const RerunRequest& rhs) noexcept {
  return std::tie(lhs.rung, lhs.cpu, lhs.threads) == std::tie(rhs.rung, rhs.cpu, rhs.threads);
}

/// What one re-run found.
///
///   kReproduced    the unit came out wrong again: the oracle or the bracket
///                  made a wrong-result record for it.
///   kClean         the unit was computed, checked by the oracle and
///                  bracketed, and nothing was wrong.
///   kNotPerformed  the re-run could not be made or could not check: no worker
///                  could start there, the provenance lacks what the unit
///                  needs (workload, iteration, seed), or the oracle answered
///                  NotChecked. Never a clean result.
enum class RerunOutcome : std::uint8_t { kReproduced, kClean, kNotPerformed };

[[nodiscard]] constexpr std::string_view describe(RerunOutcome outcome) noexcept {
  switch (outcome) {
    case RerunOutcome::kReproduced:
      return "reproduced";
    case RerunOutcome::kClean:
      return "clean";
    case RerunOutcome::kNotPerformed:
      return "not performed";
  }
  return "unrecognised rerun outcome";
}

/// Runs the unit a finding is about again, by the request.
///
/// The unit is the finding's provenance (workload, workload_version,
/// iteration, seed, isa), run in a worker of its own pinned to request.cpu,
/// with an executor of request.threads threads, every check on (oracle and
/// bracket for that unit). Blocks until the re-run is over.
///
/// Thread-safety: one call at a time.
class Rerunner {
 public:
  Rerunner() = default;
  Rerunner(const Rerunner&) = delete;
  Rerunner(Rerunner&&) = delete;
  Rerunner& operator=(const Rerunner&) = delete;
  Rerunner& operator=(Rerunner&&) = delete;
  virtual ~Rerunner() = default;

  [[nodiscard]] virtual RerunOutcome rerun(const WrongResult& finding,
                                           const RerunRequest& request) = 0;
};

/// Where the original unit ran, and where else it may be run.
///
///   cpu          the logical CPU it ran on (its provenance's logical_cpu).
///   threads      the thread count it ran with; at least 1.
///   other_cores  logical CPUs on physical cores other than cpu's, in the
///                order to try them. The ladder uses the first.
struct Placement {
  std::uint32_t cpu = 0;
  std::uint32_t threads = 1;
  std::vector<std::uint32_t> other_cores;
};

enum class Answer : std::uint8_t { kYes, kNo, kUnknown };

[[nodiscard]] constexpr std::string_view describe(Answer answer) noexcept {
  switch (answer) {
    case Answer::kYes:
      return "yes";
    case Answer::kNo:
      return "no";
    case Answer::kUnknown:
      return "unknown";
  }
  return "unrecognised answer";
}

/// The ladder's three answers: did the unit come out wrong again on the same
/// core, on another core, single-threaded.
struct Answers {
  Answer same_core = Answer::kUnknown;
  Answer other_core = Answer::kUnknown;
  Answer single_thread = Answer::kUnknown;
};

[[nodiscard]] inline bool operator==(const Answers& lhs, const Answers& rhs) noexcept {
  return std::tie(lhs.same_core, lhs.other_core, lhs.single_thread) ==
         std::tie(rhs.same_core, rhs.other_core, rhs.single_thread);
}

/// What the ladder's answers are consistent with.
enum class Finding : std::uint8_t {
  kNonReproducibleTransient,
  kCoreLocal,
  kNotCoreLocal,
  kConcurrencyDependent,
  kReproducibleUnlocated,
  kInconclusive,
};

/// The words a report uses: what the evidence is consistent with, never a
/// verdict (verification.md §7).
[[nodiscard]] constexpr std::string_view describe(Finding finding) noexcept {
  switch (finding) {
    case Finding::kNonReproducibleTransient:
      return "non-reproducible transient: did not recur on the same core; consistent with a "
             "single-event upset, a marginal timing path, or a rare race";
    case Finding::kCoreLocal:
      return "consistent with a core-local fault: recurs on its core, single-threaded too, "
             "and not on another core";
    case Finding::kNotCoreLocal:
      return "deterministic and reproducible on more than one core: consistent with memory, "
             "uncore or power delivery";
    case Finding::kConcurrencyDependent:
      return "concurrency-dependent: recurs only with several threads; could be hardware, "
             "could be a LoadForge bug";
    case Finding::kReproducibleUnlocated:
      return "reproducible, not located: recurs on its core, single-threaded too, and no "
             "other core could be tried";
    case Finding::kInconclusive:
      return "inconclusive: a re-run could not be performed";
  }
  return "unrecognised finding";
}

enum class Confidence : std::uint8_t { kLow, kMedium, kHigh };

[[nodiscard]] constexpr std::string_view describe(Confidence confidence) noexcept {
  switch (confidence) {
    case Confidence::kLow:
      return "low";
    case Confidence::kMedium:
      return "medium";
    case Confidence::kHigh:
      return "high";
  }
  return "unrecognised confidence";
}

/// The ladder's table: the three answers -- reproduced on the same core, on
/// another core, single-threaded -- to a finding, rule by rule in order:
///
///   same core unknown                  kInconclusive
///   same core no                       kNonReproducibleTransient
///   single thread no                   kConcurrencyDependent
///   single thread unknown              kInconclusive
///   other core no                      kCoreLocal
///   other core yes                     kNotCoreLocal
///   other core unknown                 kReproducibleUnlocated
[[nodiscard]] constexpr Finding derive_finding(const Answers& answers) noexcept {
  if (answers.same_core == Answer::kUnknown) {
    return Finding::kInconclusive;
  }
  if (answers.same_core == Answer::kNo) {
    return Finding::kNonReproducibleTransient;
  }
  if (answers.single_thread == Answer::kNo) {
    return Finding::kConcurrencyDependent;
  }
  if (answers.single_thread == Answer::kUnknown) {
    return Finding::kInconclusive;
  }
  if (answers.other_core == Answer::kNo) {
    return Finding::kCoreLocal;
  }
  if (answers.other_core == Answer::kYes) {
    return Finding::kNotCoreLocal;
  }
  return Finding::kReproducibleUnlocated;
}

/// How strongly the evidence supports a finding: high only for the strongest
/// evidence there is, deterministic reproduction on more than one core (§7).
[[nodiscard]] constexpr Confidence confidence_of(Finding finding) noexcept {
  switch (finding) {
    case Finding::kNotCoreLocal:
      return Confidence::kHigh;
    case Finding::kCoreLocal:
    case Finding::kReproducibleUnlocated:
      return Confidence::kMedium;
    case Finding::kNonReproducibleTransient:
    case Finding::kConcurrencyDependent:
    case Finding::kInconclusive:
      return Confidence::kLow;
  }
  return Confidence::kLow;
}

struct Step {
  RerunRequest request;
  RerunOutcome outcome = RerunOutcome::kNotPerformed;
};

[[nodiscard]] inline bool operator==(const Step& lhs, const Step& rhs) noexcept {
  return std::tie(lhs.request, lhs.outcome) == std::tie(rhs.request, rhs.outcome);
}

/// What the ladder did and what it is consistent with.
///
///   steps          the re-runs made, in rung order.
///   answers        each rung's answer: kYes for
///                  kReproduced, kNo for kClean, kUnknown for kNotPerformed or
///                  a rung not run -- except that a single-thread rung skipped
///                  because the original ran one thread takes the same-core
///                  answer, which it is.
///   finding        derive_finding(answers).
///   confidence     confidence_of(finding).
struct IsolationReport {
  std::vector<Step> steps;
  Answers answers;
  Finding finding = Finding::kInconclusive;
  Confidence confidence = Confidence::kLow;
};

[[nodiscard]] inline bool operator==(const IsolationReport& lhs,
                                     const IsolationReport& rhs) noexcept {
  return std::tie(lhs.steps, lhs.answers, lhs.finding, lhs.confidence) ==
         std::tie(rhs.steps, rhs.answers, rhs.finding, rhs.confidence);
}

/// Runs the ladder for one wrong result.
///
/// isolate(finding, placement, rerunner) makes the re-runs the header
/// comment's ladder calls for -- each through `rerunner`, with the request
/// that rung names: kSameCore {placement.cpu, placement.threads}, kOtherCore
/// {placement.other_cores[0], placement.threads}, kSingleThread
/// {placement.cpu, 1} -- in that order, stopping where the ladder stops, and
/// returns the report. It never fails: a re-run that cannot be made is an
/// answer (kUnknown), and the finding says what that leaves.
///
/// Thread-safety: one call at a time per isolator.
class Isolator {
 public:
  Isolator() = default;
  Isolator(const Isolator&) = delete;
  Isolator(Isolator&&) = delete;
  Isolator& operator=(const Isolator&) = delete;
  Isolator& operator=(Isolator&&) = delete;
  virtual ~Isolator() = default;

  [[nodiscard]] virtual IsolationReport isolate(const WrongResult& finding,
                                                const Placement& placement, Rerunner& rerunner) = 0;
};

}  // namespace loadforge::verification

#endif
