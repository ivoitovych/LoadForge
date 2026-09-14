// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/syscalls.hpp"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace loadforge::platform {
namespace {

/// Descriptors render as "fd 7" rather than as a bare number, so that a message
/// reading "read(fd 7): Bad file descriptor" cannot be mistaken for a path.
std::string describe_fd(int fd) { return "fd " + std::to_string(fd); }

}  // namespace

// Every function below is a translation and nothing else. There is no decision
// here to test, which is the point: docs/PLAN.md §4.3 keeps the layer that
// cannot be driven from a fixture as close to zero logic as possible, and puts
// every branch above the seam where a test can force it.
//
// These bodies are therefore NOT unit-tested against a fake -- there is nothing
// to fake them with, since they are the thing being faked. They are covered by
// the integration tier (T8), which reads a real file through a real descriptor,
// and by tests/unit/platform/real_syscalls_test.cpp, which drives them against
// genuine paths in a temporary directory, including genuine failures: a missing
// file for ENOENT, a directory for EISDIR, and a closed descriptor for EBADF.

core::Result<int, SyscallError> RealSyscalls::open_read(const std::string& path) {
  // O_CLOEXEC matters here beyond hygiene: the controller forks workers, and a
  // descriptor that survives exec is one the controller believes it closed.
  // open(2) is declared variadic because of its optional mode argument. POSIX
  // provides no non-variadic spelling -- openat is variadic too -- so this is
  // the one place the rule cannot be satisfied rather than merely inconvenient.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return SyscallError{errno, "open", path};
  }
  return fd;
}

core::Result<std::size_t, SyscallError> RealSyscalls::read(int fd, char* buffer, std::size_t size) {
  const ssize_t count = ::read(fd, buffer, size);
  if (count < 0) {
    return SyscallError{errno, "read", describe_fd(fd)};
  }
  return static_cast<std::size_t>(count);
}

core::Result<core::Ok, SyscallError> RealSyscalls::close(int fd) {
  if (::close(fd) != 0) {
    return SyscallError{errno, "close", describe_fd(fd)};
  }
  return core::Ok{};
}

core::Result<std::size_t, SyscallError> RealSyscalls::read_link(const std::string& path,
                                                                char* buffer, std::size_t size) {
  const ssize_t count = ::readlink(path.c_str(), buffer, size);
  if (count < 0) {
    return SyscallError{errno, "readlink", path};
  }
  return static_cast<std::size_t>(count);
}

core::Result<pid_t, SyscallError> RealSyscalls::fork_process() {
  // Branch-free on purpose: fork's failures cannot be induced in a test process,
  // so the decision lives in result_or_error, above the seam, where both arms
  // are reachable. See the note there.
  const pid_t pid = ::fork();
  return result_or_error<pid_t>(pid, pid < 0, errno, "fork", "worker process");
}

SyscallError RealSyscalls::exec(const std::string& path, const std::vector<std::string>& argv) {
  // execv takes char* const[], not const char* const[], for historical reasons;
  // it does not modify the strings. const_cast is the documented way to call it
  // and is why this lives below the seam rather than in code under test.
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    raw.push_back(
        const_cast<char*>(argument.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  }
  raw.push_back(nullptr);

  ::execv(path.c_str(), raw.data());
  // Only reachable because execv failed: on success this image is gone.
  return SyscallError{errno, "execv", path};
}

core::Result<core::Ok, SyscallError> RealSyscalls::set_parent_death_signal(int signal) {
  // prctl(2) is variadic, like open(2), and POSIX offers no non-variadic
  // spelling. See the note in open_read.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::prctl(PR_SET_PDEATHSIG, signal) != 0) {
    return SyscallError{errno, "prctl", "PR_SET_PDEATHSIG"};
  }
  return core::Ok{};
}

pid_t RealSyscalls::parent_pid() { return ::getppid(); }

core::Result<Reaped, SyscallError> RealSyscalls::wait_any() {
  int status = 0;
  const pid_t pid = ::waitpid(-1, &status, 0);
  // Branch-free for the same reason fork_process is: waitpid's failures are
  // ECHILD (no children at all) and EINTR, and neither can be produced on
  // demand in a test process without racing the test itself. The decision lives
  // above the seam in result_or_error, where both arms are reachable.
  return result_or_error<Reaped>(Reaped{pid, status}, pid < 0, errno, "waitpid", "any child");
}

core::Result<core::Ok, SyscallError> RealSyscalls::send_signal(pid_t pid, int signal) {
  // The call and the errno read are SEPARATE STATEMENTS, deliberately.
  //
  // Written as `result_or_error(core::Ok{}, ::kill(...) != 0, errno, ...)` this
  // compiled, looked tidier, and was wrong: the order in which a function's
  // arguments are evaluated is unsequenced, so `errno` could be read before
  // ::kill ran and the reported error was whatever the last unrelated syscall
  // had left behind. It reported ENOENT for a kill that failed with ESRCH,
  // because the test fixture had touched the filesystem first.
  //
  // The real-kernel test is what caught it; against the fake this code is not
  // even reached. fork_process and wait_any escape the same trap only because
  // their syscall already sits on its own line.
  const bool failed = ::kill(pid, signal) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "kill",
                                   "pid " + std::to_string(pid));
}

core::Result<TimeSpec, SyscallError> RealSyscalls::read_clock(int clock_id) {
  timespec value{};
  const bool failed = ::clock_gettime(clock_id, &value) != 0;
  const int number = errno;
  // The fields are carried across as they are. Combining them into nanoseconds
  // here would put an overflow check below the seam, where no test could reach
  // its failing arm -- see result_or_error's note, and Clock::to_nanoseconds.
  return result_or_error<TimeSpec>(
      TimeSpec{static_cast<std::int64_t>(value.tv_sec), static_cast<std::int64_t>(value.tv_nsec)},
      failed, number, "clock_gettime", "clock " + std::to_string(clock_id));
}

}  // namespace loadforge::platform
