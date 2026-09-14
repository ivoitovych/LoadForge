// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP
#define LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP

#include <cerrno>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <utility>
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

  // --- process control -------------------------------------------------------
  //
  // The same strictness rule as above applies, and one case needs stating: a
  // successful exec DOES NOT RETURN. This fake cannot replace the test process,
  // so `exec` here always returns an error -- which matches the real interface,
  // where the return type is SyscallError precisely because success has no
  // return path. A test for the success case would be testing something that
  // cannot happen.

  /// fork returns 0 next time, i.e. this process becomes the child.
  void fork_yields_child() { fork_result_ = 0; }
  /// fork returns this pid next time, i.e. this process stays the parent.
  void fork_yields_parent(pid_t pid) { fork_result_ = pid; }
  void fail_fork(int error) { fork_error_ = error; }

  void fail_exec(int error) { exec_error_ = error; }
  void fail_set_parent_death_signal(int error) { pdeathsig_error_ = error; }

  /// getppid's answer. Set it to something other than the pid the launcher
  /// captured before forking to simulate the supervisor dying in the race
  /// window -- the reparenting an orphan sees.
  void set_parent_pid(pid_t pid) { parent_pid_ = pid; }

  [[nodiscard]] int fork_count() const { return fork_count_; }
  [[nodiscard]] int exec_count() const { return exec_count_; }
  [[nodiscard]] int pdeathsig_count() const { return pdeathsig_count_; }
  [[nodiscard]] int last_death_signal() const { return last_death_signal_; }
  [[nodiscard]] const std::string& last_exec_path() const { return last_exec_path_; }
  [[nodiscard]] const std::vector<std::string>& last_exec_argv() const { return last_exec_argv_; }

  core::Result<std::size_t, platform::SyscallError> read_link(const std::string& path, char* buffer,
                                                              std::size_t size) override {
    ++read_link_count_;
    last_path_ = path;
    if (read_link_error_ != 0) {
      return platform::SyscallError{read_link_error_, "readlink", path};
    }
    // Truncates to the caller's buffer and reports the truncated length, with
    // no terminator and no error -- exactly what readlink(2) does, and the
    // whole reason the caller has to detect a full buffer itself.
    const std::size_t count = link_target_.size() < size ? link_target_.size() : size;
    for (std::size_t i = 0; i < count; ++i) {
      buffer[i] = link_target_[i];
    }
    return count;
  }

  void set_link_target(std::string target) { link_target_ = std::move(target); }
  void fail_read_link(int error) { read_link_error_ = error; }
  [[nodiscard]] int read_link_count() const { return read_link_count_; }

  /// One scripted fork: a pid returned (0 meaning "this process is the child"),
  /// or an errno raised.
  struct ForkStep {
    pid_t pid = 0;
    int error = 0;
  };

  /// Script a sequence of forks, for a caller that forks more than once. A pool
  /// spawning N workers needs N different pids, and the interesting cases --
  /// the fourth fork failing, the third returning 0 -- are positional.
  void script_forks(std::vector<ForkStep> steps) { forks_.assign(steps.begin(), steps.end()); }

  core::Result<pid_t, platform::SyscallError> fork_process() override {
    ++fork_count_;
    if (!forks_.empty()) {
      const ForkStep step = forks_.front();
      forks_.pop_front();
      if (step.error != 0) {
        return platform::SyscallError{step.error, "fork", "worker process"};
      }
      return step.pid;
    }
    if (fork_error_ != 0) {
      return platform::SyscallError{fork_error_, "fork", "worker process"};
    }
    return fork_result_;
  }

  platform::SyscallError exec(const std::string& path,
                              const std::vector<std::string>& argv) override {
    ++exec_count_;
    last_exec_path_ = path;
    last_exec_argv_ = argv;
    return platform::SyscallError{exec_error_, "execv", path};
  }

  core::Result<core::Ok, platform::SyscallError> set_parent_death_signal(int signal) override {
    ++pdeathsig_count_;
    last_death_signal_ = signal;
    if (pdeathsig_error_ != 0) {
      return platform::SyscallError{pdeathsig_error_, "prctl", "PR_SET_PDEATHSIG"};
    }
    return core::Ok{};
  }

  pid_t parent_pid() override { return parent_pid_; }

  /// One scripted reap: a child delivered, or an errno raised.
  struct WaitStep {
    platform::Reaped reaped;
    int error = 0;
  };

  /// Script the reap sequence exactly: a foreign child, an EINTR, an ECHILD.
  void script_waits(std::vector<WaitStep> steps) { waits_.assign(steps.begin(), steps.end()); }

  /// Raise this errno from every reap, forever -- the only way to reach the
  /// caller's interrupt bound, which a finite script cannot express.
  void always_fail_wait(int error) { persistent_wait_error_ = error; }

  void fail_signal(int error) { signal_error_ = error; }

  /// Fail only for this pid, so a test can make ONE worker vanish (ESRCH) while
  /// its neighbours are signalled normally. A blanket failure could not tell
  /// "the loop kept going" from "the loop stopped at the first error".
  void fail_signal_for(pid_t pid, int error) { signal_errors_[pid] = error; }

  [[nodiscard]] int wait_count() const { return wait_count_; }
  [[nodiscard]] const std::vector<std::pair<pid_t, int>>& signals_sent() const {
    return signals_sent_;
  }

  core::Result<platform::Reaped, platform::SyscallError> wait_any() override {
    ++wait_count_;
    if (persistent_wait_error_ != 0) {
      return platform::SyscallError{persistent_wait_error_, "waitpid", "any child"};
    }
    // Nothing left to reap is ECHILD, exactly as the kernel reports it, rather
    // than a success carrying pid 0 -- which the caller would have to
    // special-case and which no real waitpid ever returns for a blocking call.
    if (waits_.empty()) {
      return platform::SyscallError{ECHILD, "waitpid", "any child"};
    }
    const WaitStep step = waits_.front();
    waits_.pop_front();
    if (step.error != 0) {
      return platform::SyscallError{step.error, "waitpid", "any child"};
    }
    return step.reaped;
  }

  /// Set what a given clock id reports. Unset clocks return a fixed value, so a
  /// test states only the clock it is about.
  void set_clock(int clock_id, platform::TimeSpec value) { clocks_[clock_id] = value; }
  void fail_clock(int clock_id, int error) { clock_errors_[clock_id] = error; }

  [[nodiscard]] const std::vector<int>& clocks_read() const { return clocks_read_; }

  core::Result<platform::TimeSpec, platform::SyscallError> read_clock(int clock_id) override {
    clocks_read_.push_back(clock_id);
    if (const auto failed = clock_errors_.find(clock_id); failed != clock_errors_.end()) {
      return platform::SyscallError{failed->second, "clock_gettime",
                                    "clock " + std::to_string(clock_id)};
    }
    if (const auto found = clocks_.find(clock_id); found != clocks_.end()) {
      return found->second;
    }
    return platform::TimeSpec{0, 0};
  }

  core::Result<core::Ok, platform::SyscallError> send_signal(pid_t pid, int signal) override {
    signals_sent_.emplace_back(pid, signal);
    if (const auto found = signal_errors_.find(pid); found != signal_errors_.end()) {
      return platform::SyscallError{found->second, "kill", "pid " + std::to_string(pid)};
    }
    if (signal_error_ != 0) {
      return platform::SyscallError{signal_error_, "kill", "pid " + std::to_string(pid)};
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

  std::string link_target_ = "/proc/self/exe-target";
  int read_link_error_ = 0;
  int read_link_count_ = 0;

  std::deque<ForkStep> forks_;
  pid_t fork_result_ = 0;
  int fork_error_ = 0;
  int fork_count_ = 0;

  // Defaults to ENOEXEC rather than 0: a SyscallError with number 0 would
  // render as "Success", and "execv failed: Success" is the kind of message
  // that costs an hour. An unscripted exec in a test is a bug in the test.
  int exec_error_ = ENOEXEC;
  int exec_count_ = 0;
  std::string last_exec_path_;
  std::vector<std::string> last_exec_argv_;

  int pdeathsig_error_ = 0;
  int pdeathsig_count_ = 0;
  int last_death_signal_ = 0;

  pid_t parent_pid_ = kFakeSupervisorPid;

  std::deque<WaitStep> waits_;
  int persistent_wait_error_ = 0;
  int wait_count_ = 0;

  int signal_error_ = 0;
  std::map<pid_t, int> signal_errors_;
  std::vector<std::pair<pid_t, int>> signals_sent_;

  std::map<int, platform::TimeSpec> clocks_;
  std::map<int, int> clock_errors_;
  std::vector<int> clocks_read_;

 public:
  /// The pid the fake reports as the parent unless a test says otherwise.
  /// Tests pass this to become_worker as the captured supervisor pid, so the
  /// race check passes by default and a test states only the case it is about.
  static constexpr pid_t kFakeSupervisorPid = 1000;
};

}  // namespace loadforge::testing

#endif
