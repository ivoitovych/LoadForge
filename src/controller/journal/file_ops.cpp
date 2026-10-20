// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/journal/file_ops.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>

namespace loadforge::journal {
namespace {

// Indexed by FileOps::Mode.
constexpr std::array<int, 3> kOpenFlags{O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                                        O_RDONLY | O_CLOEXEC, O_RDONLY | O_DIRECTORY | O_CLOEXEC};
constexpr mode_t kCreateMode = 0644;

std::string describe_fd(int fd) { return "fd " + std::to_string(fd); }

}  // namespace

core::Result<int, platform::SyscallError> RealFileOps::open(const std::string& path, Mode mode) {
  // open(2) is variadic for its mode argument and POSIX has no other spelling.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int fd = ::open(path.c_str(), kOpenFlags.at(static_cast<std::size_t>(mode)), kCreateMode);
  if (fd < 0) {
    return platform::SyscallError{errno, "open", path};
  }
  return fd;
}

core::Result<std::size_t, platform::SyscallError> RealFileOps::write(
    int fd, std::span<const std::byte> bytes) {
  const ssize_t count = ::write(fd, bytes.data(), bytes.size());
  if (count < 0) {
    return platform::SyscallError{errno, "write", describe_fd(fd)};
  }
  return static_cast<std::size_t>(count);
}

core::Result<std::size_t, platform::SyscallError> RealFileOps::read(int fd,
                                                                    std::span<std::byte> into) {
  const ssize_t count = ::read(fd, into.data(), into.size());
  if (count < 0) {
    return platform::SyscallError{errno, "read", describe_fd(fd)};
  }
  return static_cast<std::size_t>(count);
}

core::Result<core::Ok, platform::SyscallError> RealFileOps::fsync(int fd) {
  if (::fsync(fd) != 0) {
    return platform::SyscallError{errno, "fsync", describe_fd(fd)};
  }
  return core::Ok{};
}

core::Result<core::Ok, platform::SyscallError> RealFileOps::close(int fd) {
  if (::close(fd) != 0) {
    return platform::SyscallError{errno, "close", describe_fd(fd)};
  }
  return core::Ok{};
}

}  // namespace loadforge::journal
