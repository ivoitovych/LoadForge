// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_SYSCALL_ERROR_HPP
#define LOADFORGE_PLATFORM_SYSCALL_ERROR_HPP

#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"

namespace loadforge::platform {

/// A failed system call: which one, on what, and why.
///
/// The call name and subject are carried because `errno` alone is not
/// actionable. "Permission denied" tells a user nothing;
/// "open(/sys/class/powercap/intel-rapl:0/energy_uj): Permission denied" tells
/// them exactly which capability they lack and which file to look at, which is
/// what the capability model (F3) has to report.
///
/// `call` is a string_view because every construction site passes a literal.
/// `subject` owns its storage, deliberately: it is usually a path, often built
/// on the fly, and an error that outlives the buffer its message points into is
/// a dangling read in the one code path nobody exercises often. The allocation
/// happens only when a call has already failed.
///
/// The suppression below is narrow and provably safe rather than a shrug.
/// clang-analyzer claims `number` can be garbage when this struct is copied out
/// of a Result; it cannot follow a value through std::variant, so it treats the
/// held alternative as unconstructed. Every field here has a default member
/// initialiser, so even a default-constructed SyscallError has defined values
/// and the condition the check describes cannot occur for this type. The
/// diagnostic is reported at the struct, which is why the marker is here rather
/// than at the copy in fs.cpp; the copy itself is asserted intact, field by
/// field, by FileSystemTest.FirstLinePropagatesTheUnderlyingFailure.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct SyscallError {
  int number = 0;         ///< The errno value the call set.
  std::string_view call;  ///< The call that failed, e.g. "open". Always a literal.
  std::string subject;    ///< What it was called on: a path, or a descriptor.

  [[nodiscard]] friend bool operator==(const SyscallError&, const SyscallError&) = default;
};

/// Human-readable rendering, suitable for putting in front of a user.
///
/// Uses strerror_r rather than strerror: the platform layer is called from a
/// multithreaded controller and strerror is not required to be thread-safe.
/// An unknown errno renders as its number rather than as an empty string --
/// silence about a failure is the one thing this project does not do (F20).
[[nodiscard]] std::string describe(const SyscallError& error);

/// Turns a raw syscall return into a Result, with the decision ABOVE the seam.
///
/// WHY THIS EXISTS FOR fork(2) AND NOT FOR open/read/close
/// -------------------------------------------------------
/// RealSyscalls is deliberately branch-free where it can be, because its own
/// decisions cannot be driven from a fixture. For most calls that costs nothing:
/// open, read and close have failures a test can genuinely provoke -- a missing
/// file, a directory, a closed descriptor -- so their error arms are reached by
/// real kernel behaviour and the branch may stay where it is.
///
/// fork(2) is different. Its documented failures are EAGAIN from RLIMIT_NPROC,
/// ENOMEM, ENOSYS and EPERM, and a test process cannot induce any of them: the
/// process limit is not even enforced for a privileged process, which is what
/// CI runs as. Leaving the branch inside RealSyscalls::fork_process would make
/// it permanently unreachable and demand a coverage exclusion -- and
/// tools/coverage-exclusions.txt says the count is zero and may only go down.
///
/// So the decision moves here, where it is an ordinary pure function with both
/// arms tested directly. That is rung 2 of the exclusion ladder (make it
/// reachable) rather than rung 4 (excuse it), and it is the same move the seam
/// itself is: put the branch where a test can stand.
template <typename T>
[[nodiscard]] core::Result<T, SyscallError> result_or_error(T value, bool failed, int number,
                                                            std::string_view call,
                                                            std::string subject) {
  if (failed) {
    return SyscallError{number, call, std::move(subject)};
  }
  return value;
}

}  // namespace loadforge::platform

#endif
