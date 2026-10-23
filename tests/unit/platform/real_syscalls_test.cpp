// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam's real side, driven against a real kernel.
//
// FakeSyscalls proves the LOGIC above the seam handles every errno. It cannot
// prove that RealSyscalls reports the errno the kernel actually set, because a
// fake agrees with whatever the code believes. This is the F21 shape again: a
// model of an external thing cannot falsify the model. So these tests use real
// paths and provoke real failures.
//
// They need no privilege and no hardware -- a temporary directory is enough to
// produce ENOENT, EISDIR and EBADF, which are the three that matter for the
// translation being right.

#include <fcntl.h>
#include <grp.h>
#include <gtest/gtest.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform/exit_status.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {
namespace {

class RealSyscallsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory = std::filesystem::temp_directory_path() /
                ("loadforge-" + std::to_string(::getpid()) + "-" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(directory);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  std::string write(const std::string& name, const std::string& contents) {
    const std::filesystem::path path = directory / name;
    std::ofstream out(path);
    out << contents;
    out.close();
    return path.string();
  }

  std::filesystem::path directory;
  RealSyscalls syscalls;
};

// The same fixture for the tests below that create a process. A test that
// creates one is named RealProcess*, so the mutation sweep can leave it out:
// a mutant that breaks fork or reap can leave a child holding Mull's pipes,
// which no timeout of Mull's reaches (tools/mutation-shard.py, which also
// checks the name against what each test really does).
class RealProcessSyscallsTest : public RealSyscallsTest {};

/// Frees descriptor 0 for the length of a test and puts standard input back
/// afterwards. A process can start with descriptor 0 closed -- a daemon, or a
/// worker whose parent closed it -- and the kernel then hands 0 to the next
/// descriptor it creates, since it always gives the lowest free number. That
/// is a valid descriptor, and a call returning it has succeeded.
class StandardInputSetAside {
 public:
  StandardInputSetAside() : saved_(::dup(0)) { ::close(0); }
  ~StandardInputSetAside() {
    if (saved_ >= 0) {
      ::dup2(saved_, 0);
      ::close(saved_);
    } else {
      ::close(0);  // It was closed when the test began; leave it so.
    }
  }
  StandardInputSetAside(const StandardInputSetAside&) = delete;
  StandardInputSetAside& operator=(const StandardInputSetAside&) = delete;
  StandardInputSetAside(StandardInputSetAside&&) = delete;
  StandardInputSetAside& operator=(StandardInputSetAside&&) = delete;

 private:
  int saved_;
};

TEST_F(RealSyscallsTest, OpensReadsAndClosesARealFile) {
  const std::string path = write("value", "95000\n");

  auto opened = syscalls.open_read(path);
  ASSERT_TRUE(opened.has_value()) << describe(opened.error());
  const int fd = opened.value();
  EXPECT_GE(fd, 0);

  std::string buffer(64, '\0');
  auto count = syscalls.read(fd, buffer.data(), buffer.size());
  ASSERT_TRUE(count.has_value()) << describe(count.error());
  EXPECT_EQ(buffer.substr(0, count.value()), "95000\n");

  auto eof = syscalls.read(fd, buffer.data(), buffer.size());
  ASSERT_TRUE(eof.has_value());
  EXPECT_EQ(eof.value(), 0U) << "a second read past the end must report EOF, not an error";

  auto closed = syscalls.close(fd);
  EXPECT_TRUE(closed.has_value()) << describe(closed.error());
  EXPECT_EQ(closed.value(), core::Ok{});
}

TEST_F(RealSyscallsTest, ReportsENOENTForAMissingPath) {
  const auto result = syscalls.open_read((directory / "absent").string());

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, ENOENT);
  EXPECT_EQ(result.error().call, "open");
  EXPECT_NE(result.error().subject.find("absent"), std::string::npos);
}

TEST_F(RealSyscallsTest, ADirectoryOpensButFailsToRead) {
  // Worth stating precisely, because the obvious expectation is wrong and this
  // test is what corrected it: on Linux, open(dir, O_RDONLY) SUCCEEDS. EISDIR
  // comes from read(2), not from open(2) -- open only rejects a directory when
  // write access is requested.
  //
  // That matters above the seam: a probe that builds a path one component short
  // gets a valid descriptor and discovers the mistake only when reading, so the
  // reader must handle a mid-stream EISDIR and must still close the descriptor.
  // A fake alone would never have shown this, because the fake would have
  // agreed with whatever the code believed (F21).
  auto opened = syscalls.open_read(directory.string());
  ASSERT_TRUE(opened.has_value()) << "open(dir, O_RDONLY) is expected to succeed";

  std::string buffer(64, '\0');
  const auto read_result = syscalls.read(opened.value(), buffer.data(), buffer.size());
  ASSERT_FALSE(read_result.has_value());
  EXPECT_EQ(read_result.error().number, EISDIR);

  EXPECT_TRUE(syscalls.close(opened.value()).has_value());
}

TEST_F(RealSyscallsTest, FileSystemReportsADirectoryAndStillClosesTheDescriptor) {
  // The consequence of the above, at the level a caller sees: read_file on a
  // directory path fails with EISDIR from the read, not from the open.
  FileSystem fs(syscalls);

  const auto result = fs.read_file(directory.string());

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EISDIR);
  EXPECT_EQ(result.error().call, "read");
}

TEST_F(RealSyscallsTest, ReportsEBADFForAClosedDescriptor) {
  const std::string path = write("value", "x");
  auto opened = syscalls.open_read(path);
  ASSERT_TRUE(opened.has_value());
  const int fd = opened.value();
  ASSERT_TRUE(syscalls.close(fd).has_value());

  std::string buffer(8, '\0');
  const auto read_again = syscalls.read(fd, buffer.data(), buffer.size());
  ASSERT_FALSE(read_again.has_value());
  EXPECT_EQ(read_again.error().number, EBADF);
  EXPECT_EQ(read_again.error().call, "read");

  const auto close_again = syscalls.close(fd);
  ASSERT_FALSE(close_again.has_value());
  EXPECT_EQ(close_again.error().number, EBADF);
  EXPECT_EQ(close_again.error().call, "close");
  // The subject names the descriptor, not a path, so a message cannot be
  // misread as being about a file.
  EXPECT_NE(close_again.error().subject.find("fd "), std::string::npos);
}

TEST_F(RealSyscallsTest, TheDescriptorIsCloseOnExec) {
  // O_CLOEXEC is not hygiene here: the controller forks workers, and a
  // descriptor surviving exec is one the controller believes it closed.
  const std::string path = write("value", "x");
  auto opened = syscalls.open_read(path);
  ASSERT_TRUE(opened.has_value());

  const int flags = ::fcntl(opened.value(), F_GETFD);
  ASSERT_NE(flags, -1);
  EXPECT_TRUE((static_cast<unsigned>(flags) & FD_CLOEXEC) != 0U);

  EXPECT_TRUE(syscalls.close(opened.value()).has_value());
}

TEST_F(RealSyscallsTest, DescriptorZeroIsASuccessfulOpen) {
  const std::string path = write("value", "x");
  const StandardInputSetAside free_zero;
  auto opened = syscalls.open_read(path);
  ASSERT_TRUE(opened.has_value()) << describe(opened.error());
  EXPECT_EQ(opened.value(), 0);
  EXPECT_TRUE(syscalls.close(opened.value()).has_value());
}

// The whole stack, real kernel included: this is the T8 end of the seam, and it
// is what would catch a FileSystem that works perfectly against the fake and
// not at all against Linux.
TEST_F(RealSyscallsTest, FileSystemReadsARealFileThroughTheRealSeam) {
  const std::string path = write("real", "reference\nsecond line\n");
  FileSystem fs(syscalls);

  const auto whole = fs.read_file(path);
  ASSERT_TRUE(whole.has_value()) << describe(whole.error());
  EXPECT_EQ(whole.value(), "reference\nsecond line\n");

  const auto line = fs.read_first_line(path);
  ASSERT_TRUE(line.has_value());
  EXPECT_EQ(line.value(), "reference");
}

TEST_F(RealSyscallsTest, FileSystemReportsARealMissingFile) {
  FileSystem fs(syscalls);
  const auto result = fs.read_file((directory / "not-here").string());

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, ENOENT);
}

TEST_F(RealSyscallsTest, FileSystemReadsAFileLargerThanOneChunk) {
  // Proves the short-read loop against the real kernel rather than only against
  // a fake that was scripted to produce short reads.
  const std::string body(FileSystem::kChunkSize * 3 + 17, 'a');
  const std::string path = write("large", body);
  FileSystem fs(syscalls);

  const auto result = fs.read_file(path);
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value(), body);
}

// --- the process-control half of the seam ------------------------------------
//
// EVERY ONE OF THESE RUNS IN THE TEST PROCESS ITSELF, never in a forked child,
// and that is not a stylistic choice. A child ends with _exit(2), which skips
// the atexit handlers -- including the one gcov installs to write its counters.
// Anything executed only in a child is therefore absent from the coverage
// report, and reads as untested code rather than as lost data. Measured: the
// first version of these tests exercised exec, prctl and getppid exclusively in
// forked children, and the gate reported all three as never executed.
//
// So the calls that CAN be made in the parent are made there. exec is the
// interesting one: a failing exec does not replace the image, so calling it with
// a path that cannot be executed exercises the whole body -- argument marshalling
// included -- and returns normally.

TEST_F(RealSyscallsTest, ReadLinkResolvesARealSymlink) {
  const std::string target = write("target", "contents");
  const std::filesystem::path link = directory / "link";
  std::filesystem::create_symlink(target, link);

  std::string buffer(4096, '\0');
  const auto count = syscalls.read_link(link.string(), buffer.data(), buffer.size());
  ASSERT_TRUE(count.has_value()) << describe(count.error());
  EXPECT_EQ(buffer.substr(0, count.value()), target);
}

TEST_F(RealSyscallsTest, ReadLinkReportsARealMissingPath) {
  std::string buffer(4096, '\0');
  const auto count =
      syscalls.read_link((directory / "not-here").string(), buffer.data(), buffer.size());
  ASSERT_FALSE(count.has_value());
  EXPECT_EQ(count.error().number, ENOENT);
  EXPECT_EQ(count.error().call, "readlink");
}

TEST_F(RealSyscallsTest, ReadLinkOnSomethingThatIsNotASymlinkIsEINVAL) {
  // The kernel's own answer, which a fake could only have asserted by agreeing
  // with whatever this author assumed.
  const std::string regular = write("regular", "not a link");
  std::string buffer(4096, '\0');

  const auto count = syscalls.read_link(regular, buffer.data(), buffer.size());
  ASSERT_FALSE(count.has_value());
  EXPECT_EQ(count.error().number, EINVAL);
}

TEST_F(RealSyscallsTest, ExecReturnsTheKernelsErrnoWhenItCannotReplaceTheImage) {
  // Safe to call here precisely because it fails: a successful exec would end
  // the test process. This covers the argument marshalling too, which no other
  // test can reach without actually exec'ing.
  const SyscallError error =
      syscalls.exec((directory / "not-here").string(), {"loadforge", "--worker", "--slot=0"});
  EXPECT_EQ(error.number, ENOENT);
  EXPECT_EQ(error.call, "execv");
}

TEST_F(RealSyscallsTest, ExecWithNoArgumentsAtAllStillReportsTheKernelsErrno) {
  // The marshalled list is then the terminating null alone, and it is still
  // handed to the kernel, which answers for the path.
  const SyscallError error = syscalls.exec((directory / "not-here").string(), {});
  EXPECT_EQ(error.number, ENOENT);
  EXPECT_EQ(error.call, "execv");
}

TEST_F(RealSyscallsTest, ExecOnADirectoryIsEACCES) {
  // Another real failure, and one worth pinning: a directory is not ENOEXEC or
  // EISDIR here, which is the sort of thing a hand-written expectation gets
  // wrong.
  const SyscallError error = syscalls.exec(directory.string(), {"loadforge"});
  EXPECT_EQ(error.number, EACCES) << "got " << describe(error);
}

TEST_F(RealSyscallsTest, ExecOnAFileWithoutTheExecuteBitIsEACCES) {
  const std::string plain = write("plain", "#!/bin/sh\necho hi\n");
  const SyscallError error = syscalls.exec(plain, {"plain"});
  EXPECT_EQ(error.number, EACCES) << "got " << describe(error);
}

TEST_F(RealSyscallsTest, TheParentDeathSignalCanBeArmedAndCleared) {
  auto armed = syscalls.set_parent_death_signal(SIGKILL);
  EXPECT_TRUE(armed.has_value()) << (armed.has_value() ? "" : describe(armed.error()));

  // Cleared immediately: leaving it set would arm a death signal on the test
  // process for the remainder of the suite.
  auto cleared = syscalls.set_parent_death_signal(0);
  EXPECT_TRUE(cleared.has_value());
}

TEST_F(RealSyscallsTest, AnInvalidDeathSignalIsRejectedByTheKernel) {
  auto armed = syscalls.set_parent_death_signal(-1);
  ASSERT_FALSE(armed.has_value());
  EXPECT_EQ(armed.error().number, EINVAL);
  EXPECT_EQ(armed.error().call, "prctl");
}

TEST_F(RealSyscallsTest, StartingASessionIsRefusedSilentlyToAGroupLeader) {
  // start_session has no answer by contract: in a fresh child it cannot fail.
  // In this process it is made to be refused rather than to succeed, because
  // success would detach the test from its terminal for the rest of the
  // suite: leading a group of its own first (harmless, and already true when a
  // shell started it) is exactly the one condition setsid refuses. The
  // session is unchanged, which is what a silent refusal must leave.
  ASSERT_TRUE(::setpgid(0, 0) == 0 || ::getpgid(0) == ::getpid());
  const pid_t session = ::getsid(0);
  syscalls.start_session();
  EXPECT_EQ(::getsid(0), session);
  EXPECT_EQ(::getpgid(0), ::getpid());
}

TEST_F(RealSyscallsTest, ParentPidMatchesGetppid) {
  EXPECT_EQ(syscalls.parent_pid(), ::getppid());
  EXPECT_GT(syscalls.parent_pid(), 0);
}

TEST_F(RealProcessSyscallsTest, ForkProducesARealChildThatTheParentReaps) {
  const auto forked = syscalls.fork_process();
  ASSERT_TRUE(forked.has_value()) << describe(forked.error());

  if (forked.value() == 0) {
    ::_exit(0);  // The child's own coverage is lost here; see the note above.
  }

  int raw = 0;
  ASSERT_EQ(::waitpid(forked.value(), &raw, 0), forked.value());
  EXPECT_TRUE(WIFEXITED(raw));
  EXPECT_EQ(WEXITSTATUS(raw), 0);
}

// --- the translation helper, whose failing arm no kernel will give us --------

TEST(SyscallResultTest, ASuccessfulCallYieldsItsValue) {
  const auto result = result_or_error<int>(7, false, ENOENT, "fork", "worker process");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 7);
}

TEST(SyscallResultTest, AFailedCallYieldsTheErrnoAndSubject) {
  // This is the arm fork(2) cannot be made to take in a test: its failures come
  // from RLIMIT_NPROC, which is not even enforced for the privileged process CI
  // runs as. Moving the decision here is what makes it reachable at all.
  const auto result = result_or_error<pid_t>(-1, true, EAGAIN, "fork", "worker process");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EAGAIN);
  EXPECT_EQ(result.error().call, "fork");
  EXPECT_EQ(result.error().subject, "worker process");
}

TEST_F(RealProcessSyscallsTest, SignallingAReapedPidIsESRCH) {
  // A real ESRCH from a real kill, rather than a fake agreeing that kill
  // returns ESRCH. This is the race WorkerPool::signal_all tolerates: a worker
  // that died between the last reap and the signal. Reaping first makes it
  // deterministic -- the pid is gone, not merely likely to be.
  const auto forked = syscalls.fork_process();
  ASSERT_TRUE(forked.has_value()) << describe(forked.error());
  if (forked.value() == 0) {
    ::_exit(0);
  }
  int raw = 0;
  ASSERT_EQ(::waitpid(forked.value(), &raw, 0), forked.value());

  const auto sent = syscalls.send_signal(forked.value(), SIGTERM);
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error().number, ESRCH);
  EXPECT_EQ(sent.error().call, "kill");
  EXPECT_NE(sent.error().subject.find("pid "), std::string::npos);
}

TEST_F(RealProcessSyscallsTest, KillSucceedsOnAnUnreapedZombieAndOnlyTheReapMakesItESRCH) {
  // The kernel fact behind EXT-01 (journal §1.27), pinned against the kernel
  // so that no fake can drift from it: a child that has terminated and not
  // been reaped is a zombie, kill(2) on it SUCCEEDS, and the signal changes
  // nothing about how the child died. ESRCH comes only after the reap.
  const auto forked = syscalls.fork_process();
  ASSERT_TRUE(forked.has_value()) << describe(forked.error());
  if (forked.value() == 0) {
    ::_exit(0);
  }
  const pid_t child = forked.value();
  // The barrier: the child has terminated, and WNOWAIT leaves it unreaped.
  siginfo_t info{};
  ASSERT_EQ(::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT), 0);
  ASSERT_EQ(info.si_pid, child);

  const auto sent = syscalls.send_signal(child, SIGKILL);
  EXPECT_TRUE(sent.has_value()) << (sent.has_value() ? "" : describe(sent.error()));

  int raw = 0;
  ASSERT_EQ(::waitpid(child, &raw, 0), child);
  EXPECT_EQ(ExitStatus{raw}.termination(), Termination::kExited)
      << "the SIGKILL did not change how it died";
  EXPECT_EQ(ExitStatus{raw}.code(), 0);

  const auto after = syscalls.send_signal(child, SIGKILL);
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error().number, ESRCH);
}

TEST_F(RealProcessSyscallsTest, HasExitedSeesARunningChildAZombieWithoutReapingItAndThenECHILD) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(::pipe(fds), 0);
  const auto forked = syscalls.fork_process();
  ASSERT_TRUE(forked.has_value()) << describe(forked.error());
  if (forked.value() == 0) {
    char byte = 0;
    (void)::read(fds[0], &byte, 1);  // Held until the parent says go.
    ::_exit(3);
  }
  const pid_t child = forked.value();
  const auto running = syscalls.has_exited(child);
  ASSERT_TRUE(running.has_value()) << describe(running.error());
  EXPECT_FALSE(running.value());

  ASSERT_EQ(::write(fds[1], "g", 1), 1);
  siginfo_t info{};
  ASSERT_EQ(::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT), 0);
  for (int peek = 0; peek < 2; ++peek) {
    const auto zombie = syscalls.has_exited(child);
    ASSERT_TRUE(zombie.has_value()) << describe(zombie.error());
    EXPECT_TRUE(zombie.value()) << "asking does not reap, so it can be asked again";
  }
  int raw = 0;
  ASSERT_EQ(::waitpid(child, &raw, 0), child) << "the peek left the child to be reaped";
  EXPECT_EQ(ExitStatus{raw}.code(), 3);
  ::close(fds[0]);
  ::close(fds[1]);

  const auto gone = syscalls.has_exited(child);
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error().number, ECHILD);
  EXPECT_EQ(gone.error().call, "waitid");
}

TEST(ExitPeekResultTest, ANegativeReturnIsTheFailureWithItsErrno) {
  const auto failed = exit_peek_result(-1, 0, ECHILD);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), (SyscallError{ECHILD, "waitid", "exit status, without reaping"}));
}

TEST(ExitPeekResultTest, ZeroPidInTheSiginfoIsAChildStillRunning) {
  const auto running = exit_peek_result(0, 0, 0);
  ASSERT_TRUE(running.has_value());
  EXPECT_FALSE(running.value());
}

TEST(ExitPeekResultTest, ThePidInTheSiginfoIsAChildThatHasTerminated) {
  const auto exited = exit_peek_result(0, 4321, 0);
  ASSERT_TRUE(exited.has_value());
  EXPECT_TRUE(exited.value());
}

TEST_F(RealSyscallsTest, SignalZeroProbesALiveProcessWithoutDisturbingIt) {
  // Signal 0 is the documented existence check. Asserted against this very
  // process, which is definitely alive, so the success arm of send_signal is
  // exercised against the kernel rather than only against the fake.
  const auto sent = syscalls.send_signal(::getpid(), 0);
  EXPECT_TRUE(sent.has_value()) << (sent.has_value() ? "" : describe(sent.error()));
}

TEST_F(RealSyscallsTest, WaitingWithNoChildrenIsECHILD) {
  // The ordinary end of a run, from the real kernel. This test process has no
  // children at this point -- every earlier test reaped its own.
  const auto reaped = syscalls.wait_any();
  ASSERT_FALSE(reaped.has_value());
  EXPECT_EQ(reaped.error().number, ECHILD);
  EXPECT_EQ(reaped.error().call, "waitpid");
}

TEST_F(RealProcessSyscallsTest, WaitAnyReapsARealChildAndReportsItsStatusWord) {
  const auto forked = syscalls.fork_process();
  ASSERT_TRUE(forked.has_value()) << describe(forked.error());
  if (forked.value() == 0) {
    ::_exit(9);
  }

  const auto reaped = syscalls.wait_any();
  ASSERT_TRUE(reaped.has_value()) << describe(reaped.error());
  EXPECT_EQ(reaped.value().pid, forked.value());
  EXPECT_EQ(ExitStatus{reaped.value().status}.code(), 9);
}

TEST_F(RealProcessSyscallsTest, ReapingWithoutWaitingAnswersNothingYetThenTheChildThenECHILD) {
  // Children, none terminated: nothing. One terminated: that one. None at
  // all: ECHILD, the failing arm, reachable here because nothing blocks.
  int fds[2] = {-1, -1};
  ASSERT_EQ(::pipe(fds), 0);
  const auto forked = syscalls.fork_process();
  ASSERT_TRUE(forked.has_value()) << describe(forked.error());
  if (forked.value() == 0) {
    char byte = 0;
    (void)::read(fds[0], &byte, 1);  // Held until the parent says go.
    ::_exit(5);
  }
  auto nothing_yet = syscalls.reap_any_nonblocking();
  ASSERT_TRUE(nothing_yet.has_value()) << describe(nothing_yet.error());
  EXPECT_FALSE(nothing_yet.value().has_value());

  ASSERT_EQ(::write(fds[1], "g", 1), 1);
  std::optional<Reaped> reaped;
  for (int attempt = 0; attempt < 500 && !reaped.has_value(); ++attempt) {
    auto answer = syscalls.reap_any_nonblocking();
    ASSERT_TRUE(answer.has_value()) << describe(answer.error());
    reaped = answer.value();
    if (!reaped.has_value()) {
      ::usleep(2000);
    }
  }
  ASSERT_TRUE(reaped.has_value()) << "the child never terminated";
  EXPECT_EQ(reaped->pid, forked.value());
  EXPECT_EQ(ExitStatus{reaped->status}.code(), 5);
  ::close(fds[0]);
  ::close(fds[1]);

  auto none = syscalls.reap_any_nonblocking();
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().number, ECHILD);
  EXPECT_EQ(none.error().call, "waitpid");
  EXPECT_EQ(none.error().subject, "any child, without waiting");
}

TEST(SyscallResultTest, TheFailingArmWorksForEveryTypeTheSeamInstantiates) {
  // A template is compiled once per type, so each instantiation carries its own
  // coverage. The fork instantiation is exercised above; these are the others
  // the seam uses, and without them their failing arms would be reported as
  // never executed even though the code is identical.
  const auto reaped = result_or_error<Reaped>(Reaped{}, true, ECHILD, "waitpid", "any child");
  ASSERT_FALSE(reaped.has_value());
  EXPECT_EQ(reaped.error().number, ECHILD);

  const auto signalled = result_or_error<core::Ok>(core::Ok{}, true, ESRCH, "kill", "pid 1234");
  ASSERT_FALSE(signalled.has_value());
  EXPECT_EQ(signalled.error().number, ESRCH);
  EXPECT_EQ(signalled.error().subject, "pid 1234");

  const auto affinity =
      result_or_error<CpuMask>(CpuMask{}, true, ESRCH, "sched_getaffinity", "pid 1234");
  ASSERT_FALSE(affinity.has_value());
  EXPECT_EQ(affinity.error().call, "sched_getaffinity");
}

TEST(SyscallResultTest, TheValueIsIgnoredWhenTheCallFailed) {
  // A failed syscall's return value is meaningless, and the helper must not
  // smuggle it through: -1 is not a pid.
  const auto result = result_or_error<pid_t>(-1, true, ENOMEM, "fork", "worker process");
  ASSERT_FALSE(result.has_value());
}

// --- what fork and a reap without waiting answered, decided above the seam ----
//
// The seam's two process calls are bare; these functions decide what their
// answers mean. Every arm is reached here without creating a process, which is
// how the mutation sweep -- which runs no test that forks -- sees them at all.

TEST(ForkResultTest, ANegativePidIsTheFailureWithItsErrno) {
  const auto result = fork_result(-1, EAGAIN);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), (SyscallError{EAGAIN, "fork", "worker process"}));
}

TEST(ForkResultTest, ZeroIsTheChildsAnswerAndIsASuccess) {
  // The boundary: 0 is not a failure but the side of the fork this process is
  // on. Reading it as one would send every child down the error path.
  const auto result = fork_result(0, EINTR);  // A stale errno means nothing here.
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value(), 0);
}

TEST(ForkResultTest, APositivePidIsTheParentsAnswerAndIsCarriedAsItIs) {
  const auto result = fork_result(4321, EINTR);
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value(), 4321);
}

TEST(ReapResultTest, ANegativePidIsTheFailureWithItsErrno) {
  const auto result = reap_result(-1, 0, ECHILD);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), (SyscallError{ECHILD, "waitpid", "any child, without waiting"}));
}

TEST(ReapResultTest, ZeroIsChildrenOfWhichNoneHasTerminated) {
  // The boundary: 0 is an answer -- nothing to reap yet -- and neither a
  // failure nor a child numbered 0.
  const auto result = reap_result(0, 0, EINTR);
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_FALSE(result.value().has_value());
}

TEST(ReapResultTest, APositivePidIsTheChildReapedWithItsStatusWord) {
  const auto result = reap_result(4321, 9 << 8, EINTR);
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().has_value());
  EXPECT_EQ(*result.value(), (Reaped{4321, 9 << 8}));
}

// --- affinity against the real kernel ----------------------------------------
//
// Every call here targets the CALLING process. sched_setaffinity(2) will move
// any process for a privileged caller -- verified on this container as root,
// where pinning pid 1 succeeded and had to be undone -- and CI runs as root.
// A test that touched another pid would be a test that could confine init.

/// Puts the process back where it started, however the test ends.
///
/// It reads and restores through glibc, never through the seam under test. It
/// once used the seam, and a conversion that kept only CPU 0 then restored the
/// process to CPU 0 alone, for the rest of the suite -- after which the test
/// comparing the seam with CPU_ISSET saw one CPU both ways and agreed.
class RestoreAffinity {
 public:
  RestoreAffinity() : saved_(::sched_getaffinity(0, sizeof raw_, &raw_) == 0) {
    for (std::size_t cpu = 0; saved_ && cpu < kAffinityMaskBits; ++cpu) {
      if (CPU_ISSET(cpu, &raw_)) {
        original_.cpus.set(cpu);
      }
    }
  }
  ~RestoreAffinity() {
    if (saved_) {
      (void)::sched_setaffinity(0, sizeof raw_, &raw_);
    }
  }
  RestoreAffinity(const RestoreAffinity&) = delete;
  RestoreAffinity& operator=(const RestoreAffinity&) = delete;
  RestoreAffinity(RestoreAffinity&&) = delete;
  RestoreAffinity& operator=(RestoreAffinity&&) = delete;

  [[nodiscard]] const CpuMask& original() const { return original_; }

 private:
  cpu_set_t raw_{};
  bool saved_;
  CpuMask original_;
};

TEST_F(RealSyscallsTest, GetAffinityReportsAtLeastOneCpuAndNoMoreThanAreOnline) {
  auto mask = syscalls.get_affinity(0);
  ASSERT_TRUE(mask.has_value()) << describe(mask.error());
  const auto count = static_cast<long>(mask.value().cpus.count());
  EXPECT_GE(count, 1) << "this code is running on one";
  EXPECT_LE(count, ::sysconf(_SC_NPROCESSORS_ONLN))
      << "a cpuset can permit fewer than are online, never more";
}

TEST_F(RealSyscallsTest, SetAffinityToOnePermittedCpuIsVisibleThroughGetAffinity) {
  const RestoreAffinity restore;
  ASSERT_TRUE(restore.original().cpus.any());
  // The lowest CPU we are currently permitted, so the choice is valid under any
  // cpuset CI happens to run in.
  std::size_t chosen = 0;
  while (!restore.original().cpus.test(chosen)) {
    ++chosen;
  }
  CpuMask one;
  one.cpus.set(chosen);

  auto placed = syscalls.set_affinity(0, one);
  ASSERT_TRUE(placed.has_value()) << describe(placed.error());
  auto now = syscalls.get_affinity(0);
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(now.value(), one) << "exactly the one CPU, no more";
}

TEST_F(RealSyscallsTest, TheMaskLayoutAgreesWithGlibcsOwnMacrosOnEveryBit) {
  // The seam converts cpu_set_t by reading its bytes directly, on a documented
  // layout it static_asserts. This is the oracle for that layout (F21): glibc's
  // CPU_ISSET, consulted here in the test on the same kernel answer, for all
  // 1024 positions -- set and unset alike. A byte-order or bit-order mistake in
  // the conversion would show as a CPU reported where the macro sees none.
  cpu_set_t raw;
  CPU_ZERO(&raw);
  ASSERT_EQ(::sched_getaffinity(0, sizeof raw, &raw), 0);
  auto through_seam = syscalls.get_affinity(0);
  ASSERT_TRUE(through_seam.has_value());
  for (std::size_t cpu = 0; cpu < kAffinityMaskBits; ++cpu) {
    EXPECT_EQ(through_seam.value().cpus.test(cpu), CPU_ISSET(cpu, &raw) != 0) << "cpu " << cpu;
  }

  // And the other direction: a mask built here, set through the seam, read back
  // raw. The chosen CPU is one we are currently permitted, so the set succeeds
  // under any cpuset CI runs in.
  const RestoreAffinity restore;
  std::size_t chosen = 0;
  while (!restore.original().cpus.test(chosen)) {
    ++chosen;
  }
  CpuMask one;
  one.cpus.set(chosen);
  ASSERT_TRUE(syscalls.set_affinity(0, one).has_value());
  CPU_ZERO(&raw);
  ASSERT_EQ(::sched_getaffinity(0, sizeof raw, &raw), 0);
  EXPECT_EQ(CPU_COUNT(&raw), 1);
  EXPECT_TRUE(CPU_ISSET(chosen, &raw));
}

TEST(MaskLayoutTest, EveryCpuBitConvertsAsGlibcsOwnMacrosPlaceIt) {
  // The conversions themselves, without the kernel: glibc's CPU_SET and
  // CPU_ISSET are the oracle for all 1024 positions, one CPU at a time. The
  // real-kernel test above sees only the CPUs this machine has, and a single
  // CPU numbered 0 cannot show a conversion that loses every other one.
  for (std::size_t cpu = 0; cpu < kAffinityMaskBits; ++cpu) {
    cpu_set_t raw;
    CPU_ZERO(&raw);
    CPU_SET(cpu, &raw);
    CpuMaskBytes bytes{};
    std::memcpy(bytes.data(), &raw, sizeof raw);
    CpuMask expected;
    expected.cpus.set(cpu);
    ASSERT_EQ(cpu_mask_from_bytes(bytes), expected) << "reading cpu " << cpu;

    const CpuMaskBytes written = cpu_mask_to_bytes(expected);
    CPU_ZERO(&raw);
    std::memcpy(&raw, written.data(), sizeof raw);
    ASSERT_TRUE(CPU_ISSET(cpu, &raw)) << "writing cpu " << cpu;
    ASSERT_EQ(CPU_COUNT(&raw), 1) << "writing cpu " << cpu;
  }
}

TEST(MaskLayoutTest, AFullCpuMaskIsEveryBitOfEveryByte) {
  CpuMask all;
  all.cpus.set();
  CpuMaskBytes full{};
  full.fill(0xFF);
  EXPECT_EQ(cpu_mask_to_bytes(all), full);
  EXPECT_EQ(cpu_mask_from_bytes(full), all);
}

TEST_F(RealSyscallsTest, SetAffinityWithAnEmptyMaskIsEINVAL) {
  const RestoreAffinity restore;
  auto placed = syscalls.set_affinity(0, CpuMask{});
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().number, EINVAL);
  EXPECT_EQ(placed.error().call, "sched_setaffinity");
  EXPECT_EQ(placed.error().subject, "pid 0");
}

TEST_F(RealSyscallsTest, SetAffinityNamingOnlyACpuThisMachineLacksIsEINVAL) {
  const RestoreAffinity restore;
  // The last representable id. A machine with 1024 CPUs would make this test
  // wrong, and would be worth hearing about.
  CpuMask absent;
  absent.cpus.set(kAffinityMaskBits - 1);
  auto placed = syscalls.set_affinity(0, absent);
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().number, EINVAL);
}

TEST_F(RealSyscallsTest, AffinityCallsOnAPidThatDoesNotExistAreESRCH) {
  // A pid past pid_max on any default kernel. Never a real process, so nothing
  // is moved -- and if it ever were real, ESRCH is not what would come back,
  // which is the assertion.
  constexpr pid_t kNoSuchPid = 4194305;
  auto got = syscalls.get_affinity(kNoSuchPid);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().number, ESRCH);
  EXPECT_EQ(got.error().subject, "pid 4194305");

  CpuMask any;
  any.cpus.set(0);
  auto placed = syscalls.set_affinity(kNoSuchPid, any);
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().number, ESRCH);
}

// --- memory against the real kernel ------------------------------------------
//
// Each answer the fake models is corroborated here (F21). The tests read and
// write only memory this process mapped, and change no kernel setting: the
// huge-page pool in particular is whatever the machine has, and the test
// asserts the answer that pool implies rather than reserving pages on a host
// it does not own.

namespace {

constexpr std::size_t kOneMiB = std::size_t{1} << 20;
constexpr std::size_t kHugePage = std::size_t{2} << 20;
constexpr int kHugeFlags = MAP_HUGETLB | (21 << MAP_HUGE_SHIFT);

/// Unmaps however the test ends, so a failing assertion does not leak.
class UnmapAtExit {
 public:
  UnmapAtExit(RealSyscalls& syscalls, void* address, std::size_t length)
      : syscalls_(&syscalls), address_(address), length_(length) {}
  ~UnmapAtExit() { (void)syscalls_->unmap(address_, length_); }
  UnmapAtExit(const UnmapAtExit&) = delete;
  UnmapAtExit& operator=(const UnmapAtExit&) = delete;
  UnmapAtExit(UnmapAtExit&&) = delete;
  UnmapAtExit& operator=(UnmapAtExit&&) = delete;

 private:
  RealSyscalls* syscalls_;
  void* address_;
  std::size_t length_;
};

NodeMask node(std::uint32_t id) {
  NodeMask mask;
  mask.nodes.set(id);
  return mask;
}

/// Whether the sanitizer runtime answers mlock(2) itself. ASan and TSan
/// intercept mlock, munlock and mlockall and return 0 without calling the
/// kernel -- verified: under ASan, an mlock of an unmapped range "succeeded"
/// and an unprivileged child met no RLIMIT_MEMLOCK at all. Their runtimes
/// reserve terabytes of shadow address space, and a real lock of it would be
/// ruinous, so the interception is deliberate on their part. The tests that
/// assert the KERNEL's answer to mlock therefore skip, visibly, under a
/// sanitizer; every other configuration runs them (journal §1.17).
constexpr bool kMlockIsIntercepted =
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    true;
#else
    false;
#endif
#else
    false;
#endif

/// This process's locked bytes as the kernel accounts for them: VmLck in
/// /proc/self/status. The oracle for whether an mlock did anything (F21).
long locked_kib() {
  std::ifstream in("/proc/self/status");
  std::string key;
  long value = 0;
  while (in >> key) {
    if (key == "VmLck:") {
      in >> value;
      return value;
    }
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return -1;
}

/// The 2 MiB pool's free count, or nothing when this kernel has no 2 MiB pool.
std::optional<long> free_2mib_huge_pages() {
  std::ifstream in("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages");
  long count = 0;
  if (!(in >> count)) {
    return std::nullopt;
  }
  return count;
}

}  // namespace

TEST_F(RealSyscallsTest, MapsWritesReadsBackAndUnmaps) {
  auto mapped = syscalls.map_anonymous(kOneMiB, 0);
  ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
  ASSERT_NE(mapped.value(), nullptr);
  auto* bytes = static_cast<unsigned char*>(mapped.value());
  EXPECT_EQ(bytes[0], 0) << "anonymous memory arrives zeroed";
  EXPECT_EQ(bytes[kOneMiB - 1], 0);
  std::memset(bytes, 0xa5, kOneMiB);
  EXPECT_EQ(bytes[kOneMiB - 1], 0xa5);
  auto unmapped = syscalls.unmap(mapped.value(), kOneMiB);
  EXPECT_TRUE(unmapped.has_value()) << describe(unmapped.error());
}

TEST_F(RealSyscallsTest, MapOfZeroBytesIsEINVAL) {
  auto mapped = syscalls.map_anonymous(0, 0);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().number, EINVAL);
  EXPECT_EQ(mapped.error().call, "mmap");
  EXPECT_EQ(mapped.error().subject, "0 bytes");
}

TEST_F(RealSyscallsTest, MapLargerThanTheAddressSpaceIsENOMEM) {
  // 2^50 bytes: past the 47-bit user address space on x86-64 and the 48-bit
  // one on arm64, and with overcommit in any mode the kernel refuses the
  // reservation rather than the pages.
  auto mapped = syscalls.map_anonymous(std::size_t{1} << 50, 0);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().number, ENOMEM);
}

TEST_F(RealSyscallsTest, UnmapOfAnUnalignedAddressIsEINVAL) {
  auto mapped = syscalls.map_anonymous(kOneMiB, 0);
  ASSERT_TRUE(mapped.has_value());
  const UnmapAtExit cleanup(syscalls, mapped.value(), kOneMiB);
  auto* const unaligned = static_cast<unsigned char*>(mapped.value()) + 1;
  auto unmapped = syscalls.unmap(unaligned, 4096);
  ASSERT_FALSE(unmapped.has_value());
  EXPECT_EQ(unmapped.error().number, EINVAL);
  EXPECT_EQ(unmapped.error().call, "munmap");
}

TEST_F(RealSyscallsTest, AdvisesTransparentHugePagesAndPopulatesARealMapping) {
  auto mapped = syscalls.map_anonymous(kHugePage, 0);
  ASSERT_TRUE(mapped.has_value());
  const UnmapAtExit cleanup(syscalls, mapped.value(), kHugePage);
  auto advised = syscalls.advise_memory(mapped.value(), kHugePage, MADV_HUGEPAGE);
  EXPECT_TRUE(advised.has_value()) << describe(advised.error());
  auto populated = syscalls.advise_memory(mapped.value(), kHugePage, MADV_POPULATE_WRITE);
  EXPECT_TRUE(populated.has_value()) << describe(populated.error());

  auto* const unaligned = static_cast<unsigned char*>(mapped.value()) + 1;
  auto bad = syscalls.advise_memory(unaligned, 4096, MADV_HUGEPAGE);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().number, EINVAL);
  EXPECT_EQ(bad.error().call, "madvise");
  EXPECT_EQ(bad.error().subject, "4096 bytes, advice " + std::to_string(MADV_HUGEPAGE));
}

TEST_F(RealSyscallsTest, CallsOnARangeThatIsNoLongerMappedAnswerAsTheFakeModels) {
  // The fake's answers for an unmapped range -- ENOMEM from madvise, EFAULT
  // from mbind -- are the kernel's, checked here on a range this process
  // mapped and released, so the addresses are ones nobody else will have
  // taken. mlock's ENOMEM has its own test below, because a sanitizer
  // answers mlock itself.
  auto mapped = syscalls.map_anonymous(kOneMiB, 0);
  ASSERT_TRUE(mapped.has_value());
  void* const address = mapped.value();
  ASSERT_TRUE(syscalls.unmap(address, kOneMiB).has_value());

  auto advised = syscalls.advise_memory(address, kOneMiB, MADV_POPULATE_WRITE);
  ASSERT_FALSE(advised.has_value());
  EXPECT_EQ(advised.error().number, ENOMEM);

  auto bound = syscalls.bind_memory(address, kOneMiB, node(0));
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error().number, EFAULT);
  EXPECT_EQ(bound.error().call, "mbind");
}

TEST_F(RealSyscallsTest, LocksARealMapping) {
  // Succeeds for a privileged process regardless of RLIMIT_MEMLOCK, and for
  // an unprivileged one under the 8 MiB default. The limit's two refusals are
  // provoked from a child that drops privileges, below.
  auto mapped = syscalls.map_anonymous(kOneMiB, 0);
  ASSERT_TRUE(mapped.has_value());
  const UnmapAtExit cleanup(syscalls, mapped.value(), kOneMiB);
  const long before = locked_kib();
  auto locked = syscalls.lock_memory(mapped.value(), kOneMiB);
  ASSERT_TRUE(locked.has_value()) << describe(locked.error());
  if (kMlockIsIntercepted) {
    GTEST_SKIP() << "the sanitizer runtime answered mlock; the kernel was not asked";
  }
  EXPECT_GE(locked_kib() - before, static_cast<long>(kOneMiB / 1024))
      << "the kernel's own accounting shows the lock";
}

TEST_F(RealSyscallsTest, MlockOnARangeThatIsNoLongerMappedIsENOMEM) {
  if (kMlockIsIntercepted) {
    GTEST_SKIP() << "the sanitizer runtime answers mlock without asking the kernel";
  }
  auto mapped = syscalls.map_anonymous(kOneMiB, 0);
  ASSERT_TRUE(mapped.has_value());
  void* const address = mapped.value();
  ASSERT_TRUE(syscalls.unmap(address, kOneMiB).has_value());
  auto locked = syscalls.lock_memory(address, kOneMiB);
  ASSERT_FALSE(locked.has_value());
  EXPECT_EQ(locked.error().number, ENOMEM);
  EXPECT_EQ(locked.error().call, "mlock");
}

TEST_F(RealSyscallsTest, BindsToNodeZeroAndIsRefusedForANodeThisMachineLacks) {
  auto mapped = syscalls.map_anonymous(kOneMiB, 0);
  ASSERT_TRUE(mapped.has_value());
  const UnmapAtExit cleanup(syscalls, mapped.value(), kOneMiB);

  auto bound = syscalls.bind_memory(mapped.value(), kOneMiB, node(0));
  EXPECT_TRUE(bound.has_value()) << describe(bound.error())
                                 << " (node 0 has memory on every Linux machine)";

  // The last representable node. A machine with 1024 nodes would make this
  // wrong, and would be worth hearing about.
  auto absent = syscalls.bind_memory(mapped.value(), kOneMiB, node(kNodeMaskBits - 1));
  ASSERT_FALSE(absent.has_value());
  EXPECT_EQ(absent.error().number, EINVAL);

  auto empty = syscalls.bind_memory(mapped.value(), kOneMiB, NodeMask{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().number, EINVAL) << "an empty mask is no policy at all";

  auto* const unaligned = static_cast<unsigned char*>(mapped.value()) + 1;
  auto crooked = syscalls.bind_memory(unaligned, 4096, node(0));
  ASSERT_FALSE(crooked.has_value());
  EXPECT_EQ(crooked.error().number, EINVAL);
}

TEST(MaskLayoutTest, EveryNodeBitLandsWhereTheKernelsBitmapPutsIt) {
  // mbind reads the kernel's bitmap type -- an array of unsigned long, bit n
  // in word n / bits-per-long -- and a cpu_set_t is the same type, which
  // glibc's CPU_SET writes. So CPU_SET is the oracle for the node words too,
  // on all 1024 positions; a machine with one node could not show a layout
  // that sends every node to node 0.
  static_assert(sizeof(cpu_set_t) == sizeof(NodeMaskWords));
  for (std::size_t node = 0; node < kNodeMaskBits; ++node) {
    cpu_set_t raw;
    CPU_ZERO(&raw);
    CPU_SET(node, &raw);
    NodeMaskWords expected{};
    std::memcpy(expected.data(), &raw, sizeof raw);
    NodeMask one;
    one.nodes.set(node);
    ASSERT_EQ(node_mask_to_words(one), expected) << "node " << node;
  }
}

TEST_F(RealSyscallsTest, ExplicitHugePagesAnswerAccordingToThePoolThisMachineHas) {
  // Three states, one assertion each, no skip: the pool is whatever it is,
  // and the kernel's answer for it is what the fake claims for that state.
  const std::optional<long> free_pages = free_2mib_huge_pages();
  auto mapped = syscalls.map_anonymous(1, kHugeFlags);

  if (!free_pages.has_value()) {
    ASSERT_FALSE(mapped.has_value()) << "no 2 MiB pool exists, so no 2 MiB page can";
    EXPECT_EQ(mapped.error().number, EINVAL);
    return;
  }
  if (free_pages.value() == 0) {
    ASSERT_FALSE(mapped.has_value()) << "the pool exists and is empty";
    EXPECT_EQ(mapped.error().number, ENOMEM);
    return;
  }
  ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
  // The length trap the module is built around: a one-byte request took a
  // whole page, and giving it back needs the rounded length.
  auto unrounded = syscalls.unmap(mapped.value(), 1);
  ASSERT_FALSE(unrounded.has_value());
  EXPECT_EQ(unrounded.error().number, EINVAL);
  auto rounded = syscalls.unmap(mapped.value(), kHugePage);
  EXPECT_TRUE(rounded.has_value()) << describe(rounded.error());
}

TEST_F(RealProcessSyscallsTest, AnUnprivilegedProcessMeetsRlimitMemlockBothWaysTheKernelSaysNo) {
  // mlock ignores RLIMIT_MEMLOCK for a privileged process, and CI is one. So
  // a child lowers the limit, drops to nobody, and reports the errno of a
  // lock past the limit and of a lock under a limit of zero. If it cannot
  // drop privileges the test is skipped rather than passed: an assertion
  // about an unprivileged process made by a privileged one would be empty.
  if (kMlockIsIntercepted) {
    GTEST_SKIP() << "the sanitizer runtime answers mlock without asking the kernel";
  }
  constexpr int kCouldNotDrop = -1;
  std::array<int, 2> pipe_fds{};
  ASSERT_EQ(::pipe(pipe_fds.data()), 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::close(pipe_fds[0]);
    std::array<int, 2> answer{kCouldNotDrop, kCouldNotDrop};
    const rlimit tiny{4096, 4096};
    constexpr uid_t kNobody = 65534;
    if (::setrlimit(RLIMIT_MEMLOCK, &tiny) == 0 && ::setgroups(0, nullptr) == 0 &&
        ::setresgid(kNobody, kNobody, kNobody) == 0 &&
        ::setresuid(kNobody, kNobody, kNobody) == 0) {
      RealSyscalls unprivileged;
      auto mapped = unprivileged.map_anonymous(kOneMiB, 0);
      if (mapped.has_value()) {
        auto past = unprivileged.lock_memory(mapped.value(), kOneMiB);
        answer[0] = past.has_value() ? 0 : past.error().number;
        const rlimit none{0, 0};
        if (::setrlimit(RLIMIT_MEMLOCK, &none) == 0) {
          auto under_zero = unprivileged.lock_memory(mapped.value(), 4096);
          answer[1] = under_zero.has_value() ? 0 : under_zero.error().number;
        }
      }
    }
    const ssize_t wrote = ::write(pipe_fds[1], answer.data(), sizeof answer);
    ::_exit(wrote == static_cast<ssize_t>(sizeof answer) ? 0 : 1);
  }
  ::close(pipe_fds[1]);
  std::array<int, 2> answer{};
  const ssize_t got = ::read(pipe_fds[0], answer.data(), sizeof answer);
  ::close(pipe_fds[0]);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "the child's write failed";
  ASSERT_EQ(got, static_cast<ssize_t>(sizeof answer));
  if (answer[0] == kCouldNotDrop) {
    GTEST_SKIP() << "needs to run as root to drop privileges";
  }
  EXPECT_EQ(answer[0], ENOMEM) << "past the limit";
  EXPECT_EQ(answer[1], EPERM) << "under a limit of zero, without CAP_IPC_LOCK";
}

// --- memory files against the real kernel ----------------------------------------

TEST_F(RealProcessSyscallsTest, AMemoryFileIsCloseOnExecUntilHandedOverAndMapsShared) {
  auto fd = syscalls.create_memory_file("loadforge-test", 4096);
  ASSERT_TRUE(fd.has_value()) << describe(fd.error());
  ASSERT_GE(fd.value(), 0);
  // glibc's own view of the flag is the oracle (F21), before and after.
  EXPECT_NE(::fcntl(fd.value(), F_GETFD) & FD_CLOEXEC, 0) << "close-on-exec by default";
  auto handed = syscalls.inherit_across_exec(fd.value());
  ASSERT_TRUE(handed.has_value()) << describe(handed.error());
  EXPECT_EQ(::fcntl(fd.value(), F_GETFD) & FD_CLOEXEC, 0) << "cleared on request";

  auto mapped = syscalls.map_shared(fd.value(), 4096);
  ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
  auto* const bytes = static_cast<unsigned char*>(mapped.value());
  EXPECT_EQ(bytes[0], 0) << "a fresh memfd is zero-filled";
  bytes[0] = 0x5a;

  // A child maps the same file anew and must see the byte, then change it.
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    RealSyscalls own;
    auto view = own.map_shared(fd.value(), 4096);
    if (!view.has_value()) {
      ::_exit(2);
    }
    auto* const seen = static_cast<unsigned char*>(view.value());
    if (seen[0] != 0x5a) {
      ::_exit(3);
    }
    seen[0] = 0xa5;
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "2: map failed, 3: did not see";
  EXPECT_EQ(bytes[0], 0xa5) << "the child's write came back through the file";

  EXPECT_TRUE(syscalls.unmap(mapped.value(), 4096).has_value());
  EXPECT_TRUE(syscalls.close(fd.value()).has_value());
}

/// Lowers one soft resource limit for the scope and restores it however the
/// test ends. Soft only: the hard limit is never touched, so raising the soft
/// limit back is always permitted.
class SoftLimit {
 public:
  SoftLimit(int resource, rlim_t soft) : resource_(resource) {
    ::getrlimit(resource_, &original_);
    const rlimit lowered{soft, original_.rlim_max};
    applied_ = ::setrlimit(resource_, &lowered) == 0;
  }
  ~SoftLimit() { ::setrlimit(resource_, &original_); }
  SoftLimit(const SoftLimit&) = delete;
  SoftLimit& operator=(const SoftLimit&) = delete;
  SoftLimit(SoftLimit&&) = delete;
  SoftLimit& operator=(SoftLimit&&) = delete;
  [[nodiscard]] bool applied() const { return applied_; }

 private:
  int resource_;
  rlimit original_{};
  bool applied_ = false;
};

TEST_F(RealSyscallsTest, SizingAMemoryFilePastRlimitFsizeIsEFBIG) {
  // In this process, not a child: a forked child that _exits never flushes
  // its coverage counters (journal §1.9), so the failure arm would run and
  // still read as never executed. The limit is lowered for the scope only,
  // and SIGXFSZ is ignored meanwhile, or the kernel would kill the process
  // instead of answering EFBIG.
  const auto previous = ::signal(SIGXFSZ, SIG_IGN);
  {
    const SoftLimit tiny_file(RLIMIT_FSIZE, 4096);
    ASSERT_TRUE(tiny_file.applied());
    auto sized = syscalls.create_memory_file("loadforge-test", 8192);
    ASSERT_FALSE(sized.has_value());
    EXPECT_EQ(sized.error().number, EFBIG);
    EXPECT_EQ(sized.error().call, "ftruncate");
    EXPECT_EQ(sized.error().subject, "loadforge-test, 8192 bytes");
  }
  ::signal(SIGXFSZ, previous);
  auto sized = syscalls.create_memory_file("loadforge-test", 8192);
  ASSERT_TRUE(sized.has_value()) << "the limit was restored";
  EXPECT_TRUE(syscalls.close(sized.value()).has_value());
}

TEST_F(RealSyscallsTest, CreatingAMemoryFileWithNoDescriptorNumberFreeIsEMFILE) {
  // The lowest free descriptor number is found by taking one and giving it
  // back; a soft RLIMIT_NOFILE equal to it leaves no number a new descriptor
  // could take.
  auto probe = syscalls.create_memory_file("loadforge-test", 4096);
  ASSERT_TRUE(probe.has_value());
  const int lowest_free = probe.value();
  ASSERT_TRUE(syscalls.close(lowest_free).has_value());
  {
    const SoftLimit no_room(RLIMIT_NOFILE, static_cast<rlim_t>(lowest_free));
    ASSERT_TRUE(no_room.applied());
    auto created = syscalls.create_memory_file("loadforge-test", 4096);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().number, EMFILE);
    EXPECT_EQ(created.error().call, "memfd_create");
    EXPECT_EQ(created.error().subject, "loadforge-test");
  }
  auto created = syscalls.create_memory_file("loadforge-test", 4096);
  ASSERT_TRUE(created.has_value()) << "the limit was restored";
  EXPECT_TRUE(syscalls.close(created.value()).has_value());
}

TEST_F(RealSyscallsTest, DescriptorZeroIsASuccessfulMemoryFile) {
  const StandardInputSetAside free_zero;
  auto created = syscalls.create_memory_file("loadforge-test", 4096);
  ASSERT_TRUE(created.has_value()) << describe(created.error());
  EXPECT_EQ(created.value(), 0);
  EXPECT_TRUE(syscalls.close(created.value()).has_value());
}

TEST_F(RealSyscallsTest, MappingOrHandingOverADescriptorThatIsNotOpenIsEBADF) {
  auto mapped = syscalls.map_shared(-1, 4096);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().number, EBADF);
  EXPECT_EQ(mapped.error().call, "mmap");
  EXPECT_EQ(mapped.error().subject, "fd -1, 4096 bytes");
  auto handed = syscalls.inherit_across_exec(-1);
  ASSERT_FALSE(handed.has_value());
  EXPECT_EQ(handed.error().number, EBADF);
  EXPECT_EQ(handed.error().call, "fcntl");
}

TEST(SeamValuesTest, AReapAndACpuMaskCompareFieldByField) {
  const Reaped reaped{4321, 9 << 8};
  EXPECT_EQ(reaped, (Reaped{4321, 9 << 8}));
  EXPECT_NE(reaped, (Reaped{4322, 9 << 8})) << "another child";
  EXPECT_NE(reaped, (Reaped{4321, 8 << 8})) << "the same child, another status";

  CpuMask some;
  some.cpus.set(1);
  CpuMask other;
  other.cpus.set(2);
  EXPECT_EQ(some, some);
  EXPECT_NE(some, other);
  EXPECT_NE(some, CpuMask{});
}

TEST(SeamValuesTest, PipeEndsAndReadinessCompareFieldByField) {
  const PipeEnds ends{3, 4};
  EXPECT_EQ(ends, (PipeEnds{3, 4}));
  EXPECT_NE(ends, (PipeEnds{5, 4}));
  EXPECT_NE(ends, (PipeEnds{3, 5}));

  const Readiness none;
  EXPECT_EQ(none, Readiness{});
  EXPECT_NE(none, (Readiness{true, false, false, false}));
  EXPECT_NE(none, (Readiness{false, true, false, false}));
  EXPECT_NE(none, (Readiness{false, false, true, false}));
  EXPECT_NE(none, (Readiness{false, false, false, true}));
}

TEST_F(RealSyscallsTest, APipeIsCloseOnExecAtBothEndsAndPollSeesBytesThenTheHangUp) {
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value()) << describe(created.error());
  const PipeEnds pipe = created.value();
  ASSERT_GE(pipe.read_fd, 0);
  ASSERT_GE(pipe.write_fd, 0);
  EXPECT_NE(pipe.read_fd, pipe.write_fd);
  EXPECT_NE(::fcntl(pipe.read_fd, F_GETFD) & FD_CLOEXEC, 0);
  EXPECT_NE(::fcntl(pipe.write_fd, F_GETFD) & FD_CLOEXEC, 0);

  // Empty and open: nothing to report, and poll returns when the timeout
  // expires rather than failing.
  auto idle = syscalls.poll_readable({pipe.read_fd, pipe.write_fd}, 0);
  ASSERT_TRUE(idle.has_value()) << describe(idle.error());
  ASSERT_EQ(idle.value().size(), 2U);
  EXPECT_EQ(idle.value()[0], Readiness{});
  EXPECT_EQ(idle.value()[1], Readiness{}) << "POLLIN was asked for; a writable end is silent";

  const std::array<std::byte, 3> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  auto count = syscalls.write(pipe.write_fd, bytes);
  ASSERT_TRUE(count.has_value()) << describe(count.error());
  EXPECT_EQ(count.value(), 3U);
  auto readable = syscalls.poll_readable({pipe.read_fd}, 0);
  ASSERT_TRUE(readable.has_value());
  EXPECT_EQ(readable.value()[0], (Readiness{true, false, false, false}));

  ASSERT_TRUE(syscalls.close(pipe.write_fd).has_value());
  // Bytes still queued AND the writer gone: both at once, as the kernel says.
  auto both = syscalls.poll_readable({pipe.read_fd}, 0);
  ASSERT_TRUE(both.has_value());
  EXPECT_EQ(both.value()[0], (Readiness{true, true, false, false}));

  std::string buffer(16, '\0');
  auto got = syscalls.read(pipe.read_fd, buffer.data(), buffer.size());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(buffer.substr(0, got.value()), "abc");
  auto eof = syscalls.read(pipe.read_fd, buffer.data(), buffer.size());
  ASSERT_TRUE(eof.has_value());
  EXPECT_EQ(eof.value(), 0U) << "end of file once the writer is gone and the bytes are drained";
  auto hung = syscalls.poll_readable({pipe.read_fd}, 0);
  ASSERT_TRUE(hung.has_value());
  EXPECT_EQ(hung.value()[0], (Readiness{false, true, false, false}));
  EXPECT_TRUE(syscalls.close(pipe.read_fd).has_value());
}

TEST_F(RealSyscallsTest, WritingToAPipeNobodyReadsIsEPIPEAndItsWriteEndPollsAsAnError) {
  // SIGPIPE would kill the process before write could answer; the worker
  // ignores it for the same reason, and the test does so for its scope.
  const auto previous = ::signal(SIGPIPE, SIG_IGN);
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value());
  const PipeEnds pipe = created.value();
  ASSERT_TRUE(syscalls.close(pipe.read_fd).has_value());
  auto polled = syscalls.poll_readable({pipe.write_fd}, 0);
  ASSERT_TRUE(polled.has_value());
  EXPECT_EQ(polled.value()[0], (Readiness{false, false, true, false}));
  const std::array<std::byte, 1> byte{std::byte{'x'}};
  auto count = syscalls.write(pipe.write_fd, byte);
  ASSERT_FALSE(count.has_value());
  EXPECT_EQ(count.error().number, EPIPE);
  EXPECT_EQ(count.error().call, "write");
  EXPECT_EQ(count.error().subject, "fd " + std::to_string(pipe.write_fd));
  EXPECT_TRUE(syscalls.close(pipe.write_fd).has_value());
  ::signal(SIGPIPE, previous);
}

TEST_F(RealSyscallsTest, WritingADescriptorThatIsNotOpenIsEBADFAndPollingItIsInvalid) {
  const std::array<std::byte, 1> byte{std::byte{'x'}};
  auto count = syscalls.write(-1, byte);
  ASSERT_FALSE(count.has_value());
  EXPECT_EQ(count.error().number, EBADF);
  EXPECT_EQ(count.error().subject, "fd -1");
  // poll does not fail for a closed descriptor; it marks that one POLLNVAL
  // and answers for the rest, which is why Readiness carries the flag. A
  // NEGATIVE number is different again: poll ignores the entry and reports
  // nothing for it (verified: -1 came back all clear), so the closed one here
  // is a real number that was open a moment ago.
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value());
  const PipeEnds pipe = created.value();
  ASSERT_TRUE(syscalls.close(pipe.read_fd).has_value());
  auto polled = syscalls.poll_readable({pipe.read_fd, pipe.write_fd, -1}, 0);
  ASSERT_TRUE(polled.has_value()) << describe(polled.error());
  ASSERT_EQ(polled.value().size(), 3U);
  EXPECT_EQ(polled.value()[0], (Readiness{false, false, false, true})) << "closed: POLLNVAL";
  EXPECT_EQ(polled.value()[1], (Readiness{false, false, true, false})) << "no reader: POLLERR";
  EXPECT_EQ(polled.value()[2], Readiness{}) << "negative: ignored";
  EXPECT_TRUE(syscalls.close(pipe.write_fd).has_value());
}

TEST_F(RealSyscallsTest, WritingNothingIsAWriteOfZeroBytesAndNotAFailure) {
  // POSIX: zero bytes to a regular file returns 0 and has no other effect. A
  // count of 0 is an answer, as it is from read at end of file.
  const std::string path = write("empty", "");
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  ASSERT_GE(fd, 0);
  auto count = syscalls.write(fd, std::span<const std::byte>{});
  ASSERT_TRUE(count.has_value()) << describe(count.error());
  EXPECT_EQ(count.value(), 0U);
  EXPECT_TRUE(syscalls.close(fd).has_value());
  EXPECT_EQ(std::filesystem::file_size(path), 0U);
}

TEST_F(RealSyscallsTest, PollingMoreDescriptorsThanTheOpenFileLimitAllowsIsEINVAL) {
  // The one failure of poll(2) a process can provoke on itself: nfds past
  // RLIMIT_NOFILE. The limit is lowered for the scope only; the descriptors
  // already open are unaffected by it.
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value());
  const PipeEnds pipe = created.value();
  {
    const SoftLimit one(RLIMIT_NOFILE, 1);
    ASSERT_TRUE(one.applied());
    auto polled = syscalls.poll_readable({pipe.read_fd, pipe.write_fd}, 0);
    ASSERT_FALSE(polled.has_value());
    EXPECT_EQ(polled.error().number, EINVAL);
    EXPECT_EQ(polled.error().call, "poll");
    EXPECT_EQ(polled.error().subject, "2 descriptors");
  }
  auto polled = syscalls.poll_readable({pipe.read_fd, pipe.write_fd}, 0);
  EXPECT_TRUE(polled.has_value()) << "the limit was restored";
  EXPECT_TRUE(syscalls.close(pipe.read_fd).has_value());
  EXPECT_TRUE(syscalls.close(pipe.write_fd).has_value());
}

TEST_F(RealSyscallsTest, CreatingAPipeWithNoDescriptorNumberFreeIsEMFILE) {
  auto probe = syscalls.create_memory_file("loadforge-test", 4096);
  ASSERT_TRUE(probe.has_value());
  const int lowest_free = probe.value();
  ASSERT_TRUE(syscalls.close(lowest_free).has_value());
  {
    const SoftLimit no_room(RLIMIT_NOFILE, static_cast<rlim_t>(lowest_free));
    ASSERT_TRUE(no_room.applied());
    auto created = syscalls.create_pipe();
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().number, EMFILE);
    EXPECT_EQ(created.error().call, "pipe2");
    EXPECT_EQ(created.error().subject, "record channel");
  }
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value()) << "the limit was restored";
  EXPECT_TRUE(syscalls.close(created.value().read_fd).has_value());
  EXPECT_TRUE(syscalls.close(created.value().write_fd).has_value());
}

TEST_F(RealSyscallsTest, IgnoresASignalAndIsRefusedForTheOneThatCannotBeIgnored) {
  // glibc's sigaction is the oracle for the disposition, before and after.
  struct sigaction before {};
  ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &before), 0);
  auto ignored = syscalls.ignore_signal(SIGPIPE);
  ASSERT_TRUE(ignored.has_value()) << describe(ignored.error());
  struct sigaction after {};
  ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &after), 0);
  EXPECT_EQ(after.sa_handler, SIG_IGN);  // NOLINT(cppcoreguidelines-pro-type-union-access)
  ASSERT_EQ(::sigaction(SIGPIPE, &before, nullptr), 0) << "restored for the other tests";

  auto refused = syscalls.ignore_signal(SIGKILL);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().number, EINVAL);
  EXPECT_EQ(refused.error().call, "sigaction");
  EXPECT_EQ(refused.error().subject, "signal 9");
}

TEST_F(RealSyscallsTest, PutsASignalBackToItsDefaultAndIsRefusedForTheOneThatCannotBeChanged) {
  // Review C2: an ignored SIGCHLD is what a launcher can hand a program.
  struct sigaction before {};
  ASSERT_EQ(::sigaction(SIGCHLD, nullptr, &before), 0);
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;  // NOLINT(cppcoreguidelines-pro-type-union-access)
  ::sigemptyset(&ignore.sa_mask);
  ASSERT_EQ(::sigaction(SIGCHLD, &ignore, nullptr), 0);

  auto defaulted = syscalls.default_signal(SIGCHLD);
  ASSERT_TRUE(defaulted.has_value()) << describe(defaulted.error());
  struct sigaction after {};
  ASSERT_EQ(::sigaction(SIGCHLD, nullptr, &after), 0);
  EXPECT_EQ(after.sa_handler, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-union-access)
  ASSERT_EQ(::sigaction(SIGCHLD, &before, nullptr), 0) << "restored for the other tests";

  auto refused = syscalls.default_signal(SIGKILL);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().number, EINVAL);
  EXPECT_EQ(refused.error().call, "sigaction");
  EXPECT_EQ(refused.error().subject, "signal 9");
}

// The handler of a diverted signal, driven by hand: the handler is one function, and a
// test that raised the signal would test the kernel with it.
class DivertedSignalTest : public RealSyscallsTest {
 protected:
  void SetUp() override {
    RealSyscallsTest::SetUp();
    auto created = syscalls.create_pipe();
    ASSERT_TRUE(created.has_value());
    pipe = created.value();
  }
  void TearDown() override {
    (void)syscalls.default_signal(SIGUSR1);
    (void)syscalls.default_signal(SIGUSR2);
    (void)syscalls.close(pipe.read_fd);
    (void)syscalls.close(pipe.write_fd);
    RealSyscallsTest::TearDown();
  }
  [[nodiscard]] std::string pending() const {
    std::array<char, 16> buffer{};
    const ssize_t count = ::read(pipe.read_fd, buffer.data(), buffer.size());
    return count > 0 ? std::string(buffer.data(), static_cast<std::size_t>(count)) : std::string{};
  }
  PipeEnds pipe;
};

TEST_F(DivertedSignalTest, ADivertedSignalWritesItsNumberToThePipeAndNothingElseHappens) {
  auto diverted = syscalls.divert_signal(SIGUSR1, pipe.write_fd);
  ASSERT_TRUE(diverted.has_value()) << describe(diverted.error());
  struct sigaction installed {};
  ASSERT_EQ(::sigaction(SIGUSR1, nullptr, &installed), 0);
  EXPECT_NE(installed.sa_flags & SA_RESTART, 0)
      << "no read or write of the controller is cut short";
  EXPECT_EQ(::sigismember(&installed.sa_mask, SIGTERM), 1)
      << "a second signal waits for the first one's byte to be written";
  EXPECT_EQ(::sigismember(&installed.sa_mask, SIGINT), 1);

  ASSERT_EQ(::raise(SIGUSR1), 0);  // The real kernel and the real handler: one byte.
  EXPECT_EQ(pending(), std::string(1, static_cast<char>(SIGUSR1)));
}

TEST_F(DivertedSignalTest, TheWriteEndIsMadeNonBlockingSoAFloodOfSignalsIsDroppedNotWaitedOn) {
  ASSERT_TRUE(syscalls.divert_signal(SIGUSR1, pipe.write_fd).has_value());
  const int flags = ::fcntl(pipe.write_fd, F_GETFL);
  ASSERT_GE(flags, 0);
  EXPECT_NE(flags & O_NONBLOCK, 0);
  // Far more signals than a pipe holds: the handler returns every time.
  constexpr int kMoreThanThePipeHolds = 70'000;
  for (int sent = 0; sent < kMoreThanThePipeHolds; ++sent) {
    note_diverted_signal(SIGUSR1);
  }
  SUCCEED() << "the handler never blocked";
}

TEST_F(DivertedSignalTest, ASignalNothingWasDivertedForWritesNothing) {
  ASSERT_TRUE(syscalls.divert_signal(SIGUSR1, pipe.write_fd).has_value());
  note_diverted_signal(SIGUSR2);
  note_diverted_signal(0);
  note_diverted_signal(kMaxDivertedSignal + 1);
  note_diverted_signal(-5);
  const int flags = ::fcntl(pipe.read_fd, F_GETFL);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(::fcntl(pipe.read_fd, F_SETFL, flags | O_NONBLOCK), 0);
  EXPECT_EQ(pending(), "");
}

TEST_F(DivertedSignalTest, TheHandlerLeavesErrnoAsItFoundIt) {
  ASSERT_TRUE(syscalls.divert_signal(SIGUSR1, pipe.write_fd).has_value());
  errno = ERANGE;
  note_diverted_signal(SIGUSR1);
  EXPECT_EQ(errno, ERANGE);
}

TEST_F(DivertedSignalTest, PuttingTheSignalBackToItsDefaultEndsTheDiversion) {
  ASSERT_TRUE(syscalls.divert_signal(SIGUSR1, pipe.write_fd).has_value());
  ASSERT_TRUE(syscalls.default_signal(SIGUSR1).has_value());
  note_diverted_signal(SIGUSR1);  // As a handler that ran after the pipe was closed would.
  const int flags = ::fcntl(pipe.read_fd, F_GETFL);
  ASSERT_EQ(::fcntl(pipe.read_fd, F_SETFL, flags | O_NONBLOCK), 0);
  EXPECT_EQ(pending(), "");
  struct sigaction after {};
  ASSERT_EQ(::sigaction(SIGUSR1, nullptr, &after), 0);
  EXPECT_EQ(after.sa_handler, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-union-access)
}

TEST_F(DivertedSignalTest, TwoSignalsEachWriteToTheirOwnDescriptor) {
  auto second = syscalls.create_pipe();
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(syscalls.divert_signal(SIGUSR1, pipe.write_fd).has_value());
  ASSERT_TRUE(syscalls.divert_signal(SIGUSR2, second.value().write_fd).has_value());
  note_diverted_signal(SIGUSR2);
  note_diverted_signal(SIGUSR1);
  EXPECT_EQ(pending(), std::string(1, static_cast<char>(SIGUSR1)));
  std::array<char, 4> buffer{};
  EXPECT_EQ(::read(second.value().read_fd, buffer.data(), buffer.size()), 1);
  EXPECT_EQ(buffer[0], static_cast<char>(SIGUSR2));
  EXPECT_TRUE(syscalls.close(second.value().read_fd).has_value());
  EXPECT_TRUE(syscalls.close(second.value().write_fd).has_value());
}

TEST_F(DivertedSignalTest, ADescriptorThatIsNotOpenIsEBADFAndNothingIsInstalled) {
  const int closed = pipe.write_fd;
  ASSERT_TRUE(syscalls.close(closed).has_value());
  pipe.write_fd = -1;
  auto refused = syscalls.divert_signal(SIGUSR1, closed);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().number, EBADF);
  EXPECT_EQ(refused.error().call, "fcntl");
  struct sigaction after {};
  ASSERT_EQ(::sigaction(SIGUSR1, nullptr, &after), 0);
  EXPECT_EQ(after.sa_handler, SIG_DFL);  // NOLINT(cppcoreguidelines-pro-type-union-access)
}

TEST_F(DivertedSignalTest, ASignalThatCannotBeCaughtIsEINVALAndTheDescriptorIsForgotten) {
  for (const int uncatchable : {SIGKILL, SIGSTOP, 0, kMaxDivertedSignal + 1, -1}) {
    auto refused = syscalls.divert_signal(uncatchable, pipe.write_fd);
    ASSERT_FALSE(refused.has_value()) << uncatchable;
    EXPECT_EQ(refused.error().number, EINVAL) << uncatchable;
    EXPECT_EQ(refused.error().call, "sigaction");
    EXPECT_EQ(refused.error().subject, "signal " + std::to_string(uncatchable));
  }
  note_diverted_signal(SIGKILL);
  note_diverted_signal(0);
  const int flags = ::fcntl(pipe.read_fd, F_GETFL);
  ASSERT_EQ(::fcntl(pipe.read_fd, F_SETFL, flags | O_NONBLOCK), 0);
  EXPECT_EQ(pending(), "") << "a refused diversion leaves nothing for the handler to write to";
}

TEST(DivertsHere, ADescriptorAndTheProcessThatGaveItDecide) {
  EXPECT_TRUE(diverts_here(5, 100, 100));
  EXPECT_TRUE(diverts_here(0, 100, 100));
  EXPECT_FALSE(diverts_here(-1, 100, 100)) << "nothing was given";
  EXPECT_FALSE(diverts_here(5, 100, 101)) << "a child that inherited the handler";
  EXPECT_FALSE(diverts_here(-1, 100, 101));
}

namespace {
void do_nothing(int /*signal*/) {}
}  // namespace

TEST_F(RealSyscallsTest, AWaitThatASignalEndsIsNotAnErrorAndNothingIsReady) {
  // A handler without SA_RESTART, so that poll is interrupted as it is by the
  // controller's own: the alarm ends a ten-second wait.
  struct sigaction handler {};
  handler.sa_handler = do_nothing;  // NOLINT(cppcoreguidelines-pro-type-union-access)
  ::sigemptyset(&handler.sa_mask);
  struct sigaction before {};
  ASSERT_EQ(::sigaction(SIGALRM, &handler, &before), 0);
  auto created = syscalls.create_pipe();
  ASSERT_TRUE(created.has_value());
  const PipeEnds quiet = created.value();

  ::alarm(1);
  auto interrupted = syscalls.poll_readable({quiet.read_fd}, 10'000);
  ::alarm(0);
  ASSERT_EQ(::sigaction(SIGALRM, &before, nullptr), 0);

  ASSERT_TRUE(interrupted.has_value()) << describe(interrupted.error());
  ASSERT_EQ(interrupted.value().size(), 1U);
  EXPECT_EQ(interrupted.value().front(), Readiness{});
  EXPECT_TRUE(syscalls.close(quiet.read_fd).has_value());
  EXPECT_TRUE(syscalls.close(quiet.write_fd).has_value());
}

TEST_F(RealSyscallsTest, ProcessIdIsTheKernelsAnswer) {
  EXPECT_EQ(syscalls.process_id(), ::getpid());
}

TEST_F(RealSyscallsTest, MappingZeroBytesOfAMemoryFileIsEINVAL) {
  auto fd = syscalls.create_memory_file("loadforge-test", 4096);
  ASSERT_TRUE(fd.has_value());
  auto mapped = syscalls.map_shared(fd.value(), 0);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().number, EINVAL);
  EXPECT_TRUE(syscalls.close(fd.value()).has_value());
}

// --- performance counters ------------------------------------------------------
//
// Whether this machine grants a counter depends on its PMU, on
// perf_event_paranoid and on a container's seccomp filter, so a test cannot
// choose between success and refusal. It asserts what holds either way -- a
// granted counter works end to end, a refused one carries the kernel's errno
// and names the call -- and the arms the machine does not take are reached
// through the files and descriptors below, which behave the same everywhere.

TEST_F(RealSyscallsTest, ASoftwareCounterIsGrantedAndWorksOrIsRefusedWithTheKernelsErrno) {
  PerfEventSpec spec;
  spec.type = 1;    // PERF_TYPE_SOFTWARE
  spec.config = 3;  // PERF_COUNT_SW_CONTEXT_SWITCHES
  for (const bool user_only : {false, true}) {
    spec.user_only = user_only;
    spec.inherit = user_only;
    auto opened = syscalls.open_perf_event(spec);
    if (!opened.has_value()) {
      EXPECT_EQ(opened.error().call, "perf_event_open");
      EXPECT_EQ(opened.error().subject, "perf event type 1 config 3");
      EXPECT_TRUE(opened.error().number == EACCES || opened.error().number == EPERM ||
                  opened.error().number == ENOSYS || opened.error().number == ENOENT)
          << describe(opened.error());
      continue;
    }
    const int fd = opened.value();
    EXPECT_TRUE(syscalls.control_perf_event(fd, PerfAction::kReset).has_value());
    EXPECT_TRUE(syscalls.control_perf_event(fd, PerfAction::kEnable).has_value());
    EXPECT_TRUE(syscalls.control_perf_event(fd, PerfAction::kDisable).has_value());
    auto count = syscalls.read_perf_count(fd);
    ASSERT_TRUE(count.has_value());
    EXPECT_LE(count.value().time_running, count.value().time_enabled);
    EXPECT_TRUE(syscalls.close(fd).has_value());
  }
}

TEST_F(RealSyscallsTest, ACounterOnAPmuThatDoesNotExistIsRefused) {
  PerfEventSpec spec;
  spec.type = 0xFFFFFF;
  auto opened = syscalls.open_perf_event(spec);
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().call, "perf_event_open");
  EXPECT_EQ(opened.error().subject, "perf event type 16777215 config 0");
  EXPECT_NE(opened.error().number, 0);
}

TEST_F(RealSyscallsTest, ControllingADescriptorThatIsNotACounterIsRefused) {
  const std::string path = write("plain", "x");
  auto plain = syscalls.open_read(path);
  ASSERT_TRUE(plain.has_value());
  for (const PerfAction action : {PerfAction::kReset, PerfAction::kEnable, PerfAction::kDisable}) {
    auto refused = syscalls.control_perf_event(plain.value(), action);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().number, ENOTTY);
    EXPECT_EQ(refused.error().call, "ioctl");
    auto closed_fd = syscalls.control_perf_event(-1, action);
    ASSERT_FALSE(closed_fd.has_value());
    EXPECT_EQ(closed_fd.error().number, EBADF);
  }
  EXPECT_TRUE(syscalls.close(plain.value()).has_value());
}

TEST_F(RealSyscallsTest, AnActionOutsideTheEnumerationIsEINVALWithoutCallingTheKernel) {
  auto refused = syscalls.control_perf_event(-1, static_cast<PerfAction>(99));
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().number, EINVAL) << "not EBADF: the request was never issued";
  EXPECT_EQ(refused.error().subject, "fd -1");
}

TEST_F(RealSyscallsTest, ACounterReadIsThreeWordsAndNothingShorter) {
  std::array<std::uint64_t, 3> words{7, 900, 450};
  const std::string whole(reinterpret_cast<const char*>(words.data()), sizeof(words));
  auto whole_fd = syscalls.open_read(write("whole", whole));
  ASSERT_TRUE(whole_fd.has_value());
  auto count = syscalls.read_perf_count(whole_fd.value());
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(count.value(), (PerfCount{7, 900, 450}));
  EXPECT_TRUE(syscalls.close(whole_fd.value()).has_value());

  auto short_fd = syscalls.open_read(write("short", whole.substr(0, 16)));
  ASSERT_TRUE(short_fd.has_value());
  auto torn = syscalls.read_perf_count(short_fd.value());
  ASSERT_FALSE(torn.has_value());
  EXPECT_EQ(torn.error().number, EIO);
  EXPECT_EQ(torn.error().call, "read");
  EXPECT_TRUE(syscalls.close(short_fd.value()).has_value());

  auto bad = syscalls.read_perf_count(-1);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().number, EBADF);
}

}  // namespace
}  // namespace loadforge::platform
