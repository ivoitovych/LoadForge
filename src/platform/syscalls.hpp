// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_SYSCALLS_HPP
#define LOADFORGE_PLATFORM_SYSCALLS_HPP

#include <cstddef>
#include <string>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::platform {

/// The seam. Every system call the platform layer issues goes through here.
///
/// WHY THIS INTERFACE EXISTS, AND WHY IT IS EXACTLY THIS NARROW
/// -----------------------------------------------------------
/// docs/IMPLEMENTATION.md §2.2 classifies execution paths, and class P2 --
/// "each documented failure mode of each fallible call" -- is invisible to a
/// coverage tool. An `EACCES` from open(), a short read(), an `EINTR`: none is
/// a branch gcov can report as uncovered, because the success case covers the
/// branch and the failure arm simply never runs. Coverage reads 100% and means
/// it.
///
/// The only way to reach those arms is to *force* them, and the only way to
/// force them is for the call to be substitutable. That is what this is for.
/// Every P2 path in every module built after this one is testable because this
/// seam exists; building it wrong here means retrofitting testability into ten
/// modules later (docs/IMPLEMENTATION.md §4, M1).
///
/// It is deliberately NOT an abstraction over the operating system.
/// docs/PLAN.md §4.2 rejects that: reading a TOML config or writing run.json is
/// ordinary file I/O, tested with temporary directories, and does not belong
/// here. This interface carries only what the platform layer itself calls --
/// hardware introspection, affinity, memory policy, process control -- and it
/// grows one method at a time, each with its own error-mode tests.
///
/// The virtual dispatch costs a vtable lookup against a syscall that costs
/// microseconds, which is not a trade worth thinking about.
class Syscalls {
 public:
  Syscalls() = default;
  Syscalls(const Syscalls&) = delete;
  Syscalls(Syscalls&&) = delete;
  Syscalls& operator=(const Syscalls&) = delete;
  Syscalls& operator=(Syscalls&&) = delete;
  virtual ~Syscalls() = default;

  /// open(2) with O_RDONLY|O_CLOEXEC. Returns the descriptor or the errno.
  ///
  /// O_CLOEXEC is not optional: the controller forks workers, and a descriptor
  /// leaked across exec into a worker is both a resource leak and a way for a
  /// worker to hold a file the controller believes it closed.
  [[nodiscard]] virtual core::Result<int, SyscallError> open_read(const std::string& path) = 0;

  /// read(2). Returns the byte count, which may be short or zero at end of
  /// file; a short read is not an error and callers must loop.
  [[nodiscard]] virtual core::Result<std::size_t, SyscallError> read(int fd, char* buffer,
                                                                     std::size_t size) = 0;

  /// close(2). The result is reported rather than discarded: a failing close
  /// can mean data was lost, and the platform layer does not decide for its
  /// caller whether that matters.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> close(int fd) = 0;
};

/// The real implementation. Contains no logic beyond translating errno into a
/// SyscallError, because anything with a decision in it belongs above the seam
/// where it can be tested (docs/PLAN.md §4.3).
class RealSyscalls final : public Syscalls {
 public:
  [[nodiscard]] core::Result<int, SyscallError> open_read(const std::string& path) override;
  [[nodiscard]] core::Result<std::size_t, SyscallError> read(int fd, char* buffer,
                                                             std::size_t size) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> close(int fd) override;
};

}  // namespace loadforge::platform

#endif
