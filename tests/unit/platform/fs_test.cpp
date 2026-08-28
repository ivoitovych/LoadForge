// SPDX-License-Identifier: GPL-3.0-or-later
//
// P2 coverage for the file reader: every documented failure of every call it
// makes, forced rather than hoped for (docs/IMPLEMENTATION.md §2.2, §2.3).
//
// The errno list is not invented. It is what open(2) and read(2) document for
// the files this layer actually touches, and each one below corresponds to a
// real way a machine in the field differs from the machine a developer tested
// on: no permission on RAPL, a device unplugged mid-run, a driver that returns
// EIO, a signal arriving during a sample.

#include "platform/fs.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <vector>

#include "support/fake_syscalls.hpp"

namespace loadforge::platform {
namespace {

using testing::FakeSyscalls;

class FileSystemTest : public ::testing::Test {
 protected:
  FakeSyscalls syscalls;
  FileSystem fs{syscalls};
};

// ---------------------------------------------------------------- happy paths

TEST_F(FileSystemTest, ReadsAWholeFile) {
  syscalls.set_content("95000\n");

  const auto result = fs.read_file("/sys/class/thermal/thermal_zone0/temp");

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value(), "95000\n");
  EXPECT_EQ(syscalls.last_path(), "/sys/class/thermal/thermal_zone0/temp");
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(FileSystemTest, AnEmptyFileIsAValueNotAnError) {
  // "Present but empty" and "absent" are different facts about a machine, and
  // the capability model (F3) reports them differently.
  syscalls.set_content("");

  const auto result = fs.read_file("/sys/empty");

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().empty());
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(FileSystemTest, ReassemblesAShortReadSequence) {
  // sysfs may hand back a value in pieces. Losing the tail would turn 95000
  // into 95, which reads as a plausible temperature and is wrong by a factor
  // of a thousand.
  syscalls.script_reads({{"95", 0}, {"00", 0}, {"0\n", 0}, {"", 0}});

  const auto result = fs.read_file("/sys/split");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "95000\n");
}

TEST_F(FileSystemTest, RetriesAfterEINTRRatherThanFailing) {
  // The controller installs handlers for SIGCHLD and the shutdown signals, so
  // a signal arriving mid-sample is ordinary operation, not an exotic corner.
  syscalls.script_reads({{"", EINTR}, {"7", 0}, {"", EINTR}, {"2\n", 0}, {"", 0}});

  const auto result = fs.read_file("/sys/interrupted");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "72\n");
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

// --------------------------------------------------------------- open failures

class OpenFailure : public FileSystemTest,
                    public ::testing::WithParamInterface<std::pair<int, const char*>> {};

TEST_P(OpenFailure, IsReportedWithTheCallAndThePath) {
  const auto [error_number, why] = GetParam();
  syscalls.fail_open(error_number);

  const auto result = fs.read_file("/sys/class/powercap/intel-rapl:0/energy_uj");

  ASSERT_FALSE(result.has_value()) << "expected failure: " << why;
  EXPECT_EQ(result.error().number, error_number);
  EXPECT_EQ(result.error().call, "open");
  EXPECT_EQ(result.error().subject, "/sys/class/powercap/intel-rapl:0/energy_uj");

  // The message must name the file, or a user cannot act on it.
  const std::string message = describe(result.error());
  EXPECT_NE(message.find("energy_uj"), std::string::npos) << message;
  EXPECT_NE(message.find("open"), std::string::npos) << message;

  // Nothing was opened, so nothing must be closed -- and no descriptor leaked.
  EXPECT_EQ(syscalls.open_count(), 0);
  EXPECT_EQ(syscalls.close_count(), 0);
}

INSTANTIATE_TEST_SUITE_P(
    DocumentedErrnos, OpenFailure,
    ::testing::Values(
        std::pair{EACCES, "no permission -- the RAPL case, and the most common in the field"},
        std::pair{ENOENT, "the sensor does not exist on this machine"},
        std::pair{EISDIR, "the path names a directory, e.g. a probe built the wrong path"},
        std::pair{ENODEV, "the device was unplugged between probe and read"},
        std::pair{EMFILE, "the process is out of descriptors -- a leak elsewhere"},
        std::pair{ENFILE, "the system is out of descriptors"},
        std::pair{ELOOP, "a symlink cycle in /sys"},
        std::pair{ENAMETOOLONG, "a constructed path exceeded PATH_MAX"}));

// --------------------------------------------------------------- read failures

class ReadFailure : public FileSystemTest,
                    public ::testing::WithParamInterface<std::pair<int, const char*>> {};

TEST_P(ReadFailure, IsReportedAndTheDescriptorIsStillClosed) {
  const auto [error_number, why] = GetParam();
  syscalls.script_reads({{"partial", 0}, {"", error_number}});

  const auto result = fs.read_file("/sys/failing");

  ASSERT_FALSE(result.has_value()) << "expected failure: " << why;
  EXPECT_EQ(result.error().number, error_number);
  EXPECT_EQ(result.error().call, "read");

  // The leak that matters: five hours of sampling at a few files per second
  // exhausts the process long before the run ends, and it is the error path
  // that survives review unexamined.
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

INSTANTIATE_TEST_SUITE_P(
    DocumentedErrnos, ReadFailure,
    ::testing::Values(std::pair{EACCES,
                                "permission checked in the driver at read time, not at open"},
                      std::pair{EIO, "the driver failed -- the sensor is there but not answering"},
                      std::pair{ENODEV, "the device disappeared mid-read"},
                      std::pair{EBADF, "the descriptor went bad under us"},
                      std::pair{EINVAL, "the driver rejects the read size"},
                      std::pair{ENXIO, "no such device or address"}));

TEST_F(FileSystemTest, AbandonsAReadThatIsInterruptedWithoutEnd) {
  // A driver or signal storm that interrupts every read would spin an unbounded
  // retry forever. A hung sampler is worse than a failed one: the watchdog sees
  // a stalled worker and no reason for it. This is the only test that can reach
  // the bound, because a finite script cannot express "forever".
  syscalls.always_fail_read(EINTR);

  const auto result = fs.read_file("/sys/interrupt-storm");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EINTR);
  EXPECT_EQ(syscalls.read_count(), FileSystem::kMaxConsecutiveInterrupts);
  // The message says it gave up, and how often, or the operator learns nothing.
  EXPECT_NE(describe(result.error()).find("abandoned after"), std::string::npos)
      << describe(result.error());
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(FileSystemTest, TheInterruptBoundCountsConsecutiveRunsNotTheWholeRead) {
  // A long file read under a busy signal load is legitimate: what is pathological
  // is an unbroken run of interruptions. Progress must reset the count, or a big
  // read on a busy machine fails for no good reason.
  std::vector<FakeSyscalls::ReadStep> steps;
  for (int block = 0; block < 4; ++block) {
    for (int i = 0; i < FileSystem::kMaxConsecutiveInterrupts - 1; ++i) {
      steps.push_back({"", EINTR});
    }
    steps.push_back({"x", 0});
  }
  steps.push_back({"", 0});
  syscalls.script_reads(steps);

  const auto result = fs.read_file("/sys/busy-but-progressing");

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value(), "xxxx");
}

TEST_F(FileSystemTest, TheSizeLimitIsReportedAsOursNotTheKernels) {
  // No system call failed here; the limit is LoadForge's judgement. Naming
  // read(2) would put words in the kernel's mouth, and a project whose doctrine
  // is never to fabricate evidence does not get to fabricate provenance either.
  const std::string chunk(FileSystem::kChunkSize, 'x');
  std::vector<FakeSyscalls::ReadStep> steps;
  for (std::size_t written = 0; written <= FileSystem::kMaxFileSize;
       written += FileSystem::kChunkSize) {
    steps.push_back({chunk, 0});
  }
  syscalls.script_reads(steps);

  const auto result = fs.read_file("/proc/runaway");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().call, "read_file");
  EXPECT_NE(result.error().call, "read");
}

TEST_F(FileSystemTest, PartialContentIsDiscardedWhenTheReadFails) {
  // Returning what was read so far would hand the caller a truncated value that
  // parses fine and is wrong. There is no partial success here.
  syscalls.script_reads({{"9500", 0}, {"", EIO}});

  const auto result = fs.read_file("/sys/truncated");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EIO);
}

// -------------------------------------------------------------- close failures

TEST_F(FileSystemTest, ACloseFailureAfterAGoodReadIsReported) {
  // The platform layer does not decide for its caller whether a failed close
  // matters; it says that it happened.
  syscalls.set_content("95000\n");
  syscalls.fail_close(EIO);

  const auto result = fs.read_file("/sys/bad-close");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EIO);
  EXPECT_EQ(result.error().call, "close");
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(FileSystemTest, CloseIsNotRetriedWhenItIsInterrupted) {
  // Linux releases the descriptor even when close reports EINTR, so retrying
  // could close a descriptor that has since been reused by another thread --
  // a bug that shows up as one part of the program reading another's file.
  // Exactly one close, whatever it returns.
  syscalls.set_content("95000\n");
  syscalls.fail_close(EINTR);

  const auto result = fs.read_file("/sys/interrupted-close");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EINTR);
  EXPECT_EQ(syscalls.close_count(), 1);
}

TEST_F(FileSystemTest, AReadFailureOutranksACloseFailure) {
  // Both failed. The read error says why the value is missing, which is what
  // the user needs; the close error would only say what happened next.
  syscalls.script_reads({{"", EIO}});
  syscalls.fail_close(EBADF);

  const auto result = fs.read_file("/sys/both-fail");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EIO);
  EXPECT_EQ(result.error().call, "read");
}

// ------------------------------------------------------------------- the limit

TEST_F(FileSystemTest, RefusesAFileLargerThanTheLimitRatherThanTruncating) {
  // A truncated value parses as a plausible wrong number. Refusing is the F20
  // position: a reader that cannot make sense of its input says so.
  const std::string chunk(FileSystem::kChunkSize, 'x');
  std::vector<FakeSyscalls::ReadStep> steps;
  for (std::size_t written = 0; written <= FileSystem::kMaxFileSize;
       written += FileSystem::kChunkSize) {
    steps.push_back({chunk, 0});
  }
  syscalls.script_reads(steps);

  const auto result = fs.read_file("/proc/runaway");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EFBIG);
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(FileSystemTest, AFileExactlyAtTheLimitIsAccepted) {
  // The boundary from the accepting side (P3): off-by-one here would reject
  // legitimate data, which is the more insidious direction of the two.
  const std::string chunk(FileSystem::kChunkSize, 'x');
  std::vector<FakeSyscalls::ReadStep> steps;
  for (std::size_t written = 0; written < FileSystem::kMaxFileSize;
       written += FileSystem::kChunkSize) {
    steps.push_back({chunk, 0});
  }
  steps.push_back({"", 0});
  syscalls.script_reads(steps);

  const auto result = fs.read_file("/proc/exactly-at-limit");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().size(), FileSystem::kMaxFileSize);
}

// ---------------------------------------------------------------- first line

TEST_F(FileSystemTest, ReadsTheFirstLineWithoutItsNewline) {
  syscalls.set_content("95000\n");
  const auto result = fs.read_first_line("/sys/temp");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "95000");
}

TEST_F(FileSystemTest, TakesOnlyTheFirstOfSeveralLines) {
  syscalls.set_content("first\nsecond\nthird\n");
  const auto result = fs.read_first_line("/sys/multi");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "first");
}

TEST_F(FileSystemTest, ToleratesAFileWithNoTrailingNewline) {
  syscalls.set_content("95000");
  const auto result = fs.read_first_line("/sys/no-newline");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "95000");
}

TEST_F(FileSystemTest, StripsACarriageReturn) {
  // A stray \r would survive into the parsed value and turn 95 into a
  // non-number, which surfaces as a confusing parse error far from here.
  syscalls.set_content("95000\r\n");
  const auto result = fs.read_first_line("/sys/crlf");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "95000");
}

TEST_F(FileSystemTest, AnEmptyFirstLineIsAnEmptyString) {
  syscalls.set_content("\nsecond\n");
  const auto result = fs.read_first_line("/sys/leading-blank");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().empty());
}

TEST_F(FileSystemTest, AnEmptyFileYieldsAnEmptyFirstLine) {
  syscalls.set_content("");
  const auto result = fs.read_first_line("/sys/empty");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().empty());
}

TEST_F(FileSystemTest, FirstLinePropagatesTheUnderlyingFailure) {
  // Every field is asserted, not just the number. clang-analyzer flags the copy
  // this exercises as reading uninitialised memory -- it cannot follow a value
  // through std::variant -- and the suppression in fs.cpp is only defensible
  // because this test shows the error arrives whole.
  syscalls.fail_open(EACCES);

  const auto result = fs.read_first_line("/sys/forbidden");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, EACCES);
  EXPECT_EQ(result.error().call, "open");
  EXPECT_EQ(result.error().subject, "/sys/forbidden");
}

}  // namespace
}  // namespace loadforge::platform
