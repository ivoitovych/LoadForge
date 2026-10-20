// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP
#define LOADFORGE_TESTS_SUPPORT_FAKE_SYSCALLS_HPP

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
  /// Reads issued on one descriptor, for a test that must tell one channel's
  /// reads from another's.
  [[nodiscard]] int reads_of(int fd) const {
    const auto found = reads_by_fd_.find(fd);
    return found == reads_by_fd_.end() ? 0 : found->second;
  }
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
    ++reads_by_fd_[fd];
    if (const auto pipe = pipe_readers_.find(fd); pipe != pipe_readers_.end()) {
      return read_pipe(fd, *pipe->second, buffer, size);
    }
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
    if (const auto reader = pipe_readers_.find(fd); reader != pipe_readers_.end()) {
      --reader->second->readers;
      pipe_readers_.erase(reader);
      inheritable_.erase(fd);
      ++close_count_;
      return close_outcome(fd);
    }
    if (const auto writer = pipe_writers_.find(fd); writer != pipe_writers_.end()) {
      --writer->second->writers;
      pipe_writers_.erase(writer);
      inheritable_.erase(fd);
      ++close_count_;
      return close_outcome(fd);
    }
    if (memory_files_.contains(fd)) {
      // The storage stays alive while a mapping holds it, exactly as a memfd's
      // pages outlive its last descriptor.
      memory_files_.erase(fd);
      inheritable_.erase(fd);
      ++close_count_;
      return core::Ok{};
    }
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
  [[nodiscard]] int session_count() const { return session_count_; }
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

  /// Counted, and nothing else: in the fake there is no session to lead.
  void start_session() override { ++session_count_; }

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

  /// A signal to a process group (a negative pid) answers ESRCH unless the
  /// test says the group still has members: what the kernel answers for the
  /// group of a worker that left nothing running, which is the usual case and
  /// so the default. A fake that accepted every group signal would report
  /// stragglers that do not exist.
  void group_left_running(pid_t group) { groups_left_running_.push_back(group); }

  /// The child has terminated and nobody has reaped it: a zombie. From here
  /// has_exited answers true, and a signal to it still SUCCEEDS, as the
  /// kernel's does (journal §1.27); only the reap that delivers it turns a
  /// signal into ESRCH. A fake that answered ESRCH for a zombie hid EXT-01:
  /// the controller's "a dead worker is ESRCH" never met the success that the
  /// kernel gives. A pid never mentioned is a running child.
  void child_exits(pid_t pid) { zombies_.insert(pid); }
  [[nodiscard]] bool is_zombie(pid_t pid) const { return zombies_.contains(pid); }
  [[nodiscard]] bool is_reaped(pid_t pid) const { return reaped_.contains(pid); }
  void fail_has_exited(int error) { has_exited_error_ = error; }
  [[nodiscard]] const std::vector<pid_t>& exit_peeks() const { return exit_peeks_; }

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
    mark_reaped(step.reaped.pid);
    return step.reaped;
  }

  /// Script what the non-blocking reaps deliver, in order; an empty queue is
  /// "children, but none terminated yet", which is what a controller mid-run
  /// sees on most ticks.
  void script_nonblocking_reaps(std::vector<WaitStep> steps) {
    nonblocking_reaps_.assign(steps.begin(), steps.end());
  }
  void fail_nonblocking_reap(int error) { nonblocking_reap_error_ = error; }
  [[nodiscard]] int nonblocking_reap_count() const { return nonblocking_reap_count_; }

  core::Result<std::optional<platform::Reaped>, platform::SyscallError> reap_any_nonblocking()
      override {
    ++nonblocking_reap_count_;
    if (nonblocking_reap_error_ != 0) {
      return platform::SyscallError{nonblocking_reap_error_, "waitpid",
                                    "any child, without waiting"};
    }
    if (nonblocking_reaps_.empty()) {
      return std::optional<platform::Reaped>{};
    }
    const WaitStep step = nonblocking_reaps_.front();
    nonblocking_reaps_.pop_front();
    if (step.error != 0) {
      return platform::SyscallError{step.error, "waitpid", "any child, without waiting"};
    }
    mark_reaped(step.reaped.pid);
    return std::optional<platform::Reaped>{step.reaped};
  }

  core::Result<bool, platform::SyscallError> has_exited(pid_t pid) override {
    exit_peeks_.push_back(pid);
    if (has_exited_error_ != 0) {
      return platform::SyscallError{has_exited_error_, "waitid", "exit status, without reaping"};
    }
    // A reaped child is no longer ours to ask about: ECHILD, as waitid says.
    if (reaped_.contains(pid)) {
      return platform::SyscallError{ECHILD, "waitid", "exit status, without reaping"};
    }
    return zombies_.contains(pid);
  }

  /// Set what a given clock id reports. Unset clocks return a fixed value, so a
  /// test states only the clock it is about.
  void set_clock(int clock_id, platform::TimeSpec value) { clocks_[clock_id] = value; }
  void fail_clock(int clock_id, int error) { clock_errors_[clock_id] = error; }

  [[nodiscard]] const std::vector<int>& clocks_read() const { return clocks_read_; }

  /// Every clock advances by this many nanoseconds on each read of it, so a
  /// loop that reads the clock each tick sees time pass within one call. The
  /// step applies per clock id, so three clocks read together stay within a
  /// step of each other -- far below the suspend threshold.
  void advance_clocks_per_read(std::int64_t nanoseconds) { clock_step_ns_ = nanoseconds; }

  /// Fail a clock on its Nth read (1-based) and succeed before and after, so
  /// a loop's mid-run failure arm is reachable without failing its first read.
  void fail_clock_on_read(int clock_id, int nth, int error) {
    clock_failures_at_[clock_id] = std::pair{nth, error};
  }
  /// Report this value on the Nth read of a clock and the stepped value on
  /// every other: a clock that jumps once, backwards or forwards.
  void jump_clock_on_read(int clock_id, int nth, platform::TimeSpec value) {
    clock_jumps_at_[clock_id] = std::pair{nth, value};
  }

  core::Result<platform::TimeSpec, platform::SyscallError> read_clock(int clock_id) override {
    clocks_read_.push_back(clock_id);
    const int nth = ++reads_per_clock_[clock_id];
    if (const auto failed = clock_errors_.find(clock_id); failed != clock_errors_.end()) {
      return platform::SyscallError{failed->second, "clock_gettime",
                                    "clock " + std::to_string(clock_id)};
    }
    if (const auto scheduled = clock_failures_at_.find(clock_id);
        scheduled != clock_failures_at_.end() && scheduled->second.first == nth) {
      return platform::SyscallError{scheduled->second.second, "clock_gettime",
                                    "clock " + std::to_string(clock_id)};
    }
    if (const auto jump = clock_jumps_at_.find(clock_id);
        jump != clock_jumps_at_.end() && jump->second.first == nth) {
      return jump->second.second;
    }
    platform::TimeSpec value{0, 0};
    if (const auto found = clocks_.find(clock_id); found != clocks_.end()) {
      value = found->second;
    }
    // Advanced AFTER the value is taken, so the first read reports the base
    // and each later read is one step further on; carried as whole seconds
    // plus nanoseconds so a long run cannot overflow the nanoseconds field.
    // Only a stepping clock is normalised: a test that set a nonsense reading
    // on purpose gets it back exactly as set.
    if (clock_step_ns_ != 0) {
      const std::int64_t advance = clock_step_ns_ * (nth - 1);
      value.seconds += advance / 1'000'000'000;
      value.nanoseconds += advance % 1'000'000'000;
      if (value.nanoseconds >= 1'000'000'000) {
        value.seconds += 1;
        value.nanoseconds -= 1'000'000'000;
      } else if (value.nanoseconds < 0) {  // A negative step: borrow a second.
        value.seconds -= 1;
        value.nanoseconds += 1'000'000'000;
      }
    }
    return value;
  }

  // --- signals and identity --------------------------------------------------------

  void fail_ignore_signal(int error) { ignore_signal_error_ = error; }
  [[nodiscard]] const std::vector<int>& signals_ignored() const { return signals_ignored_; }

  core::Result<core::Ok, platform::SyscallError> ignore_signal(int signal) override {
    signals_ignored_.push_back(signal);
    if (ignore_signal_error_ != 0) {
      return platform::SyscallError{ignore_signal_error_, "sigaction",
                                    "signal " + std::to_string(signal)};
    }
    return core::Ok{};
  }

  /// The pid this fake reports as its own.
  static constexpr pid_t kFakePid = 4242;
  pid_t process_id() override { return kFakePid; }

  core::Result<core::Ok, platform::SyscallError> send_signal(pid_t pid, int signal) override {
    signals_sent_.emplace_back(pid, signal);
    if (const auto found = signal_errors_.find(pid); found != signal_errors_.end()) {
      return platform::SyscallError{found->second, "kill", "pid " + std::to_string(pid)};
    }
    if (signal_error_ != 0) {
      return platform::SyscallError{signal_error_, "kill", "pid " + std::to_string(pid)};
    }
    // The kernel: a zombie takes the signal and discards it, and only a pid
    // that has been reaped is ESRCH.
    if (reaped_.contains(pid)) {
      return platform::SyscallError{ESRCH, "kill", "pid " + std::to_string(pid)};
    }
    if (pid < 0 && std::find(groups_left_running_.begin(), groups_left_running_.end(), -pid) ==
                       groups_left_running_.end()) {
      return platform::SyscallError{ESRCH, "kill", "pid " + std::to_string(pid)};
    }
    return core::Ok{};
  }

  // --- affinity ----------------------------------------------------------------
  //
  // Modelled the way the kernel behaves, not the way a caller might hope:
  // set_affinity intersects the requested mask with what the cpuset PERMITS and
  // refuses with EINVAL when nothing is left -- which is also what an empty
  // mask, or one naming only CPUs this machine lacks, gets. That intersection
  // is the whole P7 state of this module: a CPU the topology found is not
  // necessarily one this process may run on. A fake that accepted any mask
  // would be laxer than the kernel, and the tests above the seam would prove
  // nothing about the case that matters.

  /// What the cpuset permits. Defaults to every CPU, so a test states only the
  /// narrowing it is about.
  void permit_cpus(const platform::CpuMask& permitted) {
    permitted_ = permitted;
    current_affinity_.cpus &= permitted_.cpus;
  }
  /// What get_affinity reports until a successful set changes it.
  void set_current_affinity(const platform::CpuMask& current) { current_affinity_ = current; }
  void fail_get_affinity(int error) { get_affinity_error_ = error; }
  void fail_set_affinity(int error) { set_affinity_error_ = error; }

  [[nodiscard]] const platform::CpuMask& current_affinity() const { return current_affinity_; }
  [[nodiscard]] const std::vector<pid_t>& affinity_pids() const { return affinity_pids_; }

  core::Result<platform::CpuMask, platform::SyscallError> get_affinity(pid_t pid) override {
    affinity_pids_.push_back(pid);
    if (get_affinity_error_ != 0) {
      return platform::SyscallError{get_affinity_error_, "sched_getaffinity",
                                    "pid " + std::to_string(pid)};
    }
    return current_affinity_;
  }

  core::Result<core::Ok, platform::SyscallError> set_affinity(
      pid_t pid, const platform::CpuMask& mask) override {
    affinity_pids_.push_back(pid);
    if (set_affinity_error_ != 0) {
      return platform::SyscallError{set_affinity_error_, "sched_setaffinity",
                                    "pid " + std::to_string(pid)};
    }
    platform::CpuMask effective;
    effective.cpus = mask.cpus & permitted_.cpus;
    if (effective.cpus.none()) {
      return platform::SyscallError{EINVAL, "sched_setaffinity", "pid " + std::to_string(pid)};
    }
    current_affinity_ = effective;
    return core::Ok{};
  }

  // --- memory ------------------------------------------------------------------
  //
  // The P7 state of this module is a huge-page pool: present, empty, or
  // present but too small for the request. The pool here is consumed by every
  // MAP_HUGETLB mapping and refilled by its unmap, in whole pages, the way the
  // kernel accounts for it -- and the way the kernel does NOT: it rounds a
  // MAP_HUGETLB length up (verified: a one-byte request maps, and takes a
  // page) but munmap does not round, so unmapping with the unrounded length is
  // EINVAL and the mapping stays. A fake that forgave that would let a Mapping
  // remember the wrong length and leak a huge page on every run.
  //
  // Where this fake is STRICTER than the kernel it says so: an unmap of an
  // address it never handed out is EINVAL here and a silent success on Linux,
  // because a double unmap is a bug the suite should see rather than one the
  // kernel happens to tolerate. Stricter is not laxer.
  //
  // The memory handed out is real, from the heap, so a caller may write to it.

  static constexpr std::size_t kFakeHugePageBytes = std::size_t{2} << 20;

  /// Huge pages the pool holds. Defaults to none, which is what a fresh kernel
  /// reserves, so a test that wants explicit huge pages says so.
  void set_huge_pages_free(std::size_t pages) { huge_pages_free_ = pages; }
  /// Which NUMA nodes have memory. Defaults to node 0 alone.
  void permit_nodes(const platform::NodeMask& nodes) { permitted_nodes_ = nodes; }
  /// RLIMIT_MEMLOCK as an unprivileged process meets it: zero is EPERM,
  /// exceeding it is ENOMEM. Defaults to unlimited, as for a privileged one.
  void set_lock_limit(std::size_t bytes) { lock_limit_ = bytes; }

  void fail_map(int error) { map_error_ = error; }
  void fail_unmap(int error) { unmap_error_ = error; }
  void fail_lock(int error) { lock_error_ = error; }
  /// Fail one advice and not another, so THP advice and populate are told apart.
  void fail_advice(int advice, int error) { advice_errors_[advice] = error; }
  void fail_bind(int error) { bind_error_ = error; }

  [[nodiscard]] std::size_t live_mappings() const { return mappings_.size(); }
  [[nodiscard]] std::size_t huge_pages_free() const { return huge_pages_free_; }
  [[nodiscard]] std::size_t locked_bytes() const { return locked_bytes_; }
  [[nodiscard]] int unmap_count() const { return unmap_count_; }
  [[nodiscard]] const std::vector<int>& map_flags() const { return map_flags_; }
  [[nodiscard]] const std::vector<int>& advice_given() const { return advice_given_; }
  [[nodiscard]] const std::vector<platform::NodeMask>& binds() const { return binds_; }

  core::Result<void*, platform::SyscallError> map_anonymous(std::size_t length,
                                                            int extra_flags) override {
    map_flags_.push_back(extra_flags);
    const std::string subject = std::to_string(length) + " bytes";
    if (map_error_ != 0) {
      return platform::SyscallError{map_error_, "mmap", subject};
    }
    if (length == 0) {
      return platform::SyscallError{EINVAL, "mmap", subject};
    }
    const bool huge = (extra_flags & MAP_HUGETLB) != 0;
    std::size_t pages = 0;
    if (huge) {
      pages = (length + kFakeHugePageBytes - 1) / kFakeHugePageBytes;
      if (pages > huge_pages_free_) {
        return platform::SyscallError{ENOMEM, "mmap", subject};
      }
      huge_pages_free_ -= pages;
      length = pages * kFakeHugePageBytes;
    }
    auto storage = std::make_unique<std::byte[]>(length);
    void* const address = storage.get();
    mappings_[address] = FakeMapping{std::move(storage), length, pages, 0};
    return address;
  }

  core::Result<core::Ok, platform::SyscallError> unmap(void* address, std::size_t length) override {
    ++unmap_count_;
    const std::string subject = std::to_string(length) + " bytes";
    // Several mappings of one file share an address here (the fake hands out
    // the file's own storage), so they are counted, and an unmap releases one.
    if (shared_mappings_.contains(address)) {
      const auto range = shared_mappings_.equal_range(address);
      auto match = range.first;
      while (match != range.second && match->second != length) {
        ++match;
      }
      if (match == range.second) {
        return platform::SyscallError{EINVAL, "munmap", subject};
      }
      if (unmap_error_ != 0) {
        return platform::SyscallError{unmap_error_, "munmap", subject};
      }
      shared_mappings_.erase(match);
      return core::Ok{};
    }
    const auto found = mappings_.find(address);
    // Unknown address, or a length that is not the whole mapping: EINVAL. The
    // second is exactly the kernel's answer for a huge-page mapping and the
    // unrounded length; for ordinary pages the kernel would unmap part of the
    // range, which nothing above the seam asks for.
    if (found == mappings_.end() || found->second.bytes != length) {
      return platform::SyscallError{EINVAL, "munmap", subject};
    }
    if (unmap_error_ != 0) {
      return platform::SyscallError{unmap_error_, "munmap", subject};
    }
    huge_pages_free_ += found->second.huge_pages;
    locked_bytes_ -= found->second.locked;
    mappings_.erase(found);
    return core::Ok{};
  }

  core::Result<core::Ok, platform::SyscallError> lock_memory(void* address,
                                                             std::size_t length) override {
    const std::string subject = std::to_string(length) + " bytes";
    const auto found = mappings_.find(address);
    if (found == mappings_.end()) {
      return platform::SyscallError{ENOMEM, "mlock", subject};  // As the kernel says.
    }
    if (lock_error_ != 0) {
      return platform::SyscallError{lock_error_, "mlock", subject};
    }
    if (lock_limit_ == 0) {
      return platform::SyscallError{EPERM, "mlock", subject};
    }
    if (length > lock_limit_ - locked_bytes_) {
      return platform::SyscallError{ENOMEM, "mlock", subject};
    }
    locked_bytes_ += length;
    found->second.locked += length;
    return core::Ok{};
  }

  core::Result<core::Ok, platform::SyscallError> advise_memory(void* address, std::size_t length,
                                                               int advice) override {
    advice_given_.push_back(advice);
    const std::string subject = std::to_string(length) + " bytes, advice " + std::to_string(advice);
    if (mappings_.find(address) == mappings_.end()) {
      return platform::SyscallError{ENOMEM, "madvise", subject};  // As the kernel says.
    }
    if (const auto failed = advice_errors_.find(advice); failed != advice_errors_.end()) {
      return platform::SyscallError{failed->second, "madvise", subject};
    }
    return core::Ok{};
  }

  // --- memory files and shared mappings -------------------------------------------
  //
  // A memory file is the one thing that can carry a region across exec: the
  // worker is a fresh image and inherits descriptors, not mappings. The model
  // here is the kernel's: a file has a length, every map_shared of the same
  // descriptor returns the SAME storage (that is what shared means, and it is
  // what lets a supervisor and a "worker" in one test process see each other's
  // writes), the descriptor is close-on-exec until inherit_across_exec says
  // otherwise, and the storage outlives the descriptor while a mapping holds
  // it. Stricter than the kernel in one place, on purpose: mapping more than
  // the file's length is EINVAL here, where Linux would map it and SIGBUS on
  // the first touch past the end -- a crash is not an answer a test can read.

  void fail_create_memory_file(int error) { memory_file_error_ = error; }
  void fail_map_shared(int error) { map_shared_error_ = error; }
  void fail_inherit(int error) { inherit_error_ = error; }

  /// Whether the descriptor would survive exec: cleared of close-on-exec.
  [[nodiscard]] bool inheritable(int fd) const { return inheritable_.contains(fd); }
  /// Every descriptor ever handed over, in order, whether or not it has
  /// since been closed: what a child did before its exec, read afterwards.
  [[nodiscard]] const std::vector<int>& handed_over() const { return handed_over_; }
  [[nodiscard]] std::size_t live_shared_mappings() const { return shared_mappings_.size(); }
  [[nodiscard]] const std::vector<std::string>& memory_file_names() const {
    return memory_file_names_;
  }

  core::Result<int, platform::SyscallError> create_memory_file(const std::string& name,
                                                               std::size_t length) override {
    memory_file_names_.push_back(name);
    if (memory_file_error_ != 0) {
      return platform::SyscallError{memory_file_error_, "memfd_create", name};
    }
    const int fd = next_memory_fd_++;
    memory_files_[fd] = std::make_shared<std::vector<std::byte>>(length);
    ++open_count_;
    return fd;
  }

  core::Result<void*, platform::SyscallError> map_shared(int fd, std::size_t length) override {
    const std::string subject = describe(fd) + ", " + std::to_string(length) + " bytes";
    const auto file = memory_files_.find(fd);
    if (file == memory_files_.end()) {
      return platform::SyscallError{EBADF, "mmap", subject};
    }
    if (map_shared_error_ != 0) {
      return platform::SyscallError{map_shared_error_, "mmap", subject};
    }
    if (length == 0 || length > file->second->size()) {
      return platform::SyscallError{EINVAL, "mmap", subject};
    }
    void* const address = file->second->data();
    shared_storage_[address] = file->second;  // Keeps the bytes alive past close().
    shared_mappings_.emplace(address, length);
    return address;
  }

  core::Result<core::Ok, platform::SyscallError> inherit_across_exec(int fd) override {
    if (inherit_error_ != 0) {
      return platform::SyscallError{inherit_error_, "fcntl", describe(fd)};
    }
    if (!is_open(fd)) {
      return platform::SyscallError{EBADF, "fcntl", describe(fd)};
    }
    inheritable_.insert(fd);
    handed_over_.push_back(fd);
    return core::Ok{};
  }

  // --- pipes ---------------------------------------------------------------------
  //
  // A pipe is two descriptors over one byte queue. The model is the kernel's
  // where a test can tell: bytes arrive in order and may be delivered short;
  // a read with the queue empty is end of file once the write end is closed;
  // a write with the read end closed is EPIPE (the kernel also raises SIGPIPE,
  // which the worker ignores; the fake cannot raise it and does not pretend
  // to); poll reports readable while bytes are queued and hung up once the
  // writer is gone, both at once when both hold, and POLLNVAL for a
  // descriptor that is not open.
  //
  // Stricter than the kernel in two places, on purpose. A read of an empty pipe
  // whose writer is still open would BLOCK on Linux, and a fake cannot block,
  // so it answers EAGAIN: nothing above the seam reads without polling first,
  // and a test that does has found a bug in the code, not in the fake. And a
  // poll entry with a negative number, which the kernel silently ignores, is
  // reported invalid here, because nothing above the seam has a reason to
  // poll a descriptor it does not hold.

  /// One scripted write: accept this many bytes (short when fewer than
  /// offered, zero to transfer nothing), or raise an errno instead.
  struct WriteStep {
    std::size_t accept = 0;
    int error = 0;
  };

  /// Script the write sequence exactly: short writes, EINTR, a mid-stream
  /// failure. Unscripted writes accept everything offered.
  void script_writes(std::vector<WriteStep> steps) { writes_.assign(steps.begin(), steps.end()); }
  /// Raise this errno from every write, forever: the caller's interrupt bound.
  void always_fail_write(int error) { persistent_write_error_ = error; }
  void fail_create_pipe(int error) { pipe_error_ = error; }
  /// Fail only the Nth pipe created (1-based), so a caller that creates two
  /// per worker can be failed on either.
  void fail_create_pipe_on(int nth, int error) { pipe_failure_at_ = std::pair{nth, error}; }
  void fail_poll(int error) { poll_error_ = error; }
  /// Fail only the Nth poll (1-based): a loop's later poll, or the one after it.
  void fail_poll_on(int nth, int error) { poll_failure_at_ = std::pair{nth, error}; }
  /// Fail the close of one pipe end and no other.
  void fail_close_of(int fd, int error) { close_errors_[fd] = error; }

  /// Model a worker process holding this pipe end after exec: the pipe's
  /// far side stays open when the controller closes its own copy, exactly as
  /// a real worker keeps the end it inherited. Called while the number is
  /// still open here; released when the worker is modelled as exiting.
  void worker_holds_end(int fd) {
    if (const auto reader = pipe_readers_.find(fd); reader != pipe_readers_.end()) {
      ++reader->second->readers;
      held_ends_[fd] = HeldEnd{reader->second, true};
    } else {
      ++pipe_writers_.at(fd)->writers;
      held_ends_[fd] = HeldEnd{pipe_writers_.at(fd), false};
    }
  }
  void worker_releases_end(int fd) {
    const HeldEnd held = held_ends_.at(fd);
    held_ends_.erase(fd);
    if (held.reader) {
      --held.pipe->readers;
    } else {
      --held.pipe->writers;
    }
  }
  /// Number the next pipes' ends from this descriptor on. The kernel hands
  /// out the lowest number free, which is 0 in a process started with its
  /// standard input closed; the default, well away from that, keeps a pipe's
  /// ends apart from every other descriptor here.
  void number_pipes_from(int fd) { next_pipe_fd_ = fd; }
  /// Deliver at most this many bytes per pipe read, so a frame arrives in
  /// pieces. Defaults to everything queued.
  void limit_pipe_reads(std::size_t bytes) { pipe_read_limit_ = bytes; }
  /// What poll reports for this descriptor, overriding the model: a reader
  /// that is told "readable" with nothing queued, or an error condition.
  void script_readiness(int fd, platform::Readiness readiness) {
    scripted_readiness_[fd] = readiness;
  }

  [[nodiscard]] int write_count() const { return write_count_; }
  [[nodiscard]] int poll_count() const { return poll_count_; }
  [[nodiscard]] const std::vector<int>& poll_timeouts() const { return poll_timeouts_; }
  /// Everything written to this pipe's write end so far, in order.
  [[nodiscard]] std::string pipe_contents(int write_fd) const {
    const auto writer = pipe_writers_.find(write_fd);
    return writer == pipe_writers_.end() ? std::string{} : writer->second->queue;
  }
  /// Queue bytes for a pipe's reader directly, as if a writer in another
  /// process had sent them.
  void deliver_to_pipe(int read_fd, const std::string& bytes) {
    pipe_readers_.at(read_fd)->queue += bytes;
  }

  core::Result<platform::PipeEnds, platform::SyscallError> create_pipe() override {
    ++pipes_created_;
    if (pipe_error_ != 0) {
      return platform::SyscallError{pipe_error_, "pipe2", "record channel"};
    }
    if (pipe_failure_at_.has_value() && pipe_failure_at_->first == pipes_created_) {
      return platform::SyscallError{pipe_failure_at_->second, "pipe2", "record channel"};
    }
    auto pipe = std::make_shared<FakePipe>();
    const int read_fd = next_pipe_fd_++;
    const int write_fd = next_pipe_fd_++;
    pipe_readers_[read_fd] = pipe;
    pipe_writers_[write_fd] = pipe;
    open_count_ += 2;
    return platform::PipeEnds{read_fd, write_fd};
  }

  core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) override {
    ++write_count_;
    const std::size_t size = bytes.size();
    const auto writer = pipe_writers_.find(fd);
    // A descriptor that is not a pipe's write end is EBADF: the fake's other
    // descriptors are opened read-only, and the kernel says the same of them.
    if (writer == pipe_writers_.end()) {
      return platform::SyscallError{EBADF, "write", describe(fd)};
    }
    if (persistent_write_error_ != 0) {
      return platform::SyscallError{persistent_write_error_, "write", describe(fd)};
    }
    std::size_t accepted = size;
    if (!writes_.empty()) {
      const WriteStep step = writes_.front();
      writes_.pop_front();
      if (step.error != 0) {
        return platform::SyscallError{step.error, "write", describe(fd)};
      }
      accepted = step.accept < size ? step.accept : size;
    }
    if (writer->second->readers <= 0) {
      return platform::SyscallError{EPIPE, "write", describe(fd)};
    }
    for (const std::byte byte : bytes.first(accepted)) {
      writer->second->queue.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return accepted;
  }

  core::Result<std::vector<platform::Readiness>, platform::SyscallError> poll_readable(
      const std::vector<int>& fds, int timeout_ms) override {
    ++poll_count_;
    poll_timeouts_.push_back(timeout_ms);
    if (poll_error_ != 0) {
      return platform::SyscallError{poll_error_, "poll",
                                    std::to_string(fds.size()) + " descriptors"};
    }
    if (poll_failure_at_.has_value() && poll_failure_at_->first == poll_count_) {
      return platform::SyscallError{poll_failure_at_->second, "poll",
                                    std::to_string(fds.size()) + " descriptors"};
    }
    std::vector<platform::Readiness> readiness;
    readiness.reserve(fds.size());
    for (const int fd : fds) {
      readiness.push_back(readiness_of(fd));
    }
    return readiness;
  }

  core::Result<core::Ok, platform::SyscallError> bind_memory(
      void* address, std::size_t length, const platform::NodeMask& nodes) override {
    binds_.push_back(nodes);
    const std::string subject = std::to_string(length) + " bytes";
    if (mappings_.find(address) == mappings_.end()) {
      return platform::SyscallError{EFAULT, "mbind", subject};  // As the kernel says.
    }
    if (bind_error_ != 0) {
      return platform::SyscallError{bind_error_, "mbind", subject};
    }
    // An empty mask, or a node without memory: both EINVAL on Linux, verified.
    if (nodes.nodes.none() || (nodes.nodes & ~permitted_nodes_.nodes).any()) {
      return platform::SyscallError{EINVAL, "mbind", subject};
    }
    return core::Ok{};
  }

 private:
  static constexpr int kFakeDescriptor = 42;
  /// Memory-file descriptors start well away from kFakeDescriptor so a test
  /// that confuses the two kinds gets EBADF, not a coincidence.
  static constexpr int kFirstMemoryFd = 100;

  /// Pipe descriptors start past the memory-file range for the same reason.
  static constexpr int kFirstPipeFd = 200;

  static std::string describe(int fd) { return "fd " + std::to_string(fd); }

  /// EBADF for a descriptor this fake never handed out, or already took back.
  [[nodiscard]] core::Result<core::Ok, platform::SyscallError> check_descriptor(
      int fd, std::string_view call) const {
    if (fd != kFakeDescriptor || !descriptor_open_) {
      return platform::SyscallError{EBADF, call, describe(fd)};
    }
    return core::Ok{};
  }

  /// Any descriptor of any kind this fake currently has open.
  [[nodiscard]] bool is_open(int fd) const {
    return (fd == kFakeDescriptor && descriptor_open_) || memory_files_.contains(fd) ||
           pipe_readers_.contains(fd) || pipe_writers_.contains(fd);
  }

  /// A pipe: its queue and how many descriptors hold each end, here and in
  /// the workers a test models (worker_holds_end). An end is open while any
  /// holder remains, as the kernel counts it.
  struct FakePipe {
    std::string queue;
    int readers = 1;
    int writers = 1;
  };

  struct HeldEnd {
    std::shared_ptr<FakePipe> pipe;
    bool reader = false;
  };

  /// A pipe end's close: counted and released already; the scripted errno,
  /// if any, is the answer the caller hears.
  core::Result<core::Ok, platform::SyscallError> close_outcome(int fd) {
    if (const auto failed = close_errors_.find(fd); failed != close_errors_.end()) {
      return platform::SyscallError{failed->second, "close", describe(fd)};
    }
    return core::Ok{};
  }

  core::Result<std::size_t, platform::SyscallError> read_pipe(int fd, FakePipe& pipe, char* buffer,
                                                              std::size_t size) {
    if (persistent_read_error_ != 0) {
      return platform::SyscallError{persistent_read_error_, "read", describe(fd)};
    }
    // A scripted errno applies to a pipe read too; scripted BYTES do not,
    // because a pipe's bytes come from its writer.
    if (!reads_.empty() && reads_.front().error != 0) {
      const int error = reads_.front().error;
      reads_.pop_front();
      return platform::SyscallError{error, "read", describe(fd)};
    }
    if (pipe.queue.empty()) {
      if (pipe.writers > 0) {
        return platform::SyscallError{EAGAIN, "read", describe(fd)};  // Would block.
      }
      return std::size_t{0};  // End of file: every writer is gone.
    }
    std::size_t count = pipe.queue.size() < size ? pipe.queue.size() : size;
    if (count > pipe_read_limit_) {
      count = pipe_read_limit_;
    }
    for (std::size_t i = 0; i < count; ++i) {
      buffer[i] = pipe.queue[i];
    }
    pipe.queue.erase(0, count);
    return count;
  }

  [[nodiscard]] platform::Readiness readiness_of(int fd) const {
    if (const auto scripted = scripted_readiness_.find(fd); scripted != scripted_readiness_.end()) {
      return scripted->second;
    }
    if (const auto reader = pipe_readers_.find(fd); reader != pipe_readers_.end()) {
      platform::Readiness readiness;
      readiness.readable = !reader->second->queue.empty();
      readiness.hung_up = reader->second->writers <= 0;
      return readiness;
    }
    if (const auto writer = pipe_writers_.find(fd); writer != pipe_writers_.end()) {
      platform::Readiness readiness;
      readiness.error = writer->second->readers <= 0;  // POLLERR: no reader left.
      return readiness;
    }
    platform::Readiness readiness;
    if (is_open(fd)) {
      readiness.readable = true;  // A regular file is always readable.
    } else {
      readiness.invalid = true;
    }
    return readiness;
  }

  std::deque<ReadStep> reads_;
  int open_error_ = 0;
  int close_error_ = 0;
  int persistent_read_error_ = 0;
  int open_count_ = 0;
  int close_count_ = 0;
  int read_count_ = 0;
  std::map<int, int> reads_by_fd_;
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
  int session_count_ = 0;

  pid_t parent_pid_ = kFakeSupervisorPid;

  std::deque<WaitStep> waits_;
  int persistent_wait_error_ = 0;
  int wait_count_ = 0;
  std::deque<WaitStep> nonblocking_reaps_;
  int nonblocking_reap_error_ = 0;
  int nonblocking_reap_count_ = 0;

  void mark_reaped(pid_t pid) {
    zombies_.erase(pid);
    reaped_.insert(pid);
  }

  std::set<pid_t> zombies_;
  std::set<pid_t> reaped_;
  int has_exited_error_ = 0;
  std::vector<pid_t> exit_peeks_;

  int signal_error_ = 0;
  std::map<pid_t, int> signal_errors_;
  std::vector<std::pair<pid_t, int>> signals_sent_;
  std::vector<pid_t> groups_left_running_;

  platform::CpuMask permitted_ = all_cpus();
  platform::CpuMask current_affinity_ = all_cpus();
  int get_affinity_error_ = 0;
  int set_affinity_error_ = 0;
  std::vector<pid_t> affinity_pids_;

  static platform::CpuMask all_cpus() {
    platform::CpuMask mask;
    mask.cpus.set();
    return mask;
  }

  std::map<int, platform::TimeSpec> clocks_;
  std::map<int, int> clock_errors_;
  std::vector<int> clocks_read_;
  std::map<int, int> reads_per_clock_;
  std::map<int, std::pair<int, int>> clock_failures_at_;
  std::map<int, std::pair<int, platform::TimeSpec>> clock_jumps_at_;
  std::int64_t clock_step_ns_ = 0;
  int ignore_signal_error_ = 0;
  std::vector<int> signals_ignored_;

  struct FakeMapping {
    std::unique_ptr<std::byte[]> storage;
    std::size_t bytes = 0;
    std::size_t huge_pages = 0;  ///< Taken from the pool; returned on unmap.
    std::size_t locked = 0;
  };
  std::map<void*, FakeMapping> mappings_;
  std::map<int, std::shared_ptr<std::vector<std::byte>>> memory_files_;
  std::map<void*, std::shared_ptr<std::vector<std::byte>>> shared_storage_;
  std::multimap<void*, std::size_t> shared_mappings_;
  std::set<int> inheritable_;
  std::vector<int> handed_over_;
  std::vector<std::string> memory_file_names_;
  int next_memory_fd_ = kFirstMemoryFd;
  int memory_file_error_ = 0;
  int map_shared_error_ = 0;
  int inherit_error_ = 0;
  std::size_t huge_pages_free_ = 0;
  platform::NodeMask permitted_nodes_ = node_zero();
  std::size_t lock_limit_ = std::numeric_limits<std::size_t>::max();
  std::size_t locked_bytes_ = 0;
  int map_error_ = 0;
  int unmap_error_ = 0;
  int lock_error_ = 0;
  int bind_error_ = 0;
  int unmap_count_ = 0;
  std::map<int, int> advice_errors_;
  std::vector<int> map_flags_;
  std::vector<int> advice_given_;
  std::vector<platform::NodeMask> binds_;

  std::map<int, std::shared_ptr<FakePipe>> pipe_readers_;
  std::map<int, std::shared_ptr<FakePipe>> pipe_writers_;
  std::map<int, HeldEnd> held_ends_;
  std::map<int, platform::Readiness> scripted_readiness_;
  std::map<int, int> close_errors_;
  std::deque<WriteStep> writes_;
  int next_pipe_fd_ = kFirstPipeFd;
  int pipe_error_ = 0;
  int pipes_created_ = 0;
  std::optional<std::pair<int, int>> pipe_failure_at_;
  std::optional<std::pair<int, int>> poll_failure_at_;
  int poll_error_ = 0;
  int persistent_write_error_ = 0;
  int write_count_ = 0;
  int poll_count_ = 0;
  std::size_t pipe_read_limit_ = std::numeric_limits<std::size_t>::max();
  std::vector<int> poll_timeouts_;

  static platform::NodeMask node_zero() {
    platform::NodeMask mask;
    mask.nodes.set(0);
    return mask;
  }

 public:
  /// The pid the fake reports as the parent unless a test says otherwise.
  /// Tests pass this to become_worker as the captured supervisor pid, so the
  /// race check passes by default and a test states only the case it is about.
  static constexpr pid_t kFakeSupervisorPid = 1000;
};

}  // namespace loadforge::testing

#endif
