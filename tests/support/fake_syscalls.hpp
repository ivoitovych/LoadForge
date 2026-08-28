// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP
#define LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "platform/syscalls.hpp"

namespace loadforge::testing {

/// A scriptable Syscalls, so that every P2 path is reachable on demand.
///
/// This is the other half of the seam. docs/IMPLEMENTATION.md §2.3 requires a
/// path to be executed, asserted, AND demonstrated to fail when the behaviour it
/// guards is broken; for error paths the demonstration is the forcing itself.
/// A test that says "EACCES is handled" without making EACCES happen has
/// asserted nothing.
///
/// The script is a queue of outcomes per call. Anything not scripted succeeds
/// with the default content, so a test states only the failure it is about.
class FakeSyscalls final : public platform::Syscalls {
 public:
  /// One scripted read: either bytes delivered, or an errno raised.
  struct ReadStep {
    std::string bytes;  ///< Delivered when `error` is zero. Empty means EOF.
    int error = 0;      ///< Non-zero raises this errno instead.
  };

  /// Deliver `content` in one read, then end of file. The common case.
  void set_content(std::string content) {
    reads_.clear();
    reads_.push_back(ReadStep{std::move(content), 0});
    reads_.push_back(ReadStep{"", 0});
  }

  /// Script the read sequence exactly: short reads, EINTR, mid-stream failures.
  void script_reads(std::vector<ReadStep> steps) { reads_.assign(steps.begin(), steps.end()); }

  void fail_open(int error) { open_error_ = error; }
  void fail_close(int error) { close_error_ = error; }

  [[nodiscard]] int open_count() const { return open_count_; }
  [[nodiscard]] int close_count() const { return close_count_; }
  [[nodiscard]] const std::string& last_path() const { return last_path_; }

  /// Every descriptor handed out must come back. A telemetry sampler reading a
  /// few dozen files per second for five hours leaks the process to death long
  /// before the run ends, and a leak on an error path is the one that survives
  /// review — so the fake counts rather than trusting.
  [[nodiscard]] bool all_descriptors_closed() const { return open_count_ == close_count_; }

  core::Result<int, platform::SyscallError> open_read(const std::string& path) override {
    last_path_ = path;
    if (open_error_ != 0) {
      return platform::SyscallError{open_error_, "open", path};
    }
    ++open_count_;
    return kFakeDescriptor;
  }

  core::Result<std::size_t, platform::SyscallError> read(int fd, char* buffer,
                                                         std::size_t size) override {
    if (reads_.empty()) {
      return std::size_t{0};  // Nothing scripted left: end of file.
    }
    const ReadStep step = reads_.front();
    reads_.pop_front();
    if (step.error != 0) {
      return platform::SyscallError{step.error, "read", "fd " + std::to_string(fd)};
    }
    const std::size_t count = step.bytes.size() < size ? step.bytes.size() : size;
    for (std::size_t i = 0; i < count; ++i) {
      buffer[i] = step.bytes[i];
    }
    return count;
  }

  core::Result<core::Ok, platform::SyscallError> close(int fd) override {
    if (close_error_ != 0) {
      return platform::SyscallError{close_error_, "close", "fd " + std::to_string(fd)};
    }
    ++close_count_;
    return core::Ok{};
  }

 private:
  static constexpr int kFakeDescriptor = 42;

  std::deque<ReadStep> reads_;
  int open_error_ = 0;
  int close_error_ = 0;
  int open_count_ = 0;
  int close_count_ = 0;
  std::string last_path_;
};

}  // namespace loadforge::testing

#endif
