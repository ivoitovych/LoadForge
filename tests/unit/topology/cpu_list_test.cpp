// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the sysfs CPU-list parser.
//
// The shapes below are not invented. They are what this machine's kernel
// actually writes, read out of /sys before any of this was designed:
//
//   /sys/devices/system/cpu/online                    "0-3"
//   /sys/devices/system/cpu/offline                   ""
//   /sys/devices/system/cpu/cpu0/topology/
//       thread_siblings_list                          "0"
//       core_siblings_list                            "0-3"
//   /sys/devices/system/cpu/cpu0/cache/index3/
//       shared_cpu_list                               "0-3"
//
// The comma-and-range forms come from machines with CPUs offline, which this
// one does not have -- so those cases are constructed, and the strictness rules
// are derived from what cpumask_print_to_pagebuf can emit rather than from
// guesses about what a file might contain.
#include "topology/cpu_list.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::topology {
namespace {

using Ids = std::vector<std::uint32_t>;

Ids parsed(std::string_view text) {
  auto list = CpuList::parse(text);
  EXPECT_TRUE(list) << (list ? "" : describe(list.error()));
  return list ? list.value().ids() : Ids{};
}

// --- the shapes this machine's kernel really writes ---------------------------

TEST(CpuListTest, ARangeIsExpanded) {
  // /sys/devices/system/cpu/online on this machine.
  EXPECT_EQ(parsed("0-3"), (Ids{0, 1, 2, 3}));
}

TEST(CpuListTest, ASingleIdIsAccepted) {
  // thread_siblings_list on a machine without SMT -- this one.
  EXPECT_EQ(parsed("0"), (Ids{0}));
}

TEST(CpuListTest, EmptyIsALegitimateReadingAndNotAnError) {
  // THE CASE A STRICTER PARSER WOULD GET WRONG. /sys/.../cpu/offline is EMPTY
  // on a healthy machine. Rejecting it would fail on the ordinary case, and
  // would collapse "no CPUs are offline" into "I could not find out" -- two
  // different facts (F3).
  auto list = CpuList::parse("");
  ASSERT_TRUE(list);
  EXPECT_TRUE(list.value().empty());
  EXPECT_EQ(list.value().size(), 0U);
}

TEST(CpuListTest, MixedSinglesAndRangesAreExpandedInOrder) {
  // What `online` looks like with CPUs 1, 5 and 6 offline.
  EXPECT_EQ(parsed("0,2-4,7"), (Ids{0, 2, 3, 4, 7}));
}

TEST(CpuListTest, ARangeOfOneIsAccepted) {
  // The kernel writes "5" rather than "5-5", but the latter is well-formed and
  // refusing it would be strictness with no argument behind it.
  EXPECT_EQ(parsed("5-5"), (Ids{5}));
}

TEST(CpuListTest, ALargeRealisticMachineParses) {
  // 128 CPUs, the shape a two-socket server writes.
  auto list = CpuList::parse("0-127");
  ASSERT_TRUE(list);
  EXPECT_EQ(list.value().size(), 128U);
  EXPECT_EQ(list.value().ids().front(), 0U);
  EXPECT_EQ(list.value().ids().back(), 127U);
}

// --- membership ---------------------------------------------------------------

TEST(CpuListTest, MembershipAnswersForPresentAndAbsentIds) {
  auto list = CpuList::parse("0,2-4,7");
  ASSERT_TRUE(list);
  for (const std::uint32_t present : {0U, 2U, 3U, 4U, 7U}) {
    EXPECT_TRUE(list.value().contains(present)) << present;
  }
  for (const std::uint32_t absent : {1U, 5U, 6U, 8U, 1000U}) {
    EXPECT_FALSE(list.value().contains(absent)) << absent;
  }
}

TEST(CpuListTest, AnEmptyListContainsNothing) {
  auto list = CpuList::parse("");
  ASSERT_TRUE(list);
  EXPECT_FALSE(list.value().contains(0));
}

// --- the boundary of the id space --------------------------------------------

TEST(CpuListTest, TheLargestAcceptedIdIsAccepted) {
  auto list = CpuList::parse(std::to_string(CpuList::kMaxCpuId));
  ASSERT_TRUE(list) << describe(list.error());
  EXPECT_EQ(list.value().ids(), (Ids{CpuList::kMaxCpuId}));
}

TEST(CpuListTest, AnIdPastTheLimitIsRefused) {
  auto list = CpuList::parse(std::to_string(CpuList::kMaxCpuId + 1));
  ASSERT_FALSE(list);
  EXPECT_NE(std::string{list.error().reason}.find("larger than any CPU"), std::string::npos);
}

TEST(CpuListTest, ADigitRunThatWouldWrapACounterIsRefused) {
  // THE CASE THE BOUND IS CHECKED INSIDE THE LOOP FOR. Twenty-five digits
  // overflow a 64-bit accumulator and land back on a small, entirely plausible
  // id -- so a file containing this would parse as a real CPU rather than fail.
  auto list = CpuList::parse("9999999999999999999999999");
  ASSERT_FALSE(list);
  EXPECT_NE(std::string{list.error().reason}.find("larger than any CPU"), std::string::npos);
}

TEST(CpuListTest, AnIdPastTheLimitInsideARangeIsRefused) {
  auto list = CpuList::parse("0-" + std::to_string(CpuList::kMaxCpuId + 1));
  ASSERT_FALSE(list);
}

// --- everything the kernel's own formatter cannot emit ------------------------

TEST(CpuListTest, ADescendingRangeIsRefusedRatherThanReversed) {
  // Not an empty range to skip, not a range to helpfully reverse.
  // cpumask_print_to_pagebuf cannot produce it, so the file is not what we think.
  auto list = CpuList::parse("3-0");
  ASSERT_FALSE(list);
  EXPECT_NE(std::string{list.error().reason}.find("ends before it begins"), std::string::npos);
}

TEST(CpuListTest, DescendingElementsAreRefused) {
  auto list = CpuList::parse("3,1");
  ASSERT_FALSE(list);
  EXPECT_NE(std::string{list.error().reason}.find("ascending"), std::string::npos);
}

TEST(CpuListTest, ADuplicateIdIsRefused) {
  auto list = CpuList::parse("0,0");
  ASSERT_FALSE(list);
  EXPECT_NE(std::string{list.error().reason}.find("ascending"), std::string::npos);
}

TEST(CpuListTest, OverlappingRangesAreRefused) {
  auto list = CpuList::parse("2-4,3-5");
  ASSERT_FALSE(list);
}

TEST(CpuListTest, AdjacentButNotOverlappingIsStillRefused) {
  // "0-1,1-2" shares exactly one id. Refused for the same reason as any other
  // overlap: the kernel would have written "0-2".
  auto list = CpuList::parse("0-1,1-2");
  ASSERT_FALSE(list);
}

TEST(CpuListTest, EveryMalformedShapeIsRefused) {
  // Each of these has a plausible "obvious" interpretation, which is exactly
  // why guessing is the wrong behaviour: a wrong topology is worse than a
  // reported failure, because nothing downstream would question it.
  for (const std::string_view text : {
           "a",      // not a number at all
           "0-",     // range missing its end
           "-3",     // range missing its start
           "0,,1",   // empty element between separators
           "0,",     // trailing comma
           ",0",     // leading comma
           "0-1-2",  // two dashes
           "0 1",    // space-separated, which the kernel never writes
           " 0-3",   // leading space: read_first_line does not trim, and a
                     // caller that trimmed elsewhere would hide this
           "0-3 ",   // trailing space
           "0x3",    // hexadecimal, which strtoul would have accepted as 3
           "+1",     // a sign, which strtoul would also have accepted
           "-1",     // a negative id
           "0..3",   // a different language's range syntax
       }) {
    auto list = CpuList::parse(text);
    EXPECT_FALSE(list) << "accepted malformed input: \"" << text << "\"";
  }
}

TEST(CpuListTest, AHexadecimalIdIsNotSilentlyAccepted) {
  // Stated on its own because this is what strtoul would have done: read "0x10"
  // as 16 and report success. The parser accepts digits only, so a file that
  // somehow contained hex fails instead of inventing CPU 16.
  auto list = CpuList::parse("0x10");
  ASSERT_FALSE(list);
  EXPECT_NE(std::string{list.error().reason}.find("not a digit"), std::string::npos);
}

// --- what a failure tells the reader ------------------------------------------

TEST(CpuListTest, TheFailureQuotesTheTextItCouldNotInterpret) {
  // The value is machine-reported, so a reader debugging this has no file of
  // their own to look at. Quoting the text is the only way they learn what the
  // kernel actually said.
  auto list = CpuList::parse("0,,1");
  ASSERT_FALSE(list);
  EXPECT_EQ(list.error().value, "0,,1");
  EXPECT_NE(describe(list.error()).find("\"0,,1\""), std::string::npos);
  EXPECT_NE(describe(list.error()).find("missing its number"), std::string::npos);
}

TEST(CpuListTest, TheFailureQuotesTheWholeValueNotJustTheBadElement) {
  // "2-4,3-5" is only wrong when both elements are seen together, so an error
  // naming just "3-5" would look correct in isolation and waste the reader's
  // time.
  auto list = CpuList::parse("2-4,3-5");
  ASSERT_FALSE(list);
  EXPECT_EQ(list.error().value, "2-4,3-5");
}

// --- the default-constructed list ---------------------------------------------

TEST(CpuListTest, ADefaultConstructedListIsEmpty) {
  const CpuList list;
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0U);
  EXPECT_FALSE(list.contains(0));
}

TEST(CpuListTest, EqualityComparesTheIds) {
  auto first = CpuList::parse("0-2");
  auto second = CpuList::parse("0,1,2");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value()) << "the same set written two ways is the same set";

  auto different = CpuList::parse("0-3");
  ASSERT_TRUE(different);
  EXPECT_NE(first.value(), different.value());
}

// --- T8: the files themselves, read through the real seam ---------------------
//
// Everything above is text this author typed. These read the actual files this
// kernel writes, through the real FileSystem and the real syscalls, because a
// parser tested only against invented strings is testing its author's idea of
// the format (F21). It is also the only tier that would notice the kernel
// changing the format under us.

class RealSysfs : public ::testing::Test {
 protected:
  /// Reads a sysfs value, or nullopt when the file does not exist on this
  /// machine. Absence is not a failure here: `offline` is present everywhere,
  /// but a container or an unusual kernel may omit others, and skipping is
  /// honest where refusing would just be this suite disagreeing with reality.
  std::optional<std::string> read(const std::string& path) {
    auto value = files.read_first_line(path);
    return value ? std::optional{value.value()} : std::nullopt;
  }

  platform::RealSyscalls syscalls;
  platform::FileSystem files{syscalls};
};

TEST_F(RealSysfs, TheOnlineListParsesAndAgreesWithTheRunningMachine) {
  const auto text = read("/sys/devices/system/cpu/online");
  ASSERT_TRUE(text.has_value()) << "/sys/devices/system/cpu/online should exist on any Linux";

  auto list = CpuList::parse(*text);
  ASSERT_TRUE(list) << "kernel wrote \"" << *text << "\": " << describe(list.error());

  // A machine running this test has at least one CPU online, and the count must
  // match what the C library reports -- an independent source, so the two
  // agreeing is evidence rather than a tautology.
  EXPECT_GE(list.value().size(), 1U);
  EXPECT_EQ(list.value().size(), static_cast<std::size_t>(::sysconf(_SC_NPROCESSORS_ONLN)))
      << "parsed \"" << *text << "\"";
}

TEST_F(RealSysfs, TheOfflineListIsUsuallyEmptyAndThatIsNotAnError) {
  // The case a stricter parser would fail on, against the real file.
  const auto text = read("/sys/devices/system/cpu/offline");
  ASSERT_TRUE(text.has_value());

  auto list = CpuList::parse(*text);
  ASSERT_TRUE(list) << "kernel wrote \"" << *text << "\": " << describe(list.error());
  // Whatever it says, online and offline must not overlap.
  const auto online = CpuList::parse(*read("/sys/devices/system/cpu/online"));
  ASSERT_TRUE(online);
  for (const std::uint32_t id : list.value().ids()) {
    EXPECT_FALSE(online.value().contains(id)) << "CPU " << id << " is both online and offline";
  }
}

TEST_F(RealSysfs, EveryOnlineCpusTopologyListsParse) {
  const auto text = read("/sys/devices/system/cpu/online");
  ASSERT_TRUE(text.has_value());
  auto online = CpuList::parse(*text);
  ASSERT_TRUE(online);

  int checked = 0;
  for (const std::uint32_t cpu : online.value().ids()) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    for (const std::string_view name : {"thread_siblings_list", "core_siblings_list"}) {
      const auto value = read(base + std::string{name});
      if (!value) {
        continue;  // Not every kernel exposes every file; absence is not malformed.
      }
      auto list = CpuList::parse(*value);
      ASSERT_TRUE(list) << base << name << " = \"" << *value << "\": " << describe(list.error());

      // A CPU is always its own thread sibling and its own core sibling. If this
      // ever fails, either the parser or this author's model of sysfs is wrong.
      EXPECT_TRUE(list.value().contains(cpu))
          << "cpu" << cpu << " missing from its own " << name << ": \"" << *value << "\"";
      ++checked;
    }
  }
  EXPECT_GT(checked, 0) << "no topology files were readable; this tier proved nothing";
}

TEST_F(RealSysfs, CacheSharingListsParseAndIncludeTheirOwnCpu) {
  int checked = 0;
  for (int index = 0; index < 8; ++index) {
    const std::string path =
        "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(index) + "/shared_cpu_list";
    const auto value = read(path);
    if (!value) {
      continue;
    }
    auto list = CpuList::parse(*value);
    ASSERT_TRUE(list) << path << " = \"" << *value << "\": " << describe(list.error());
    EXPECT_TRUE(list.value().contains(0)) << path << " excludes the CPU it belongs to";
    ++checked;
  }
  EXPECT_GT(checked, 0) << "no cache topology was readable; this tier proved nothing";
}

}  // namespace
}  // namespace loadforge::topology
