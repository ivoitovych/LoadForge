// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_STOP_SIGNALS_HPP
#define LOADFORGE_CONTROLLER_STOP_SIGNALS_HPP

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::controller {

/// SIGINT and SIGTERM, caught for as long as this object is installed, and
/// kept as what they are: a list of signal numbers, in the order they came.
///
/// The handler does one async-signal-safe thing (Syscalls::divert_signal): it
/// writes the signal's number to the write end of a pipe. The controller
/// polls the read end with the descriptors it already polls, so a Ctrl-C ends
/// the tick's wait and is read, in ordinary code, at once. Nothing else of the
/// process knows a signal came.
///
/// The object outlives a run when a command runs several (`explore`): the
/// signals it has taken stay in it, so that the search, which runs between
/// the controller's runs, can tell that the user has asked for the end.
///
/// A signal that arrives while the object is not installed has its default
/// action, which is how a Ctrl-C ends the parts of a command that are not a
/// run (the isolation re-runs, the writing of the results).
/// "SIGINT" or "SIGTERM": the two StopSignals catches, which are the only ones
/// it can hand over, so that anything else is not named.
[[nodiscard]] std::string_view signal_name(int signal) noexcept;

class StopSignals {
 public:
  explicit StopSignals(platform::Syscalls& syscalls) noexcept : syscalls_(&syscalls) {}
  ~StopSignals() { release(); }
  StopSignals(const StopSignals&) = delete;
  StopSignals(StopSignals&&) = delete;
  StopSignals& operator=(const StopSignals&) = delete;
  StopSignals& operator=(StopSignals&&) = delete;

  /// Makes the pipe and diverts SIGINT and SIGTERM into it. A failure leaves
  /// nothing installed, nothing open, and the signals as they were. Call once.
  [[nodiscard]] core::Result<core::Ok, platform::SyscallError> install();

  /// The pipe's read end, for the caller's poll; -1 before install.
  [[nodiscard]] int fd() const noexcept { return read_fd_; }

  /// Reads what the handler has written, if anything is there (a look, never a
  /// wait), and returns how many signals that was.
  [[nodiscard]] core::Result<std::size_t, platform::SyscallError> absorb();

  /// Every signal absorbed so far, oldest first.
  [[nodiscard]] const std::vector<int>& received() const noexcept { return received_; }

  /// The first of them: the one that asked for the stop.
  [[nodiscard]] std::optional<int> first() const noexcept;

  /// Puts both signals back to their default action and closes the pipe.
  /// Silent: it runs on every way out. Does nothing when not installed.
  void release() noexcept;

 private:
  platform::Syscalls* syscalls_;
  int read_fd_ = -1;
  int write_fd_ = -1;
  std::vector<int> received_;
};

}  // namespace loadforge::controller

#endif
