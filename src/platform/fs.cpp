// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/fs.hpp"

#include <cerrno>
#include <string>
#include <utility>

namespace loadforge::platform {
namespace {

/// Close, keeping whichever failure the caller should hear about first.
///
/// A read failure outranks a close failure: it says why the value is missing,
/// which is what the user needs. A close failure with a good read is still
/// reported, because a failed close can mean the read was not what it seemed.
core::Result<std::string, SyscallError> close_and_return(
    Syscalls& syscalls, int fd, core::Result<std::string, SyscallError> outcome) {
  auto closed = syscalls.close(fd);
  if (!closed.has_value() && outcome.has_value()) {
    return closed.error();
  }
  return outcome;
}

}  // namespace

core::Result<std::string, SyscallError> FileSystem::read_file(const std::string& path) {
  auto opened = syscalls_->open_read(path);
  if (!opened.has_value()) {
    return opened.error();
  }
  const int fd = opened.value();

  std::string contents;
  std::string chunk(kChunkSize, '\0');
  int interrupts = 0;

  while (true) {
    auto count = syscalls_->read(fd, chunk.data(), chunk.size());
    if (!count.has_value()) {
      // EINTR is not a failure: a signal arrived before any byte was
      // transferred, and the read must simply be reissued. The controller
      // installs handlers for SIGCHLD and the shutdown signals, so this is
      // reached in normal operation rather than in some exotic corner.
      //
      // But the retry is BOUNDED. Retrying forever is what everyone writes and
      // it turns a signal storm into a hang, and a hung sampler is worse than a
      // failed one: the watchdog sees a stalled worker with no reason attached.
      // Giving up says what happened and how many times.
      if (count.error().number == EINTR) {
        if (++interrupts < kMaxConsecutiveInterrupts) {
          continue;
        }
        return close_and_return(
            *syscalls_, fd,
            SyscallError{EINTR, "read",
                         path + " (abandoned after " + std::to_string(interrupts) +
                             " consecutive interruptions)"});
      }
      return close_and_return(*syscalls_, fd, count.error());
    }
    interrupts = 0;  // A successful read clears the run; the bound is on runs.
    if (count.value() == 0) {
      break;  // End of file.
    }
    if (contents.size() + count.value() > kMaxFileSize) {
      // Refusing beats truncating. A truncated sysfs value parses as a
      // plausible wrong number, and a plausible wrong number is exactly the
      // failure this project exists not to produce.
      //
      // `call` names read_file, not read(2), because no system call failed
      // here: this limit is LoadForge's judgement, not the kernel's. Reporting
      // it as "read: File too large" would put words in the kernel's mouth, and
      // a project whose doctrine is never to fabricate evidence does not get to
      // fabricate the provenance of its own error messages either.
      return close_and_return(*syscalls_, fd, SyscallError{EFBIG, "read_file", path});
    }
    contents.append(chunk, 0, count.value());
  }

  return close_and_return(*syscalls_, fd, std::move(contents));
}

core::Result<std::string, SyscallError> FileSystem::read_first_line(const std::string& path) {
  auto contents = read_file(path);
  if (!contents.has_value()) {
    return contents.error();
  }

  std::string text = contents.value();
  const std::size_t newline = text.find('\n');
  if (newline != std::string::npos) {
    text.resize(newline);
  }
  // A trailing carriage return would otherwise survive into a parsed value and
  // turn "95" into something that is not a number.
  if (!text.empty() && text.back() == '\r') {
    text.pop_back();
  }
  return text;
}

}  // namespace loadforge::platform
