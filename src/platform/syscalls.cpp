// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/syscalls.hpp"

#include <fcntl.h>
#include <linux/mempolicy.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace loadforge::platform {
namespace {

/// Descriptors render as "fd 7" rather than as a bare number, so that a message
/// reading "read(fd 7): Bad file descriptor" cannot be mistaken for a path.
std::string describe_fd(int fd) { return "fd " + std::to_string(fd); }

}  // namespace

// Every function below is a translation and nothing else. There is no decision
// here to test, which is the point: docs/PLAN.md §4.3 keeps the layer that
// cannot be driven from a fixture as close to zero logic as possible, and puts
// every branch above the seam where a test can force it.
//
// These bodies are therefore NOT unit-tested against a fake -- there is nothing
// to fake them with, since they are the thing being faked. They are covered by
// the integration tier (T8), which reads a real file through a real descriptor,
// and by tests/unit/platform/real_syscalls_test.cpp, which drives them against
// genuine paths in a temporary directory, including genuine failures: a missing
// file for ENOENT, a directory for EISDIR, and a closed descriptor for EBADF.

core::Result<int, SyscallError> RealSyscalls::open_read(const std::string& path) {
  // O_CLOEXEC matters here beyond hygiene: the controller forks workers, and a
  // descriptor that survives exec is one the controller believes it closed.
  // open(2) is declared variadic because of its optional mode argument. POSIX
  // provides no non-variadic spelling -- openat is variadic too -- so this is
  // the one place the rule cannot be satisfied rather than merely inconvenient.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return SyscallError{errno, "open", path};
  }
  return fd;
}

core::Result<std::size_t, SyscallError> RealSyscalls::read(int fd, char* buffer, std::size_t size) {
  const ssize_t count = ::read(fd, buffer, size);
  if (count < 0) {
    return SyscallError{errno, "read", describe_fd(fd)};
  }
  return static_cast<std::size_t>(count);
}

core::Result<core::Ok, SyscallError> RealSyscalls::close(int fd) {
  if (::close(fd) != 0) {
    return SyscallError{errno, "close", describe_fd(fd)};
  }
  return core::Ok{};
}

core::Result<std::size_t, SyscallError> RealSyscalls::read_link(const std::string& path,
                                                                char* buffer, std::size_t size) {
  const ssize_t count = ::readlink(path.c_str(), buffer, size);
  // -1 is readlink's one failure value, and it is tested for exactly: `< 0`
  // would make 0 a boundary, and no input reaches it -- a symlink cannot have
  // an empty target, and a zero-sized buffer is refused with EINVAL.
  if (count == -1) {
    return SyscallError{errno, "readlink", path};
  }
  return static_cast<std::size_t>(count);
}

// The answer and its errno are two ints in a row: the check is right that they
// could be swapped, and (what the call returned, what it set) is the order the
// call produces them in. The tests give each a distinct value.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
core::Result<pid_t, SyscallError> fork_result(pid_t pid, int number) {
  if (pid < 0) {
    return SyscallError{number, "fork", "worker process"};
  }
  return pid;
}

// Three ints in a row, for the reason fork_result has two.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
core::Result<std::optional<Reaped>, SyscallError> reap_result(pid_t pid, int status, int number) {
  if (pid < 0) {
    return SyscallError{number, "waitpid", "any child, without waiting"};
  }
  if (pid == 0) {
    return std::optional<Reaped>{};  // Children, but none has terminated.
  }
  return std::optional<Reaped>{Reaped{pid, status}};
}

core::Result<pid_t, SyscallError> RealSyscalls::fork_process() {
  // The bare call. What its answer means is fork_result's to decide, above the
  // seam, where a test reaches every arm without creating a process. In the
  // child nothing here allocates: the errno read and the call below are all
  // that runs before the caller has its 0.
  const pid_t pid = ::fork();
  const int number = errno;
  return fork_result(pid, number);
}

SyscallError RealSyscalls::exec(const std::string& path, const std::vector<std::string>& argv) {
  // execv takes char* const[], not const char* const[], for historical reasons;
  // it does not modify the strings. const_cast is the documented way to call it
  // and is why this lives below the seam rather than in code under test.
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    raw.push_back(
        const_cast<char*>(argument.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  }
  raw.push_back(nullptr);

  ::execv(path.c_str(), raw.data());
  // Only reachable because execv failed: on success this image is gone.
  return SyscallError{errno, "execv", path};
}

core::Result<core::Ok, SyscallError> RealSyscalls::set_parent_death_signal(int signal) {
  // prctl(2) is variadic, like open(2), and POSIX offers no non-variadic
  // spelling. See the note in open_read.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::prctl(PR_SET_PDEATHSIG, signal) != 0) {
    return SyscallError{errno, "prctl", "PR_SET_PDEATHSIG"};
  }
  return core::Ok{};
}

pid_t RealSyscalls::parent_pid() { return ::getppid(); }

// Its answer is dropped by contract: see the declaration. In the one place it
// is called, a freshly forked child, setsid cannot refuse.
void RealSyscalls::start_session() { (void)::setsid(); }

core::Result<Reaped, SyscallError> RealSyscalls::wait_any() {
  int status = 0;
  const pid_t pid = ::waitpid(-1, &status, 0);
  // Branch-free on purpose: waitpid's failures are ECHILD (no children at all)
  // and EINTR, and the second cannot be produced on demand in a test process
  // without racing the test itself. The decision lives above the seam in
  // result_or_error, where both arms are reachable. The test is for -1, the
  // one failure value, rather than for `< 0`: a wait that blocks never answers
  // 0, so that boundary would be one no input reaches.
  return result_or_error<Reaped>(Reaped{pid, status}, pid == -1, errno, "waitpid", "any child");
}

core::Result<core::Ok, SyscallError> RealSyscalls::send_signal(pid_t pid, int signal) {
  // The call and the errno read are SEPARATE STATEMENTS, deliberately.
  //
  // Written as `result_or_error(core::Ok{}, ::kill(...) != 0, errno, ...)` this
  // compiled, looked tidier, and was wrong: the order in which a function's
  // arguments are evaluated is unsequenced, so `errno` could be read before
  // ::kill ran and the reported error was whatever the last unrelated syscall
  // had left behind. It reported ENOENT for a kill that failed with ESRCH,
  // because the test fixture had touched the filesystem first.
  //
  // The real-kernel test is what caught it; against the fake this code is not
  // even reached. fork_process and wait_any escape the same trap only because
  // their syscall already sits on its own line.
  const bool failed = ::kill(pid, signal) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "kill",
                                   "pid " + std::to_string(pid));
}

core::Result<TimeSpec, SyscallError> RealSyscalls::read_clock(int clock_id) {
  timespec value{};
  const bool failed = ::clock_gettime(clock_id, &value) != 0;
  const int number = errno;
  // The fields are carried across as they are. Combining them into nanoseconds
  // here would put an overflow check below the seam, where no test could reach
  // its failing arm -- see result_or_error's note, and Clock::to_nanoseconds.
  return result_or_error<TimeSpec>(
      TimeSpec{static_cast<std::int64_t>(value.tv_sec), static_cast<std::int64_t>(value.tv_nsec)},
      failed, number, "clock_gettime", "clock " + std::to_string(clock_id));
}

// The seam's mask width is a plain constant so the header stays free of
// <sched.h>. This is where the two are held to the same value -- and where the
// LAYOUT the conversions below rely on is pinned: a cpu_set_t is exactly
// CPU_SETSIZE bits of storage, bit `cpu` living in byte cpu/8 at bit cpu%8.
static_assert(kAffinityMaskBits == CPU_SETSIZE,
              "CpuMask must be exactly as wide as the kernel's cpu_set_t");
static_assert(sizeof(cpu_set_t) * CHAR_BIT == CPU_SETSIZE,
              "cpu_set_t must be a plain bit array with no padding");

static_assert(sizeof(CpuMaskBytes) == sizeof(cpu_set_t),
              "CpuMaskBytes must be exactly the storage of a cpu_set_t");

// The conversions read and write the set's bytes directly rather than through
// CPU_ISSET and CPU_SET. Those macros carry a size guard -- `cpu / 8 < setsize`
// -- that can never be false for a fixed-size cpu_set_t, so every use of them
// left a branch no test could take, and the coverage gate correctly refused
// to call it covered. Rung 3 of the exclusion ladder: delete the unreachable
// code rather than excuse it. The layout this assumes is the one glibc
// documents and the static_asserts above pin; the tests cross-check every one
// of the 1024 bits against CPU_SET and CPU_ISSET themselves, so the macros are
// still the oracle -- in the test, where an oracle belongs.
CpuMask cpu_mask_from_bytes(const CpuMaskBytes& bytes) {
  CpuMask mask;
  for (std::size_t cpu = 0; cpu < kAffinityMaskBits; ++cpu) {
    // .at(), and the widening cast, are for the checkers rather than for safety:
    // cpu < kAffinityMaskBits keeps the index in range by construction, and
    // unsigned char promotes to int before the shift, which -Wsign-conversion
    // objects to when the result meets 1U.
    const auto byte = static_cast<unsigned int>(bytes.at(cpu / CHAR_BIT));
    if (((byte >> (cpu % CHAR_BIT)) & 1U) != 0U) {
      mask.cpus.set(cpu);
    }
  }
  return mask;
}

CpuMaskBytes cpu_mask_to_bytes(const CpuMask& mask) {
  CpuMaskBytes bytes{};
  for (std::size_t cpu = 0; cpu < kAffinityMaskBits; ++cpu) {
    if (mask.cpus.test(cpu)) {
      bytes.at(cpu / CHAR_BIT) |= static_cast<unsigned char>(1U << (cpu % CHAR_BIT));
    }
  }
  return bytes;
}

namespace {

CpuMask from_raw(const cpu_set_t& raw) {
  CpuMaskBytes bytes{};
  std::memcpy(bytes.data(), &raw, sizeof raw);
  return cpu_mask_from_bytes(bytes);
}

cpu_set_t to_raw(const CpuMask& mask) {
  const CpuMaskBytes bytes = cpu_mask_to_bytes(mask);
  cpu_set_t raw;
  std::memcpy(&raw, bytes.data(), sizeof raw);
  return raw;
}

}  // namespace

core::Result<CpuMask, SyscallError> RealSyscalls::get_affinity(pid_t pid) {
  cpu_set_t raw;
  CPU_ZERO(&raw);
  // Separate statements, for the reason send_signal spells out.
  const bool failed = ::sched_getaffinity(pid, sizeof raw, &raw) != 0;
  const int number = errno;
  return result_or_error<CpuMask>(from_raw(raw), failed, number, "sched_getaffinity",
                                  "pid " + std::to_string(pid));
}

core::Result<core::Ok, SyscallError> RealSyscalls::set_affinity(pid_t pid, const CpuMask& mask) {
  const cpu_set_t raw = to_raw(mask);
  const bool failed = ::sched_setaffinity(pid, sizeof raw, &raw) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "sched_setaffinity",
                                   "pid " + std::to_string(pid));
}

namespace {

/// The subject every memory call reports: the length, never the address. An
/// address in a message is noise to a user and a different value on every run
/// to a test; the length is what a reader can act on.
std::string describe_length(std::size_t length) { return std::to_string(length) + " bytes"; }

}  // namespace

// The node mask crosses the seam as the array of unsigned long that mbind(2)
// reads, built word by word -- bit n in word n / bits-per-long -- rather than
// by copying bytes, so that the layout holds on either byte order.
static_assert(kNodeMaskBits % (sizeof(unsigned long) * CHAR_BIT) == 0,
              "the node mask must be a whole number of words");

NodeMaskWords node_mask_to_words(const NodeMask& nodes) {
  constexpr std::size_t kBitsPerWord = sizeof(unsigned long) * CHAR_BIT;
  NodeMaskWords words{};
  for (std::size_t node = 0; node < kNodeMaskBits; ++node) {
    if (nodes.nodes.test(node)) {
      words.at(node / kBitsPerWord) |= 1UL << (node % kBitsPerWord);
    }
  }
  return words;
}

core::Result<void*, SyscallError> RealSyscalls::map_anonymous(std::size_t length, int extra_flags) {
  // PROT and the base flags are fixed here because every mapping this project
  // makes is anonymous, private and writable: a test pattern has to be written
  // somewhere. MAP_POPULATE is deliberately not among them -- a populate that
  // fails is silent inside mmap, and Memory populates afterwards with an
  // madvise whose failure is reported.
  void* address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | extra_flags, -1, 0);
  const bool failed = address == MAP_FAILED;
  const int number = errno;
  return result_or_error<void*>(address, failed, number, "mmap", describe_length(length));
}

core::Result<core::Ok, SyscallError> RealSyscalls::unmap(void* address, std::size_t length) {
  const bool failed = ::munmap(address, length) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "munmap", describe_length(length));
}

core::Result<core::Ok, SyscallError> RealSyscalls::lock_memory(void* address, std::size_t length) {
  const bool failed = ::mlock(address, length) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "mlock", describe_length(length));
}

core::Result<core::Ok, SyscallError> RealSyscalls::advise_memory(void* address, std::size_t length,
                                                                 int advice) {
  const bool failed = ::madvise(address, length, advice) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "madvise",
                                   describe_length(length) + ", advice " + std::to_string(advice));
}

core::Result<core::Ok, SyscallError> RealSyscalls::bind_memory(void* address, std::size_t length,
                                                               const NodeMask& nodes) {
  const NodeMaskWords words = node_mask_to_words(nodes);
  // No libnuma: mbind(2) has no glibc wrapper, and linking a library for one
  // call would add a runtime dependency the static release cannot carry. The
  // maxnode argument is one more than the highest bit the kernel should read,
  // which is how the kernel counts it (it decrements before use). syscall(2)
  // is variadic like open(2), and there is no other spelling.
  constexpr unsigned long kMaxNode = kNodeMaskBits + 1;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const long rc = ::syscall(SYS_mbind, address, length, MPOL_BIND, words.data(), kMaxNode, 0);
  const int number = errno;
  const bool failed = rc != 0;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "mbind", describe_length(length));
}

core::Result<int, SyscallError> RealSyscalls::create_memory_file(const std::string& name,
                                                                 std::size_t length) {
  const int fd = ::memfd_create(name.c_str(), MFD_CLOEXEC);
  if (fd < 0) {
    return SyscallError{errno, "memfd_create", name};
  }
  // Sized here rather than by the caller: a descriptor to a zero-length file
  // is a mapping that faults on first touch, a plausible-looking handle to
  // nothing, which is the shape this project refuses to hand out.
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    const int number = errno;
    (void)::close(fd);
    std::string subject = name + ", " + describe_length(length);
    return SyscallError{number, "ftruncate", std::move(subject)};
  }
  return fd;
}

core::Result<void*, SyscallError> RealSyscalls::map_shared(int fd, std::size_t length) {
  void* address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  const bool failed = address == MAP_FAILED;
  const int number = errno;
  return result_or_error<void*>(address, failed, number, "mmap",
                                describe_fd(fd) + ", " + describe_length(length));
}

core::Result<core::Ok, SyscallError> RealSyscalls::inherit_across_exec(int fd) {
  // fcntl(2) is variadic, like open(2) and prctl(2); see the note in open_read.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const bool failed = ::fcntl(fd, F_SETFD, 0) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "fcntl", describe_fd(fd));
}

core::Result<PipeEnds, SyscallError> RealSyscalls::create_pipe() {
  std::array<int, 2> fds{-1, -1};
  // O_CLOEXEC for the same reason open_read sets it: a worker must inherit
  // exactly the descriptors it is given, and nothing else of its parent's.
  if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
    return SyscallError{errno, "pipe2", "record channel"};
  }
  return PipeEnds{fds[0], fds[1]};
}

core::Result<std::size_t, SyscallError> RealSyscalls::write(int fd,
                                                            std::span<const std::byte> bytes) {
  const ssize_t count = ::write(fd, bytes.data(), bytes.size());
  if (count < 0) {
    return SyscallError{errno, "write", describe_fd(fd)};
  }
  return static_cast<std::size_t>(count);
}

core::Result<std::vector<Readiness>, SyscallError> RealSyscalls::poll_readable(
    const std::vector<int>& fds, int timeout_ms) {
  std::vector<pollfd> entries;
  entries.reserve(fds.size());
  for (const int fd : fds) {
    entries.push_back(pollfd{fd, POLLIN, 0});
  }
  // A timeout expiring is not a failure: it is the controller's heartbeat
  // tick, and poll reports it as zero descriptors ready, which is what the
  // all-false Readiness values below say.
  const bool failed = ::poll(entries.data(), entries.size(), timeout_ms) < 0;
  const int number = errno;
  if (failed) {
    return SyscallError{number, "poll", std::to_string(fds.size()) + " descriptors"};
  }
  std::vector<Readiness> readiness;
  readiness.reserve(entries.size());
  for (const pollfd& entry : entries) {
    readiness.push_back(Readiness{(entry.revents & POLLIN) != 0, (entry.revents & POLLHUP) != 0,
                                  (entry.revents & POLLERR) != 0, (entry.revents & POLLNVAL) != 0});
  }
  return readiness;
}

core::Result<core::Ok, SyscallError> RealSyscalls::ignore_signal(int signal) {
  // sigaction rather than signal(2): the latter's semantics vary by libc
  // flags, and this needs exactly one thing, the disposition set to ignore
  // with no mask and no flags.
  struct sigaction action {};
  action.sa_handler = SIG_IGN;  // NOLINT(cppcoreguidelines-pro-type-union-access)
  ::sigemptyset(&action.sa_mask);
  const bool failed = ::sigaction(signal, &action, nullptr) != 0;
  const int number = errno;
  return result_or_error<core::Ok>(core::Ok{}, failed, number, "sigaction",
                                   "signal " + std::to_string(signal));
}

pid_t RealSyscalls::process_id() { return ::getpid(); }

core::Result<std::optional<Reaped>, SyscallError> RealSyscalls::reap_any_nonblocking() {
  int status = 0;
  // The bare call, as fork_process is; reap_result decides what the answer
  // means. Real-kernel tests reach all three of its arms as well, but only one
  // that forks can reach the last two, and the mutation sweep runs none.
  const pid_t pid = ::waitpid(-1, &status, WNOHANG);
  const int number = errno;
  return reap_result(pid, status, number);
}

}  // namespace loadforge::platform
