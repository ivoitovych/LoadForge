// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/exit_status.hpp"

#include <sys/wait.h>

#include <csignal>
#include <string>

namespace loadforge::platform {

// The W* macros take the address of their argument (glibc spells this
// __WAIT_INT, `*(const int *) &(status)`), so they need an lvalue. `raw_` is
// one, including inside these const member functions, so it is passed directly
// rather than copied into a local first.

Termination ExitStatus::termination() const noexcept {
  if (WIFEXITED(raw_)) {
    return Termination::kExited;
  }
  if (WIFSIGNALED(raw_)) {
    return Termination::kSignalled;
  }
  if (WIFSTOPPED(raw_)) {
    return Termination::kStopped;
  }
  if (WIFCONTINUED(raw_)) {
    return Termination::kContinued;
  }
  // Reached by 16,777,215 of the 2^32 possible words -- the four macros do not
  // partition the integers, which was established by enumerating them rather
  // than by reading the headers. waitpid(2) never produces one, but a truncated
  // or corrupted crash-journal record can, and that record must classify as
  // "unknown" rather than as a clean exit.
  return Termination::kUnrecognised;
}

int ExitStatus::code() const noexcept { return WIFEXITED(raw_) ? WEXITSTATUS(raw_) : 0; }

int ExitStatus::signal() const noexcept {
  if (WIFSIGNALED(raw_)) {
    return WTERMSIG(raw_);
  }
  if (WIFSTOPPED(raw_)) {
    return WSTOPSIG(raw_);
  }
  return 0;
}

bool ExitStatus::dumped_core() const noexcept { return WIFSIGNALED(raw_) && WCOREDUMP(raw_); }

bool ExitStatus::is_success() const noexcept {
  return termination() == Termination::kExited && code() == 0;
}

bool ExitStatus::killed_by_sigkill() const noexcept {
  return termination() == Termination::kSignalled && signal() == SIGKILL;
}

std::string ExitStatus::describe() const {
  switch (termination()) {
    case Termination::kExited:
      return "exited with code " + std::to_string(code());
    case Termination::kSignalled:
      return "killed by signal " + std::to_string(signal()) +
             (dumped_core() ? " (core dumped)" : "");
    case Termination::kStopped:
      return "stopped by signal " + std::to_string(signal());
    case Termination::kContinued:
      return "continued";
    // `default` rather than `case kUnrecognised`, which costs the -Wswitch
    // exhaustiveness warning and is still right. Two reasons:
    //
    // 1. It is accurate. kUnrecognised and "the switch matched no case" mean the
    //    same thing -- this is not a shape we know -- so giving them one arm
    //    states that, where separate arms imply a distinction that does not
    //    exist.
    // 2. The alternative is a coverage exclusion. This switch reads the return
    //    value of termination(), which can only ever be one of the five
    //    enumerators, so a written-out kUnrecognised case leaves the no-match
    //    edge unreachable -- gcov counts it, no test can take it, and
    //    tools/coverage-exclusions.txt says the exclusion count is zero and may
    //    only go down. core::describe(ParseError) has the same shape and stays
    //    total because it takes its enum as a PARAMETER, so a test can cast an
    //    out-of-range value in. A member function's own accessor cannot be
    //    driven that way.
    //
    // The cost is that adding an enumerator without updating this function
    // compiles. It then renders as "unrecognised", which is the graceful
    // degradation core::describe's comment asks for rather than a fall off the
    // end of a non-void function.
    //
    // The raw word is quoted deliberately: it is the only thing left that
    // carries information, and a reader debugging this needs it, not a shrug.
    default:
      return "unrecognised wait status " + std::to_string(raw_);
  }
}

}  // namespace loadforge::platform
