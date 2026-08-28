// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/syscalls.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>

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

}  // namespace loadforge::platform
