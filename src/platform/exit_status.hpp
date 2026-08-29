// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_EXIT_STATUS_HPP
#define LOADFORGE_PLATFORM_EXIT_STATUS_HPP

#include <cstdint>
#include <string>

namespace loadforge::platform {

/// How a child process stopped running.
///
/// waitpid(2) reports four distinguishable things in one integer, and only two
/// of them are terminations. Collapsing them -- the usual `if (status != 0)` --
/// loses the distinction this project exists to report: a worker that computed a
/// wrong answer and exited 1 is a verification failure, a worker killed by
/// SIGBUS is a hardware event, and they must never be reported as the same
/// thing (docs/PLAN.md §4.6).
enum class Termination : std::uint8_t {
  kExited,        ///< Ran to completion and returned an exit code.
  kSignalled,     ///< Killed by a signal.
  kStopped,       ///< Stopped by a signal. Not a termination; the process is alive.
  kContinued,     ///< Resumed after a stop. Not a termination either.
  kUnrecognised,  ///< A status word matching none of the above. See below.
};

/// The classification of a raw waitpid(2) status word.
///
/// WHY THIS IS A TYPE AND NOT A HELPER FUNCTION
/// -------------------------------------------
/// The W* macros are easy to use wrongly in ways that compile: WEXITSTATUS on a
/// signalled child returns garbage, WTERMSIG on an exited one likewise, and
/// neither is diagnosed. Wrapping the status word in a type whose accessors
/// document which are valid when moves that from "remember the manual page" to
/// "the accessor says so", and gives one place to test every combination.
///
/// The class holds the raw word and decodes on demand rather than decomposing in
/// the constructor: the raw value is what a crash journal should record (F16),
/// because a decode this project got wrong is recoverable from the raw word and
/// not from a wrong decode.
class ExitStatus {
 public:
  /// Wraps the status word waitpid(2) wrote. No validation is possible -- every
  /// bit pattern is a legal int -- so an unrecognised one is classified as
  /// kUnrecognised rather than rejected. See `termination()`.
  constexpr explicit ExitStatus(int raw) noexcept : raw_(raw) {}

  /// The raw status word, for the crash journal and for error messages.
  [[nodiscard]] constexpr int raw() const noexcept { return raw_; }

  /// Which of the four shapes this is.
  ///
  /// kUnrecognised is reachable in principle and is reported rather than
  /// silently folded into one of the others. That is the F20 position applied to
  /// a decode: a classifier that cannot classify says so, because a supervisor
  /// told "exited with code 0" about a word it did not understand would record a
  /// clean run for a worker whose fate is unknown.
  [[nodiscard]] Termination termination() const noexcept;

  /// The exit code, valid only when termination() == kExited.
  ///
  /// Returns the low 8 bits, which is all exit(2) preserves: a worker calling
  /// exit(256) is indistinguishable from one calling exit(0), and this project
  /// therefore never uses an exit code above 255 to mean anything.
  [[nodiscard]] int code() const noexcept;

  /// The signal number, valid when termination() is kSignalled or kStopped.
  [[nodiscard]] int signal() const noexcept;

  /// Whether the kernel wrote a core file for a signalled child.
  ///
  /// Informational only: it is false whenever core dumps are disabled by
  /// RLIMIT_CORE, which is the default in most containers, so a false here is
  /// not evidence that the crash was minor.
  [[nodiscard]] bool dumped_core() const noexcept;

  /// Ran to completion and reported success. The only shape a supervisor may
  /// treat as a clean worker exit.
  [[nodiscard]] bool is_success() const noexcept;

  /// Killed by SIGKILL.
  ///
  /// NOT named "out of memory", deliberately. The kernel OOM killer uses
  /// SIGKILL, and on a memory-stress tool that is the likeliest cause -- but
  /// SIGKILL is also what a `kill -9`, a cgroup limit, a systemd stop and a
  /// watchdog send, and the signal itself carries no reason. Reporting a
  /// suspicion as a finding is exactly the fabricated provenance
  /// docs/verification.md forbids. The corroboration (the kernel log, the
  /// cgroup's memory.events) arrives with the crash journal at M5; until then
  /// this reports the fact and not the inference.
  [[nodiscard]] bool killed_by_sigkill() const noexcept;

  /// A description suitable for a user-facing report, e.g.
  /// "killed by signal 11 (core dumped)" or "exited with code 3".
  [[nodiscard]] std::string describe() const;

  [[nodiscard]] friend constexpr bool operator==(ExitStatus lhs, ExitStatus rhs) noexcept {
    return lhs.raw_ == rhs.raw_;
  }

 private:
  int raw_;
};

}  // namespace loadforge::platform

#endif
