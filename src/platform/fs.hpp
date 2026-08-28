// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_FS_HPP
#define LOADFORGE_PLATFORM_FS_HPP

#include <cstddef>
#include <string>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

/// Reads the small pseudo-files the platform layer lives on.
///
/// This is not general file I/O. docs/PLAN.md §4.2 draws the line: a TOML config
/// or run.json is ordinary I/O tested with temporary directories, and does not
/// come through here. What comes through here is `/sys` and `/proc`, and those
/// are strange enough to deserve their own reader:
///
///   * They report size 0 from stat(), so the read-the-size-then-read-the-file
///     pattern reads nothing. The loop below never asks how big the file is.
///   * They can return short reads with more to come.
///   * They can return EACCES on *read* despite a successful open, because the
///     permission check happens in the driver, not at open time. RAPL's
///     energy_uj is the example that motivated this whole class.
///   * They can vanish between two reads when a device is unplugged mid-run.
///
/// Every one of those is a P2 path (docs/IMPLEMENTATION.md §2.2), reachable only
/// because Syscalls is substitutable.
class FileSystem {
 public:
  /// The largest file this will read. `/sys` and `/proc` entries are tens of
  /// bytes; a megabyte means something is wrong -- a runaway generator, a
  /// misaimed path -- and reading it into memory would be the wrong answer to
  /// that. Refusing is the F20 position: a reader that cannot make sense of its
  /// input says so rather than proceeding.
  static constexpr std::size_t kMaxFileSize = 1U << 20U;

  /// Bytes requested per read(). Sized so that every real sysfs value arrives
  /// in one call while the short-read loop still exists and is still tested.
  static constexpr std::size_t kChunkSize = 4096;

  explicit FileSystem(Syscalls& syscalls) : syscalls_(&syscalls) {}

  /// The whole file as text.
  ///
  /// Reads until end of file, retrying EINTR, tolerating short reads. Always
  /// closes the descriptor, including on the error paths -- a stress run samples
  /// telemetry for hours, and a descriptor leaked once per sample exhausts the
  /// process within one run.
  ///
  /// A close() that fails *after* a successful read is reported, not swallowed:
  /// the caller decides whether a failed close matters, because the platform
  /// layer does not know what the value was for.
  [[nodiscard]] core::Result<std::string, SyscallError> read_file(const std::string& path);

  /// The file's first line, with the trailing newline removed.
  ///
  /// Almost every sysfs value is a single line, and callers that want the value
  /// should not each re-implement the trimming. An empty file yields an empty
  /// string, which is a legitimate reading and not an error -- distinguishing
  /// "empty" from "absent" is the whole point of the capability model (F3).
  [[nodiscard]] core::Result<std::string, SyscallError> read_first_line(const std::string& path);

 private:
  Syscalls* syscalls_;
};

}  // namespace loadforge::platform

#endif
