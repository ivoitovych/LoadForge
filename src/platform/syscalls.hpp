// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_SYSCALLS_HPP
#define LOADFORGE_PLATFORM_SYSCALLS_HPP

#include <sys/types.h>

#include <array>
#include <bitset>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"

namespace loadforge::platform {

/// A child that waitpid(2) reaped: which one, and the raw status word.
///
/// The two travel together because neither is usable alone. A status word with
/// no pid cannot be attributed to a worker, and a pid with no status says only
/// that something ended.
struct Reaped {
  pid_t pid = 0;
  int status = 0;  ///< Raw; decode with ExitStatus, which keeps the raw value.

  /// Defaulted out of line, in syscalls.cpp, so there is one definition and so
  /// one coverage record: a comparison defaulted here is copied into every
  /// unit that uses it, the linker keeps one copy, and the others are reported
  /// at zero (journal §1.22). This one and CpuMask's showed it.
  friend bool operator==(const Reaped&, const Reaped&);
};

/// The declaration the attribute can appertain to: a friend declaration that
/// is not a definition cannot carry one.
[[nodiscard]] bool operator==(const Reaped&, const Reaped&);

/// What fork(2)'s return value means: a negative pid is the failure `number`
/// names; anything else is the answer -- 0 in the child, the child's pid in the
/// parent -- which WorkerLauncher::fork_worker tells apart (process.hpp).
///
/// RealSyscalls::fork_process is the bare call and hands its answer here. The
/// decision is a function of its own so that a test reaches every arm without
/// creating a process: fork's failures cannot be induced in a test process
/// (result_or_error says why), and its successes cannot be seen without
/// forking, which the mutation sweep's tests may not do (docs/JOURNAL.md §2.28).
///
/// It runs in the child too, between fork and exec. The success arm therefore
/// allocates nothing; only the failure arm, which no child reaches, builds the
/// error's subject.
[[nodiscard]] core::Result<pid_t, SyscallError> fork_result(pid_t pid, int number);

/// What waitpid(-1, &status, WNOHANG)'s return value means: a negative pid is
/// the failure `number` names (ECHILD when there are no children at all), 0 is
/// children of which none has terminated, and a positive pid is the child
/// reaped, with its status word. Above the seam for the reason fork_result is:
/// RealSyscalls::reap_any_nonblocking stays the bare call.
[[nodiscard]] core::Result<std::optional<Reaped>, SyscallError> reap_result(pid_t pid, int status,
                                                                            int number);

/// What waitid(P_PID, pid, WEXITED | WNOHANG | WNOWAIT)'s return value means: a
/// negative return is the failure `number` names (ECHILD for a pid that is not
/// an unreaped child); otherwise `reported_pid` is what the kernel wrote into
/// siginfo, which is 0 when the child has not terminated and the pid when it
/// has. Above the seam for the reason fork_result is.
[[nodiscard]] core::Result<bool, SyscallError> exit_peek_result(int returned, pid_t reported_pid,
                                                                int number);

/// One clock reading, exactly as clock_gettime(2) reports it.
///
/// The seconds and nanoseconds stay separate here rather than being combined
/// below the seam: the combination can overflow and the nanoseconds field can
/// be out of range on a broken clock, and both of those are decisions that
/// belong where a test can force them.
struct TimeSpec {
  std::int64_t seconds = 0;
  std::int64_t nanoseconds = 0;

  [[nodiscard]] friend bool operator==(const TimeSpec&, const TimeSpec&) = default;
};

/// Width of a CPU affinity mask: the kernel's CPU_SETSIZE, which glibc fixes
/// at 1024. Named here rather than taken from <sched.h> so the seam's header
/// stays free of it; syscalls.cpp static_asserts the two agree.
inline constexpr std::size_t kAffinityMaskBits = 1024;

/// A CPU affinity mask exactly as sched_getaffinity(2) reports it and
/// sched_setaffinity(2) accepts it: one bit per CPU id.
///
/// The seam exchanges the raw bits and nothing else. Which CPUs are online,
/// which the cpuset permits, and what an EINVAL therefore means are all
/// decisions, and decisions live above the seam where a test can reach both
/// arms.
struct CpuMask {
  std::bitset<kAffinityMaskBits> cpus;

  /// Defaulted out of line, for the reason Reaped's comparison is.
  friend bool operator==(const CpuMask&, const CpuMask&);
};

[[nodiscard]] bool operator==(const CpuMask&, const CpuMask&);

/// Width of a NUMA node mask. No Linux kernel numbers a node past 1023
/// (CONFIG_NODES_SHIFT is at most 10), so a mask this wide names every node any
/// kernel can have. Named here for the same reason kAffinityMaskBits is.
inline constexpr std::size_t kNodeMaskBits = 1024;

/// A NUMA node mask exactly as mbind(2) accepts it: one bit per node id.
///
/// The seam exchanges the raw bits and nothing else. Which nodes exist, which
/// have memory, and what an EINVAL therefore means are decisions, and they
/// live above the seam.
struct NodeMask {
  std::bitset<kNodeMaskBits> nodes;

  [[nodiscard]] friend bool operator==(const NodeMask&, const NodeMask&) = default;
};

/// A CpuMask as the bytes of a cpu_set_t: CPU n is bit n % CHAR_BIT of byte
/// n / CHAR_BIT, the layout glibc documents and syscalls.cpp static_asserts.
///
/// The conversions are declared here, rather than kept private to the seam,
/// so that a test can hold them to glibc's own CPU_SET and CPU_ISSET on every
/// one of the 1024 bits without asking the kernel to move the test process.
/// Through the kernel, a conversion that dropped every CPU but 0 passes on a
/// machine whose only CPU is 0, and on any machine once a test has pinned
/// itself there.
using CpuMaskBytes = std::array<unsigned char, kAffinityMaskBits / CHAR_BIT>;
[[nodiscard]] CpuMask cpu_mask_from_bytes(const CpuMaskBytes& bytes);
[[nodiscard]] CpuMaskBytes cpu_mask_to_bytes(const CpuMask& mask);

/// A NodeMask as the array of unsigned long that mbind(2) reads: node n is bit
/// n % bits-per-long of word n / bits-per-long, built word by word rather than
/// by copying bytes, so the layout holds on either byte order. Declared here
/// for the reason the CPU conversions are: through the kernel, a machine with
/// one node cannot tell most wrong layouts from the right one.
using NodeMaskWords = std::array<unsigned long, kNodeMaskBits / (sizeof(unsigned long) * CHAR_BIT)>;
[[nodiscard]] NodeMaskWords node_mask_to_words(const NodeMask& nodes);

/// The two ends of a pipe, as pipe2(2) hands them out.
struct PipeEnds {
  int read_fd = -1;
  int write_fd = -1;

  [[nodiscard]] friend bool operator==(const PipeEnds&, const PipeEnds&) = default;
};

/// What poll(2) reported for one descriptor, as four facts rather than a bit
/// mask: readable (POLLIN), the far end gone (POLLHUP), an error condition
/// (POLLERR), or a descriptor that is not open at all (POLLNVAL). The kernel
/// sets several at once -- a pipe whose writer closed with bytes still queued
/// is readable AND hung up -- so these are not alternatives, and deciding what
/// a combination means is the controller's job, above the seam.
struct Readiness {
  bool readable = false;
  bool hung_up = false;
  bool error = false;
  bool invalid = false;

  [[nodiscard]] friend bool operator==(const Readiness&, const Readiness&) = default;
};

/// A hardware or software counter to open with perf_event_open(2): which PMU
/// (`type`), which event on it (`config`), and what to count.
///
/// `pid` and `cpu` are the syscall's own pair: pid 0 with cpu -1 counts the
/// calling process on whichever CPU it runs, pid -1 with a cpu counts every
/// process on that CPU (privileged). `user_only` sets exclude_kernel and
/// exclude_hv, which is what lets an unprivileged process count itself at
/// perf_event_paranoid 2; a PMU that counts the whole package (the power PMU)
/// refuses them with EINVAL, which is why they are a choice and not a default.
/// `inherit` makes children forked after the open count into the same
/// descriptor. The event is opened disabled, close-on-exec, with total time
/// enabled and running in its read format, so a multiplexed counter can be
/// scaled (perf_event_open(2), PERF_FORMAT_TOTAL_TIME_*).
struct PerfEventSpec {
  std::uint32_t type = 0;
  std::uint64_t config = 0;
  pid_t pid = 0;
  int cpu = -1;
  bool user_only = false;
  bool inherit = false;
};

/// What read(2) on a counter returns with the read format above: the count,
/// and for how long the event existed and ran. They differ when the kernel
/// time-shared the PMU between more events than it has counters.
struct PerfCount {
  std::uint64_t value = 0;
  std::uint64_t time_enabled = 0;  ///< Nanoseconds since the event was enabled.
  std::uint64_t time_running = 0;  ///< Nanoseconds it was on a counter.

  /// Defined in syscalls.cpp, once: a defaulted comparison in a header reads as
  /// uncovered the moment a second translation unit uses it (journal §1.22).
  friend bool operator==(const PerfCount& lhs, const PerfCount& rhs) noexcept;
};

/// The three ioctl(2) requests a counter needs, as perf_event_open(2) names them.
enum class PerfAction : std::uint8_t { kReset, kEnable, kDisable };

/// The seam. Every system call the platform layer issues goes through here.
///
/// WHY THIS INTERFACE EXISTS, AND WHY IT IS EXACTLY THIS NARROW
/// -----------------------------------------------------------
/// docs/IMPLEMENTATION.md §2.2 classifies execution paths, and class P2 --
/// "each documented failure mode of each fallible call" -- is invisible to a
/// coverage tool. An `EACCES` from open(), a short read(), an `EINTR`: none is
/// a branch gcov can report as uncovered, because the success case covers the
/// branch and the failure arm simply never runs. Coverage reads 100% and means
/// it.
///
/// The only way to reach those arms is to *force* them, and the only way to
/// force them is for the call to be substitutable. That is what this is for.
/// Every P2 path in every module built after this one is testable because this
/// seam exists; building it wrong here means retrofitting testability into ten
/// modules later (docs/IMPLEMENTATION.md §4, M1).
///
/// It is deliberately NOT an abstraction over the operating system.
/// docs/PLAN.md §4.2 rejects that: reading a TOML config or writing run.json is
/// ordinary file I/O, tested with temporary directories, and does not belong
/// here. This interface carries only what the platform layer itself calls --
/// hardware introspection, affinity, memory policy, process control -- and it
/// grows one method at a time, each with its own error-mode tests.
///
/// The virtual dispatch costs a vtable lookup against a syscall that costs
/// microseconds, which is not a trade worth thinking about.
///
/// A call added after M1 has an ENOSYS default so that adding it breaks no
/// implementation; the real implementation and any fake that models it
/// override it. Above the seam ENOSYS already means "the kernel does not offer
/// this", which is what a fake that does not model the call should look like.
class Syscalls {
 public:
  Syscalls() = default;
  Syscalls(const Syscalls&) = delete;
  Syscalls(Syscalls&&) = delete;
  Syscalls& operator=(const Syscalls&) = delete;
  Syscalls& operator=(Syscalls&&) = delete;
  virtual ~Syscalls() = default;

  /// open(2) with O_RDONLY|O_CLOEXEC. Returns the descriptor or the errno.
  ///
  /// O_CLOEXEC is not optional: the controller forks workers, and a descriptor
  /// leaked across exec into a worker is both a resource leak and a way for a
  /// worker to hold a file the controller believes it closed.
  [[nodiscard]] virtual core::Result<int, SyscallError> open_read(const std::string& path) = 0;

  /// read(2). Returns the byte count, which may be short or zero at end of
  /// file; a short read is not an error and callers must loop.
  [[nodiscard]] virtual core::Result<std::size_t, SyscallError> read(int fd, char* buffer,
                                                                     std::size_t size) = 0;

  /// close(2). The result is reported rather than discarded: a failing close
  /// can mean data was lost, and the platform layer does not decide for its
  /// caller whether that matters.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> close(int fd) = 0;

  /// readlink(2) into `buffer`, returning the byte count written.
  ///
  /// readlink does NOT null-terminate, and it does not report truncation: when
  /// the target is longer than the buffer it fills the buffer completely and
  /// returns the buffer size, indistinguishable from an exact fit. Detecting
  /// that is the caller's job and is why this returns the raw count rather than
  /// a string -- a wrapper that hid the count would hide the only evidence of
  /// truncation there is.
  [[nodiscard]] virtual core::Result<std::size_t, SyscallError> read_link(const std::string& path,
                                                                          char* buffer,
                                                                          std::size_t size) = 0;

  /// fork(2). Returns 0 in the child and the child's pid in the parent.
  ///
  /// Callers above the seam must not test this against 0 themselves --
  /// WorkerLauncher::fork_worker wraps it in a type that makes the two cases
  /// impossible to confuse (process.hpp).
  [[nodiscard]] virtual core::Result<pid_t, SyscallError> fork_process() = 0;

  /// execv(2), replacing the current image.
  ///
  /// Returns ONLY a failure, because a successful exec does not return: there is
  /// no success value this could carry. Modelling it as Result<Ok, E> would
  /// invent a success branch that cannot occur and that no test could ever take.
  [[nodiscard]] virtual SyscallError exec(const std::string& path,
                                          const std::vector<std::string>& argv) = 0;

  /// prctl(PR_SET_PDEATHSIG, signal) -- ask the kernel to deliver `signal` to
  /// this process when its parent dies. Set in the child, after fork.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> set_parent_death_signal(
      int signal) = 0;

  /// getppid(2). Cannot fail.
  ///
  /// Needed because PR_SET_PDEATHSIG has a race: the parent can die between the
  /// fork and the prctl, and the signal then never arrives because there is no
  /// longer a parent to die. Re-reading the parent pid afterwards is the only
  /// way to see that it already happened.
  [[nodiscard]] virtual pid_t parent_pid() = 0;

  /// setsid(2): makes this process the leader of a new session, and of a new
  /// process group whose id is its pid. Set in the child, after fork, so that
  /// everything a worker starts shares a group the supervisor can signal as
  /// one -- kill(-pid) -- after the worker itself is gone.
  ///
  /// Returns nothing, because where it is called it cannot fail: setsid
  /// refuses only a process that already leads a process group, and the child
  /// of a fork never does (its group is its parent's, whose leader is another
  /// process). A failure arm here would be a branch no test could take
  /// (IMPLEMENTATION §2.6, rung 3). Called anywhere else, it may be refused,
  /// silently; nothing above the seam does that.
  virtual void start_session() = 0;

  /// waitpid(-1, &status, 0) -- reap whichever child terminated first.
  ///
  /// Returns the pid AND the raw status word together, because they are only
  /// meaningful as a pair: a status word with no pid attached cannot be
  /// attributed to a worker, and a supervisor that reported "a worker died" for
  /// a child some library forked would be inventing evidence.
  [[nodiscard]] virtual core::Result<Reaped, SyscallError> wait_any() = 0;

  /// kill(2).
  ///
  /// A success proves only that the signal was queued. A child that has
  /// terminated and has not been reaped is a zombie, and the kernel accepts a
  /// signal to a zombie and discards it: success here says nothing about the
  /// signal having caused, or even preceded, the child's death (journal §1.27).
  /// ESRCH comes only once the child is reaped.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> send_signal(pid_t pid, int signal) = 0;

  /// waitid(P_PID, pid, WEXITED | WNOHANG | WNOWAIT): whether the child has
  /// terminated, without reaping it. True for a zombie. ECHILD -- not a child,
  /// or already reaped -- is an error, as in the reaps: a supervisor asking
  /// about a worker it still tracks has lost track of it. The default fails
  /// with ENOSYS, so that adding the call broke no fake.
  [[nodiscard]] virtual core::Result<bool, SyscallError> has_exited(pid_t pid);

  /// clock_gettime(2) for the given clock id.
  ///
  /// Takes the raw clock id rather than an enum so the seam stays a translation
  /// and nothing else; naming the clocks, and arguing which one answers which
  /// question, is Clock's job above the seam where it can be tested.
  [[nodiscard]] virtual core::Result<TimeSpec, SyscallError> read_clock(int clock_id) = 0;

  /// sched_getaffinity(2). pid 0 means the calling thread.
  [[nodiscard]] virtual core::Result<CpuMask, SyscallError> get_affinity(pid_t pid) = 0;

  /// sched_setaffinity(2). pid 0 means the calling thread.
  ///
  /// The kernel intersects the mask with what the cpuset permits and refuses
  /// with EINVAL when nothing is left. It also, for a privileged caller, moves
  /// ANY process -- verified: as root, pinning pid 1 succeeded and had to be
  /// undone. Nothing above the seam passes a pid it did not fork.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> set_affinity(pid_t pid,
                                                                          const CpuMask& mask) = 0;

  /// mmap(2) of `length` bytes of anonymous, private, readable and writable
  /// memory, with `extra_flags` added to MAP_PRIVATE|MAP_ANONYMOUS. The one
  /// extra flag anything above the seam passes is MAP_HUGETLB with its size.
  ///
  /// Takes the raw flags rather than an enum for the reason read_clock takes a
  /// raw clock id: the seam stays a translation. Deciding which huge-page
  /// strategy to ask for is Memory's job, above the seam, where it is tested.
  [[nodiscard]] virtual core::Result<void*, SyscallError> map_anonymous(std::size_t length,
                                                                        int extra_flags) = 0;

  /// munmap(2). Reported rather than discarded, because a failing unmap is a
  /// leak the caller may need to know about -- and for a huge-page mapping
  /// it is the answer to an unrounded length (verified: EINVAL).
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> unmap(void* address,
                                                                   std::size_t length) = 0;

  /// mlock(2). Faults the pages in and pins them, subject to RLIMIT_MEMLOCK
  /// for an unprivileged caller and to nothing for a privileged one.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> lock_memory(void* address,
                                                                         std::size_t length) = 0;

  /// madvise(2) with the raw advice value: MADV_HUGEPAGE, MADV_POPULATE_WRITE.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> advise_memory(void* address,
                                                                           std::size_t length,
                                                                           int advice) = 0;

  /// mbind(2) with MPOL_BIND: every page of the range must come from `nodes`.
  /// Issued before the range is touched, since the policy governs the faults.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> bind_memory(void* address,
                                                                         std::size_t length,
                                                                         const NodeMask& nodes) = 0;

  /// memfd_create(2) with MFD_CLOEXEC, sized with ftruncate(2). Returns the
  /// descriptor. This is how a region of memory survives exec: the worker is
  /// a fresh image, so it cannot inherit a mapping, but it can inherit the
  /// descriptor and map the same file. Close-on-exec by default, so no child
  /// but the intended one carries it; inherit_across_exec hands it over.
  [[nodiscard]] virtual core::Result<int, SyscallError> create_memory_file(const std::string& name,
                                                                           std::size_t length) = 0;

  /// mmap(2) of `length` bytes of `fd`, MAP_SHARED, readable and writable,
  /// from offset 0. Writes are visible to every process mapping the same
  /// file, which is the whole point.
  [[nodiscard]] virtual core::Result<void*, SyscallError> map_shared(int fd,
                                                                     std::size_t length) = 0;

  /// fcntl(F_SETFD, 0): clears FD_CLOEXEC so the descriptor survives exec.
  /// Called in the child between fork and exec for exactly the descriptors
  /// that worker needs, and no other.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> inherit_across_exec(int fd) = 0;

  /// pipe2(2) with O_CLOEXEC on both ends. A worker's record channel: the
  /// controller keeps the read end, the worker inherits the write end, and
  /// end of file on the read end is the one signal that cannot be faked --
  /// it arrives when the last writer is gone, however it went.
  [[nodiscard]] virtual core::Result<PipeEnds, SyscallError> create_pipe() = 0;

  /// write(2). Returns the byte count, which may be short; callers loop. The
  /// mirror of read: a short write is not an error. Takes bytes rather than
  /// chars because what crosses a record pipe is a frame, not text.
  [[nodiscard]] virtual core::Result<std::size_t, SyscallError> write(
      int fd, std::span<const std::byte> bytes) = 0;

  /// poll(2) for POLLIN on each descriptor, waiting at most `timeout_ms`
  /// (negative waits forever, zero does not wait). One Readiness per
  /// descriptor, in the order given. The timeout is the controller's
  /// heartbeat interval: this is the one blocking call in its loop.
  [[nodiscard]] virtual core::Result<std::vector<Readiness>, SyscallError> poll_readable(
      const std::vector<int>& fds, int timeout_ms) = 0;

  /// sigaction(2) setting the signal's disposition to SIG_IGN. A worker
  /// ignores SIGPIPE so that a write to a controller that has gone is a
  /// reported EPIPE rather than a death by signal 13, which a supervisor
  /// could not tell from a fault. The kernel refuses for SIGKILL and SIGSTOP
  /// (EINVAL), which is the failing arm's real-kernel test.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> ignore_signal(int signal) = 0;

  /// sigaction(2) setting the signal's disposition to SIG_DFL, with no mask
  /// and no flags. The controller does it for SIGCHLD before its first fork:
  /// a disposition of SIG_IGN is inherited across exec, and with it the kernel
  /// reaps a child at its exit, so every waitpid(2) after the last child is
  /// gone fails with ECHILD and the run is aborted (review C2). The kernel
  /// refuses for SIGKILL and SIGSTOP (EINVAL).
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> default_signal(int signal) = 0;

  /// getpid(2). Cannot fail. A worker stamps its beats with it so a reader
  /// can tell a slot's writer from a stale pid the controller remembers.
  [[nodiscard]] virtual pid_t process_id() = 0;

  /// waitpid(-1, WNOHANG): a child that has terminated, if one has. The
  /// controller's loop owns a poll and must never block in waitpid; a
  /// worker's exit closes its record pipe, which wakes the poll, and this is
  /// asked afterwards. ECHILD -- no children at all -- is an error here as
  /// in wait_any, because a supervisor that believes it has live workers and
  /// is told otherwise has lost track of them.
  [[nodiscard]] virtual core::Result<std::optional<Reaped>, SyscallError>
  reap_any_nonblocking() = 0;

  /// perf_event_open(2) with group -1 and PERF_FLAG_FD_CLOEXEC. The failures
  /// the man page documents are E2BIG, EACCES, EBADF, EBUSY, EFAULT, EINVAL,
  /// EINTR, EMFILE, ENODEV, ENOENT, ENOSPC, ENOSYS, EOPNOTSUPP, EOVERFLOW,
  /// EPERM and ESRCH; what each means for a source is decided above the seam.
  /// The default fails with ENOSYS (no perf events).
  [[nodiscard]] virtual core::Result<int, SyscallError> open_perf_event(const PerfEventSpec& spec);

  /// ioctl(2) PERF_EVENT_IOC_RESET, _ENABLE or _DISABLE on a counter. The
  /// default fails with ENOSYS.
  [[nodiscard]] virtual core::Result<core::Ok, SyscallError> control_perf_event(int fd,
                                                                                PerfAction action);

  /// read(2) of a counter: its count and its two times. A counter is read as a
  /// whole or not at all, so a short read is an error here (EIO), unlike
  /// read() above where it is normal. The default fails with ENOSYS.
  [[nodiscard]] virtual core::Result<PerfCount, SyscallError> read_perf_count(int fd);
};

/// The real implementation. Contains no logic beyond translating errno into a
/// SyscallError, because anything with a decision in it belongs above the seam
/// where it can be tested (docs/PLAN.md §4.3).
class RealSyscalls final : public Syscalls {
 public:
  [[nodiscard]] core::Result<int, SyscallError> open_read(const std::string& path) override;
  [[nodiscard]] core::Result<std::size_t, SyscallError> read(int fd, char* buffer,
                                                             std::size_t size) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> close(int fd) override;
  [[nodiscard]] core::Result<std::size_t, SyscallError> read_link(const std::string& path,
                                                                  char* buffer,
                                                                  std::size_t size) override;
  [[nodiscard]] core::Result<pid_t, SyscallError> fork_process() override;
  [[nodiscard]] SyscallError exec(const std::string& path,
                                  const std::vector<std::string>& argv) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> set_parent_death_signal(int signal) override;
  [[nodiscard]] pid_t parent_pid() override;
  void start_session() override;
  [[nodiscard]] core::Result<Reaped, SyscallError> wait_any() override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> send_signal(pid_t pid, int signal) override;
  [[nodiscard]] core::Result<bool, SyscallError> has_exited(pid_t pid) override;
  [[nodiscard]] core::Result<TimeSpec, SyscallError> read_clock(int clock_id) override;
  [[nodiscard]] core::Result<CpuMask, SyscallError> get_affinity(pid_t pid) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> set_affinity(pid_t pid,
                                                                  const CpuMask& mask) override;
  [[nodiscard]] core::Result<void*, SyscallError> map_anonymous(std::size_t length,
                                                                int extra_flags) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> unmap(void* address,
                                                           std::size_t length) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> lock_memory(void* address,
                                                                 std::size_t length) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> advise_memory(void* address,
                                                                   std::size_t length,
                                                                   int advice) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> bind_memory(void* address, std::size_t length,
                                                                 const NodeMask& nodes) override;
  [[nodiscard]] core::Result<int, SyscallError> create_memory_file(const std::string& name,
                                                                   std::size_t length) override;
  [[nodiscard]] core::Result<void*, SyscallError> map_shared(int fd, std::size_t length) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> inherit_across_exec(int fd) override;
  [[nodiscard]] core::Result<PipeEnds, SyscallError> create_pipe() override;
  [[nodiscard]] core::Result<std::size_t, SyscallError> write(
      int fd, std::span<const std::byte> bytes) override;
  [[nodiscard]] core::Result<std::vector<Readiness>, SyscallError> poll_readable(
      const std::vector<int>& fds, int timeout_ms) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> ignore_signal(int signal) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> default_signal(int signal) override;
  [[nodiscard]] pid_t process_id() override;
  [[nodiscard]] core::Result<std::optional<Reaped>, SyscallError> reap_any_nonblocking() override;
  [[nodiscard]] core::Result<int, SyscallError> open_perf_event(const PerfEventSpec& spec) override;
  [[nodiscard]] core::Result<core::Ok, SyscallError> control_perf_event(int fd,
                                                                        PerfAction action) override;
  [[nodiscard]] core::Result<PerfCount, SyscallError> read_perf_count(int fd) override;
};

}  // namespace loadforge::platform

#endif
