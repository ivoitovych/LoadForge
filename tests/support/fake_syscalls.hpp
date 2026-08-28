// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP
#define LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP

#include <cerrno>
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
/// A FAKE MUST NOT BE MORE PERMISSIVE THAN THE THING IT REPLACES
/// ------------------------------------------------------------
/// Everything above the seam is tested against this class, so anything the
/// kernel would reject and this fake accepts is a bug the suite cannot see.
/// That is the F21 failure mode wearing different clothes: not a model that
/// disagrees with reality, but one that is *laxer* than reality, which is worse
/// because it fails silently and only in production.
///
/// Three ways it was laxer, all found by reviewing it rather than by a failing
/// test, and all now closed:
///
///   * It ignored the descriptor entirely. A read on a closed or foreign fd
///     succeeded here and would be EBADF on Linux.
///   * A scripted step larger than the caller's buffer had its tail silently
///     discarded. The kernel returns the remainder on the next read, so code
///     that lost data would have passed.
///   * It counted successful closes rather than close *attempts*, so
///     all_descriptors_closed() reported a leak whenever close failed -- even
///     though the caller had done exactly the right thing.
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

  /// Raise this errno from every read, forever. The only way to reach a
  /// caller's retry limit, which a finite script cannot express.
  void always_fail_read(int error) { persistent_read_error_ = error; }

  void fail_open(int error) { open_error_ = error; }
  void fail_close(int error) { close_error_ = error; }

  [[nodiscard]] int open_count() const { return open_count_; }
  [[nodiscard]] int close_count() const { return close_count_; }
  [[nodiscard]] int read_count() const { return read_count_; }
  [[nodiscard]] const std::string& last_path() const { return last_path_; }

  /// Every descriptor handed out was handed back.
  ///
  /// Counts close ATTEMPTS, not successes: the question this answers is whether
  /// the caller released what it took, and a close that the kernel refused was
  /// still released by the caller. Counting successes made this report a leak
  /// on every failing-close test, which is the opposite of informative.
  [[nodiscard]] bool all_descriptors_closed() const { return open_count_ == close_count_; }

  core::Result<int, platform::SyscallError> open_read(const std::string& path) override {
    last_path_ = path;
    if (open_error_ != 0) {
      return platform::SyscallError{open_error_, "open", path};
    }
    ++open_count_;
    descriptor_open_ = true;
    return kFakeDescriptor;
  }

  core::Result<std::size_t, platform::SyscallError> read(int fd, char* buffer,
                                                         std::size_t size) override {
    ++read_count_;
    if (auto bad = check_descriptor(fd, "read"); !bad.has_value()) {
      return bad.error();
    }
    if (persistent_read_error_ != 0) {
      return platform::SyscallError{persistent_read_error_, "read", describe(fd)};
    }
    if (reads_.empty()) {
      return std::size_t{0};  // Nothing scripted left: end of file.
    }
    if (reads_.front().error != 0) {
      const int error = reads_.front().error;
      reads_.pop_front();
      return platform::SyscallError{error, "read", describe(fd)};
    }

    // A short read leaves the remainder for the next call, exactly as the
    // kernel does. Dropping it would let code that loses the tail of a value
    // pass -- and losing the tail turns 95000 into 95.
    ReadStep& step = reads_.front();
    const std::size_t count = step.bytes.size() < size ? step.bytes.size() : size;
    for (std::size_t i = 0; i < count; ++i) {
      buffer[i] = step.bytes[i];
    }
    if (count == step.bytes.size()) {
      reads_.pop_front();
    } else {
      step.bytes.erase(0, count);
    }
    return count;
  }

  core::Result<core::Ok, platform::SyscallError> close(int fd) override {
    if (auto bad = check_descriptor(fd, "close"); !bad.has_value()) {
      return bad.error();
    }
    // Counted and marked closed before the scripted failure is applied: on
    // Linux the descriptor is released even when close reports an error, and
    // the caller has done its part either way.
    ++close_count_;
    descriptor_open_ = false;
    if (close_error_ != 0) {
      return platform::SyscallError{close_error_, "close", describe(fd)};
    }
    return core::Ok{};
  }

 private:
  static constexpr int kFakeDescriptor = 42;

  static std::string describe(int fd) { return "fd " + std::to_string(fd); }

  /// EBADF for a descriptor this fake never handed out, or already took back.
  core::Result<core::Ok, platform::SyscallError> check_descriptor(int fd, std::string_view call) {
    if (fd != kFakeDescriptor || !descriptor_open_) {
      return platform::SyscallError{EBADF, call, describe(fd)};
    }
    return core::Ok{};
  }

  std::deque<ReadStep> reads_;
  int open_error_ = 0;
  int close_error_ = 0;
  int persistent_read_error_ = 0;
  int open_count_ = 0;
  int close_count_ = 0;
  int read_count_ = 0;
  bool descriptor_open_ = false;
  std::string last_path_;
};

}  // namespace loadforge::testing

#endif
