// SPDX-License-Identifier: GPL-3.0-or-later
//
// The capability model (F3) under test: absent, empty, denied, malformed,
// vanished and merely broken are six different facts, and this file's job is to
// show that no two of them collapse.
//
// TWO TIERS, AND WHY BOTH ARE NEEDED
// ----------------------------------
// T2 drives FakeSyscalls, because EACCES is not reachable any other way here:
// the tests run as root (euid 0 in CI and in the container), and root bypasses
// the DAC check that produces it. A chmod-000 file is readable by root, so a
// "real" EACCES test would pass by reading the file and prove nothing. The seam
// exists for exactly this.
//
// T8 drives RealSyscalls against a real directory, because a fake agrees with
// whatever the code believes (F21). In particular kVanished -- the state this
// whole module was written for -- is provoked here the way the kernel actually
// provokes it: read a file, unlink it, read it again.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// RealSourceTest below constructs the hostile trees this module's capability
// paths need, rather than reading a tree committed under tests/fixtures/. That
// is not laziness, and tools/check-test-obligations.py accepts it for a stated
// reason: neither state that matters here CAN be committed. "Vanishes mid-run"
// is an event, not a state -- only a test that reads a file and then removes it
// produces it -- and a mode-000 file arrives from a clone readable, because git
// records the execute bit and nothing else. A committed fixture for either
// would pass while proving the opposite of what it claims.

#include "topology/source.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "topology/cpu_list.hpp"

namespace loadforge::topology {
namespace {

using loadforge::testing::FakeSyscalls;

// --- describe ----------------------------------------------------------------

TEST(AvailabilityTest, EveryStateHasItsOwnWords) {
  EXPECT_EQ(describe(Availability::kPresent), "present");
  EXPECT_EQ(describe(Availability::kAbsent), "not offered by this kernel");
  EXPECT_EQ(describe(Availability::kVanished), "gone since it was last read");
  EXPECT_EQ(describe(Availability::kDenied), "not permitted");
  EXPECT_EQ(describe(Availability::kMalformed), "not in the expected format");
  EXPECT_EQ(describe(Availability::kUnreadable), "unreadable");
}

TEST(AvailabilityTest, NoTwoStatesShareAWording) {
  // The point of the enum is that a report can tell these apart. If two
  // rendered the same, a user reading a log could not, and the distinction
  // would be real only inside the process.
  const std::string_view words[] = {
      describe(Availability::kPresent),   describe(Availability::kAbsent),
      describe(Availability::kVanished),  describe(Availability::kDenied),
      describe(Availability::kMalformed), describe(Availability::kUnreadable),
  };
  for (std::size_t i = 0; i < std::size(words); ++i) {
    for (std::size_t j = i + 1; j < std::size(words); ++j) {
      EXPECT_NE(words[i], words[j]) << "states " << i << " and " << j << " render identically";
    }
  }
}

TEST(AvailabilityTest, AValueOutsideTheEnumerationDegradesGracefully) {
  // Reachable because a scoped enum's value range is wider than its
  // enumerators, and it is what a future enumerator added without updating
  // describe would hit. It must say something, not return a blank a report
  // would render as nothing at all.
  EXPECT_EQ(describe(static_cast<Availability>(99)), "unrecognised availability");
}

TEST(UnavailableTest, DescriptionNamesThePathTheStateAndTheEvidence) {
  const Unavailable unavailable{Availability::kDenied, "/sys/x/y",
                                "open(/sys/x/y): Permission denied"};
  EXPECT_EQ(describe(unavailable), "/sys/x/y: not permitted (open(/sys/x/y): Permission denied)");
}

// --- classify ----------------------------------------------------------------
//
// Tested directly as well as through Source, because this is where the
// judgement lives and a table of errno-to-meaning that can only be reached
// through three layers is a table that quietly rots.

platform::SyscallError failure(int number) { return platform::SyscallError{number, "open", "/p"}; }

TEST(ClassifyTest, TheThreeWaysSysfsSaysThereIsNothingHereAreAbsentWhenUnseen) {
  for (const int number : {ENOENT, ENODEV, ENXIO}) {
    EXPECT_EQ(classify(failure(number), false), Availability::kAbsent) << "errno " << number;
  }
}

TEST(ClassifyTest, TheSameErrnosAreVanishedOnceTheSourceHasBeenRead) {
  for (const int number : {ENOENT, ENODEV, ENXIO}) {
    EXPECT_EQ(classify(failure(number), true), Availability::kVanished) << "errno " << number;
  }
}

TEST(ClassifyTest, PermissionIsDeniedWhetherOrNotTheSourceWasEverRead) {
  // The asymmetry with ENOENT is the point, and it is deliberate: a device
  // going away does not revoke a permission. Promoting this to kVanished would
  // send a user looking for a hardware change when what they need is a group
  // membership.
  for (const int number : {EACCES, EPERM}) {
    EXPECT_EQ(classify(failure(number), false), Availability::kDenied) << "errno " << number;
    EXPECT_EQ(classify(failure(number), true), Availability::kDenied) << "errno " << number;
  }
}

TEST(ClassifyTest, EverythingElseIsUnreadableRatherThanFoldedIntoAbsent) {
  // EISDIR and ENOTDIR are the ones that matter most: both mean OUR PATH IS
  // WRONG. Calling them absent would turn a bug in our own path construction
  // into a confident statement about the user's hardware.
  for (const int number : {EIO, EISDIR, ENOTDIR, EINTR, EFBIG, ENOMEM}) {
    EXPECT_EQ(classify(failure(number), false), Availability::kUnreadable) << "errno " << number;
    EXPECT_EQ(classify(failure(number), true), Availability::kUnreadable) << "errno " << number;
  }
}

// --- Source against the fake (T2) --------------------------------------------

class SourceTest : public ::testing::Test {
 protected:
  FakeSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
};

TEST_F(SourceTest, AValueIsReadAndTheSourceRemembersHavingReadIt) {
  syscalls.set_content("0-3\n");
  Source source(filesystem, "/sys/devices/system/cpu/online");

  EXPECT_FALSE(source.has_been_read());
  auto text = source.text();
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text.value(), "0-3");
  EXPECT_TRUE(source.has_been_read());
  EXPECT_EQ(source.path(), "/sys/devices/system/cpu/online");
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(SourceTest, AnEmptyValueIsASuccessNotAnAbsence) {
  // `offline` is empty on a machine with every CPU up. A reader that treated
  // empty as "no value" would report a healthy machine as an unreadable one --
  // and would collapse two facts F3 exists to keep apart.
  syscalls.set_content("\n");
  Source source(filesystem, "/sys/devices/system/cpu/offline");

  auto text = source.text();
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text.value(), "");
  EXPECT_TRUE(source.has_been_read());
}

TEST_F(SourceTest, AMissingSourceNeverReadIsAbsent) {
  syscalls.fail_open(ENOENT);
  Source source(filesystem, "/sys/class/powercap/intel-rapl:0/energy_uj");

  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kAbsent);
  EXPECT_EQ(text.error().path, "/sys/class/powercap/intel-rapl:0/energy_uj");
  EXPECT_NE(text.error().detail.find("No such file"), std::string::npos);
  EXPECT_FALSE(source.has_been_read());
}

TEST_F(SourceTest, AMissingSourceThatWasReadBeforeIsVanished) {
  // The distinction this module was written to make, and the one no stateless
  // classifier can produce: the errno is identical, only the history differs.
  syscalls.set_content("0-3\n");
  Source source(filesystem, "/sys/devices/system/cpu/cpu3/topology/core_id");
  ASSERT_TRUE(source.text().has_value());

  syscalls.fail_open(ENOENT);
  auto second = source.text();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, Availability::kVanished);
}

TEST_F(SourceTest, AFailedReadDoesNotCountAsHavingBeenRead) {
  // The negative half of the test above, and the one that would have caught the
  // obvious implementation: setting `seen_` on entry, or before checking the
  // result, makes the SECOND failure of a source that never worked report as
  // kVanished -- a machine change that never happened.
  syscalls.fail_open(EACCES);
  Source source(filesystem, "/sys/kernel/debug/x");
  ASSERT_FALSE(source.text().has_value());
  EXPECT_FALSE(source.has_been_read());

  syscalls.fail_open(ENOENT);
  auto second = source.text();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, Availability::kAbsent)
      << "a source that never worked cannot vanish";
}

TEST_F(SourceTest, ADeniedSourceStaysDeniedAfterASuccessfulRead) {
  syscalls.set_content("42\n");
  Source source(filesystem, "/sys/class/powercap/intel-rapl:0/energy_uj");
  ASSERT_TRUE(source.text().has_value());

  syscalls.fail_open(EACCES);
  auto second = source.text();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, Availability::kDenied);
}

TEST_F(SourceTest, AFailureMidReadIsReportedRatherThanTruncated) {
  // The read failure arrives AFTER a successful open, which is the sysfs shape
  // the FileSystem class was written for: the driver's permission check and its
  // I/O errors both happen at read time, not at open time.
  syscalls.script_reads({{"0-", 0}, {"", EIO}});
  Source source(filesystem, "/sys/devices/system/cpu/online");

  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kUnreadable);
  EXPECT_FALSE(source.has_been_read()) << "a partial read is not a reading";
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(SourceTest, ADeniedReadAfterASuccessfulOpenIsStillDenied) {
  syscalls.always_fail_read(EACCES);
  Source source(filesystem, "/sys/class/powercap/intel-rapl:0/energy_uj");

  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kDenied);
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

// --- cpu_list ----------------------------------------------------------------

TEST_F(SourceTest, ACpuListIsParsedFromTheValue) {
  syscalls.set_content("0,2-4\n");
  Source source(filesystem, "/sys/devices/system/cpu/online");

  auto list = source.cpu_list();
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(list.value().ids(), (std::vector<std::uint32_t>{0, 2, 3, 4}));
}

TEST_F(SourceTest, AnEmptyCpuListIsAValueNotAnError) {
  syscalls.set_content("\n");
  Source source(filesystem, "/sys/devices/system/cpu/offline");

  auto list = source.cpu_list();
  ASSERT_TRUE(list.has_value());
  EXPECT_TRUE(list.value().empty());
}

TEST_F(SourceTest, TextThatIsNotACpuListIsMalformedAndQuotesItself) {
  syscalls.set_content("3-0\n");
  Source source(filesystem, "/sys/devices/system/cpu/online");

  auto list = source.cpu_list();
  ASSERT_FALSE(list.has_value());
  EXPECT_EQ(list.error().kind, Availability::kMalformed);
  EXPECT_EQ(list.error().path, "/sys/devices/system/cpu/online");
  EXPECT_NE(list.error().detail.find("3-0"), std::string::npos)
      << "the offending text must survive into the report";
  EXPECT_TRUE(source.has_been_read())
      << "the file was there and was read; only its contents were wrong";
}

TEST_F(SourceTest, ASourceThatWasMalformedCanStillVanish) {
  // Follows from the assertion above: a malformed read is still a read, so the
  // file demonstrably existed, and an ENOENT afterwards is a machine change.
  syscalls.set_content("nonsense\n");
  Source source(filesystem, "/sys/devices/system/cpu/online");
  ASSERT_FALSE(source.cpu_list().has_value());

  syscalls.fail_open(ENOENT);
  auto second = source.cpu_list();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, Availability::kVanished);
}

TEST_F(SourceTest, ACpuListPropagatesAReadFailureUnchanged) {
  // One ladder, not two. A caller must not have to unpack a Result of a Result
  // to find out whether the source was absent -- the second check is the one
  // written less carefully.
  syscalls.fail_open(ENOENT);
  Source source(filesystem, "/sys/devices/system/cpu/online");

  auto list = source.cpu_list();
  ASSERT_FALSE(list.has_value());
  // Field by field, deliberately. This copy out of one Result and into another
  // is the one clang-analyzer claims yields a garbage `kind`; it cannot follow a
  // value through std::variant. The suppression on Unavailable cites this test,
  // so the test has to actually check every field rather than just the one.
  EXPECT_EQ(list.error().kind, Availability::kAbsent);
  EXPECT_EQ(list.error().path, "/sys/devices/system/cpu/online");
  EXPECT_NE(list.error().detail.find("No such file"), std::string::npos);
}

// --- Source against a real kernel (T8) ---------------------------------------

class RealSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory = std::filesystem::temp_directory_path() /
                ("loadforge-source-" + std::to_string(::getpid()) + "-" +
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
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
};

TEST_F(RealSourceTest, AFileThatIsUnlinkedBetweenTwoReadsIsVanished) {
  // The state the module exists for, provoked the way the kernel provokes it.
  // Nothing is faked: the first read succeeds against a real file, the file is
  // really removed, and the second read gets a real ENOENT.
  const std::string path = write("online", "0-3\n");
  Source source(filesystem, path);

  auto first = source.cpu_list();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first.value().size(), 4U);

  ASSERT_TRUE(std::filesystem::remove(path));

  auto second = source.cpu_list();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().kind, Availability::kVanished);
}

TEST_F(RealSourceTest, APathThatNeverExistedIsAbsent) {
  Source source(filesystem, (directory / "never-here").string());
  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kAbsent);
}

TEST_F(RealSourceTest, ADirectoryIsUnreadableRatherThanAbsent) {
  // A real EISDIR, and it arrives from read(2) rather than open(2) -- which is
  // why classify has to handle it at all.
  Source source(filesystem, directory.string());
  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kUnreadable);
}

TEST_F(RealSourceTest, APathWhoseParentIsAFileIsUnreadableRatherThanAbsent) {
  // A real ENOTDIR, verified against this kernel. The distinction matters: this
  // is a wrong path, not a missing capability, and reporting it as kAbsent
  // would state as fact something about the machine that is actually a bug here.
  const std::string file = write("attribute", "0-3\n");
  Source source(filesystem, file + "/child");
  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kUnreadable);
}

TEST_F(RealSourceTest, AnEmptyRealFileReadsAsAnEmptyValue) {
  const std::string path = write("offline", "");
  Source source(filesystem, path);

  auto list = source.cpu_list();
  ASSERT_TRUE(list.has_value());
  EXPECT_TRUE(list.value().empty());
  EXPECT_TRUE(source.has_been_read());
}

TEST_F(RealSourceTest, TheKernelsOwnOnlineListReadsAndParses) {
  // The real thing, on the machine running the test: a source this kernel does
  // offer, cross-checked against sysconf so the agreement is evidence rather
  // than this code agreeing with itself (F21).
  Source source(filesystem, "/sys/devices/system/cpu/online");
  auto list = source.cpu_list();
  ASSERT_TRUE(list.has_value()) << describe(list.error());
  EXPECT_EQ(list.value().size(), static_cast<std::size_t>(::sysconf(_SC_NPROCESSORS_ONLN)));
  EXPECT_TRUE(source.has_been_read());
}

TEST_F(RealSourceTest, ASysfsAttributeThisKernelDoesNotOfferIsAbsent) {
  // A CPU id past anything a machine has. The path is well-formed and its
  // parent directory genuinely does not exist, which is exactly the shape of a
  // capability this kernel does not offer -- and is reported as such rather
  // than as a failure.
  Source source(filesystem, "/sys/devices/system/cpu/cpu99999/topology/core_id");
  auto text = source.text();
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().kind, Availability::kAbsent);
  EXPECT_NE(describe(text.error()).find("not offered by this kernel"), std::string::npos);
}

}  // namespace
}  // namespace loadforge::topology
