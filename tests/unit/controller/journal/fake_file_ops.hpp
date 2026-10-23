// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_JOURNAL_FAKE_FILE_OPS_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_JOURNAL_FAKE_FILE_OPS_HPP

// A FileOps over memory that fails on demand. By default it behaves like a
// file system: open(kAppend) creates, write appends, read reads from where the
// descriptor left off, everything else succeeds. A test scripts a failure for
// the next call of one kind; the script is consumed one step per call.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "controller/journal/file_ops.hpp"

namespace loadforge::testing {

class FakeFileOps final : public journal::FileOps {
 public:
  enum class Call : std::uint8_t { kOpen, kWrite, kRead, kFsync, kClose, kLock };

  /// One scripted outcome. `error` non-zero fails the call with that errno;
  /// otherwise `limit` caps the bytes a write takes or a read gives.
  struct Step {
    int error = 0;
    std::size_t limit = static_cast<std::size_t>(-1);
  };

  void script(Call call, Step step) { scripts_.at(static_cast<std::size_t>(call)).push_back(step); }
  void script(Call call, int error, int times = 1) {
    for (int i = 0; i < times; ++i) {
      script(call, Step{error, 0});
    }
  }

  core::Result<int, platform::SyscallError> open(const std::string& path, Mode mode) override {
    opens.emplace_back(path, mode);
    if (const auto step = next(Call::kOpen); step.error != 0) {
      return platform::SyscallError{step.error, "open", path};
    }
    if (mode == Mode::kAppend) {
      files[path];
    }
    const int fd = next_fd_++;
    open_fds[fd] = Handle{path, mode, 0};
    return fd;
  }

  core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) override {
    write_sizes.push_back(bytes.size());
    const auto step = next(Call::kWrite);
    if (step.error != 0) {
      return platform::SyscallError{step.error, "write", "fd"};
    }
    const std::size_t taken = std::min(bytes.size(), step.limit);
    auto& file = files[open_fds.at(fd).path];
    file.insert(file.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(taken));
    return taken;
  }

  core::Result<std::size_t, platform::SyscallError> read(int fd,
                                                         std::span<std::byte> into) override {
    const auto step = next(Call::kRead);
    if (step.error != 0) {
      return platform::SyscallError{step.error, "read", "fd"};
    }
    Handle& handle = open_fds.at(fd);
    const auto& file = files[handle.path];
    const std::size_t given = std::min({into.size(), step.limit, file.size() - handle.position});
    std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(handle.position), given, into.begin());
    handle.position += given;
    return given;
  }

  core::Result<core::Ok, platform::SyscallError> fsync(int fd) override {
    synced_fds.push_back(fd);
    if (const auto step = next(Call::kFsync); step.error != 0) {
      return platform::SyscallError{step.error, "fsync", "fd"};
    }
    return core::Ok{};
  }

  core::Result<core::Ok, platform::SyscallError> close(int fd) override {
    closed_fds.push_back(fd);
    open_fds.erase(fd);
    if (const auto step = next(Call::kClose); step.error != 0) {
      return platform::SyscallError{step.error, "close", "fd"};
    }
    return core::Ok{};
  }

  core::Result<core::Ok, platform::SyscallError> lock(int fd) override {
    locked_fds.push_back(fd);
    if (const auto step = next(Call::kLock); step.error != 0) {
      return platform::SyscallError{step.error, "flock", "fd"};
    }
    return core::Ok{};
  }

  struct Handle {
    std::string path;
    Mode mode = Mode::kRead;
    std::size_t position = 0;
  };

  std::map<std::string, std::vector<std::byte>> files;
  std::map<int, Handle> open_fds;
  std::vector<std::pair<std::string, Mode>> opens;
  std::vector<std::size_t> write_sizes;
  std::vector<int> synced_fds;
  std::vector<int> closed_fds;
  std::vector<int> locked_fds;

 private:
  Step next(Call call) {
    auto& queue = scripts_.at(static_cast<std::size_t>(call));
    if (queue.empty()) {
      return Step{};
    }
    const Step step = queue.front();
    queue.pop_front();
    return step;
  }

  std::array<std::deque<Step>, 6> scripts_;
  int next_fd_ = 10;
};

}  // namespace loadforge::testing

#endif
