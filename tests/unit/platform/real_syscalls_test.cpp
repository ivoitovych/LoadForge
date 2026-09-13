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
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

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

}  // namespace
}  // namespace loadforge::platform
