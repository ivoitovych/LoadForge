// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_JOURNAL_FILE_OPS_HPP
#define LOADFORGE_CONTROLLER_JOURNAL_FILE_OPS_HPP

// The journal's file seam. Ordinary file I/O is not host introspection, so it
// stays out of platform::Syscalls (PLAN §4.2); but the failures of open, write
// and fsync are P2 paths, and a test can only reach ENOSPC, EIO and a short
// write by forcing them, so the calls the journal makes are substitutable
// here and nowhere else. The errors are platform::SyscallError, which already
// says which call failed on what and why.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::journal {

class FileOps {
 public:
  /// How open() opens a path. kAppend is write-only, O_APPEND, created when
  /// missing; kRead is read-only; kDirectory is a directory, read-only, which
  /// is what fsync of a directory entry needs. All are close-on-exec.
  enum class Mode : std::uint8_t { kAppend, kRead, kDirectory };

  FileOps() = default;
  FileOps(const FileOps&) = delete;
  FileOps(FileOps&&) = delete;
  FileOps& operator=(const FileOps&) = delete;
  FileOps& operator=(FileOps&&) = delete;
  virtual ~FileOps() = default;

  [[nodiscard]] virtual core::Result<int, platform::SyscallError> open(const std::string& path,
                                                                       Mode mode) = 0;
  /// write(2): how many bytes the kernel took, which may be fewer than given.
  [[nodiscard]] virtual core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) = 0;
  /// read(2): how many bytes arrived; zero is the end of the file.
  [[nodiscard]] virtual core::Result<std::size_t, platform::SyscallError> read(
      int fd, std::span<std::byte> into) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, platform::SyscallError> fsync(int fd) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, platform::SyscallError> close(int fd) = 0;
};

/// The kernel.
class RealFileOps final : public FileOps {
 public:
  [[nodiscard]] core::Result<int, platform::SyscallError> open(const std::string& path,
                                                               Mode mode) override;
  [[nodiscard]] core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) override;
  [[nodiscard]] core::Result<std::size_t, platform::SyscallError> read(
      int fd, std::span<std::byte> into) override;
  [[nodiscard]] core::Result<core::Ok, platform::SyscallError> fsync(int fd) override;
  [[nodiscard]] core::Result<core::Ok, platform::SyscallError> close(int fd) override;
};

}  // namespace loadforge::journal

#endif
