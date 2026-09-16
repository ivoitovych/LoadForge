// SPDX-License-Identifier: GPL-3.0-or-later
//
// Discovery, and the cross-checks that are the reason it can fail on a machine
// where every individual file reads perfectly.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Every test here builds a sysfs tree on disk and drives RealSyscalls against
// it. Not a fake, and not a committed fixture:
//
//   * A FAKE would agree with whatever this code believes (F21). The whole
//     point of these tests is that the kernel publishes the same facts several
//     times over and they must agree, so a test double that answers from one
//     model cannot demonstrate anything about disagreement.
//   * A COMMITTED tree could hold most of these states, but the module takes
//     its root as a parameter precisely so the hostile arrangements can be
//     CONSTRUCTED -- and constructing them keeps each test's tree next to the
//     assertion it exists for, rather than in a directory of near-identical
//     trees named after the bug each one once caught.
//
// The real /sys is read too, at the bottom, because a tree this file wrote is
// still a tree this file believes in.

#include "topology/cpu_topology.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

class CpuTopologyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("loadforge-topo-" + std::to_string(::getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(root / "devices" / "system" / "cpu");
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << contents << "\n";
  }

  void set_online(const std::string& list) {
    write(root / "devices" / "system" / "cpu" / "online", list);
  }

  /// One CPU's topology directory, exactly as the kernel lays it out.
  void add_cpu(std::uint32_t id, const std::string& package, const std::string& core,
               const std::string& siblings) {
    const std::filesystem::path directory =
        root / "devices" / "system" / "cpu" / ("cpu" + std::to_string(id)) / "topology";
    write(directory / "physical_package_id", package);
    write(directory / "core_id", core);
    write(directory / "thread_siblings_list", siblings);
  }

  core::Result<CpuTopology, DiscoveryFailure> discover() {
    return CpuTopology::discover(filesystem, root.string());
  }

  std::filesystem::path root;
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
};

// --- the shapes a real machine has ------------------------------------------

TEST_F(CpuTopologyTest, FourSingleThreadedCoresInOnePackage) {
  // The arrangement of the machine this was written against: 4 CPUs, no SMT,
  // one socket. Probed from its /sys before the parser was designed.
  set_online("0-3");
  for (std::uint32_t id = 0; id < 4; ++id) {
    add_cpu(id, "0", std::to_string(id), std::to_string(id));
  }

  auto topology = discover();
  ASSERT_TRUE(topology.has_value()) << describe(topology.error());
  EXPECT_EQ(topology.value().logical_cpu_count(), 4U);
  EXPECT_EQ(topology.value().physical_core_count(), 4U);
  EXPECT_EQ(topology.value().package_count(), 1U);
  EXPECT_FALSE(topology.value().multithreaded());
}

TEST_F(CpuTopologyTest, TwoCoresWithTwoThreadsEach) {
  set_online("0-3");
  add_cpu(0, "0", "0", "0-1");
  add_cpu(1, "0", "0", "0-1");
  add_cpu(2, "0", "1", "2-3");
  add_cpu(3, "0", "1", "2-3");

  auto topology = discover();
  ASSERT_TRUE(topology.has_value()) << describe(topology.error());
  EXPECT_EQ(topology.value().logical_cpu_count(), 4U);
  EXPECT_EQ(topology.value().physical_core_count(), 2U);
  EXPECT_EQ(topology.value().package_count(), 1U);
  EXPECT_TRUE(topology.value().multithreaded());
}

TEST_F(CpuTopologyTest, CoreIdIsOnlyUniqueWithinItsPackage) {
  // The case CoreAddress exists for. Both packages have a core 0; counting by
  // core_id alone would merge them and report half the machine.
  set_online("0-1");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "1", "0", "1");

  auto topology = discover();
  ASSERT_TRUE(topology.has_value()) << describe(topology.error());
  EXPECT_EQ(topology.value().physical_core_count(), 2U) << "two sockets' core 0 are two cores";
  EXPECT_EQ(topology.value().package_count(), 2U);
}

TEST_F(CpuTopologyTest, OfflineCpusAreAbsentFromTheTopology) {
  // A gap in `online` is ordinary: CPUs 1 and 2 offlined. The topology must
  // describe what is running, and must not stumble over the directories the
  // kernel leaves in place for offline CPUs.
  set_online("0,3");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "0", "1", "1");  // Present on disk, not online.
  add_cpu(2, "0", "2", "2");
  add_cpu(3, "0", "3", "3");

  auto topology = discover();
  ASSERT_TRUE(topology.has_value()) << describe(topology.error());
  EXPECT_EQ(topology.value().logical_cpu_count(), 2U);
  ASSERT_EQ(topology.value().cpus().size(), 2U);
  EXPECT_EQ(topology.value().cpus()[0].id, 0U);
  EXPECT_EQ(topology.value().cpus()[1].id, 3U);
}

TEST_F(CpuTopologyTest, ASingleCpuMachineIsNotMultithreaded) {
  set_online("0");
  add_cpu(0, "0", "0", "0");

  auto topology = discover();
  ASSERT_TRUE(topology.has_value()) << describe(topology.error());
  EXPECT_EQ(topology.value().logical_cpu_count(), 1U);
  EXPECT_EQ(topology.value().physical_core_count(), 1U);
  EXPECT_FALSE(topology.value().multithreaded());
}

// --- a source that will not answer (Kind::kSource) ---------------------------

TEST_F(CpuTopologyTest, AMissingOnlineFileIsASourceFailureNotAContradiction) {
  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(topology.error().availability, Availability::kAbsent);
}

TEST_F(CpuTopologyTest, AMalformedOnlineFileIsASourceFailure) {
  set_online("3-0");
  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(topology.error().availability, Availability::kMalformed);
}

TEST_F(CpuTopologyTest, AMissingCoreIdIsASourceFailure) {
  set_online("0");
  const std::filesystem::path directory = root / "devices" / "system" / "cpu" / "cpu0" / "topology";
  write(directory / "physical_package_id", "0");
  write(directory / "thread_siblings_list", "0");
  // core_id deliberately not written.

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  // Field by field, deliberately. This failure is copied out of one Result and
  // into another, which is the copy clang-analyzer claims yields a garbage
  // `kind` because it cannot follow a value through std::variant. The
  // suppression on DiscoveryFailure cites this test, so the test has to check
  // every field rather than the one the diagnostic happens to name.
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(topology.error().availability, Availability::kAbsent);
  EXPECT_EQ(topology.error().subject, (directory / "core_id").string());
  EXPECT_NE(topology.error().detail.find("No such file"), std::string::npos);
}

TEST_F(CpuTopologyTest, AMissingSiblingListIsASourceFailure) {
  set_online("0");
  const std::filesystem::path directory = root / "devices" / "system" / "cpu" / "cpu0" / "topology";
  write(directory / "physical_package_id", "0");
  write(directory / "core_id", "0");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(topology.error().availability, Availability::kAbsent);
}

TEST_F(CpuTopologyTest, CoreIdThatIsNotANumberIsASourceFailure) {
  set_online("0");
  add_cpu(0, "0", "banana", "0");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(topology.error().availability, Availability::kMalformed);
}

// --- the machine contradicting itself (Kind::kContradiction) -----------------

TEST_F(CpuTopologyTest, NoCpuOnlineIsImpossibleRatherThanEmpty) {
  // An empty value is a legitimate reading -- CpuList accepts it, and must,
  // because `offline` is empty on every healthy machine. It is an impossible
  // FACT here, and that difference between a parser and a discovery layer is
  // the point of this test.
  set_online("");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("running on one"), std::string::npos);
}

TEST_F(CpuTopologyTest, ACoreIdOfMinusOneSaysTheKernelDoesNotKnow) {
  // Real kernels write -1 into core_id and physical_package_id when they cannot
  // determine them. Reading it as a number rather than refusing it as a
  // non-digit is what lets this report the truth: the file is exactly what the
  // kernel meant to write, and the kernel does not know.
  set_online("0");
  add_cpu(0, "0", "-1", "0");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("cannot determine"), std::string::npos);
}

TEST_F(CpuTopologyTest, APackageIdOfMinusOneIsRefusedTheSameWay) {
  set_online("0");
  add_cpu(0, "-1", "0", "0");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("cannot determine"), std::string::npos);
}

TEST_F(CpuTopologyTest, AnIdPastAnythingTheKernelCanAssignIsRefused) {
  set_online("0");
  add_cpu(0, "0", "999999", "0");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("past any id"), std::string::npos);
}

TEST_F(CpuTopologyTest, ACpuMissingFromItsOwnSiblingSetIsRefused) {
  set_online("0-1");
  add_cpu(0, "0", "0", "1");  // cpu0 says its core holds only cpu1.
  add_cpu(1, "0", "0", "0-1");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("not among its own thread siblings"),
            std::string::npos);
}

TEST_F(CpuTopologyTest, ASiblingThatIsNotOnlineIsRefusedRatherThanSkipped) {
  // The scheduler's masks and the hotplug machinery are describing different
  // machines. Dropping the missing CPU would silently report a core smaller
  // than the one the kernel thinks it has.
  set_online("0");
  add_cpu(0, "0", "0", "0-1");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("is not online"), std::string::npos);
}

TEST_F(CpuTopologyTest, SiblingSetsThatAreNotIdenticalAreRefused) {
  // Sharing a core is an equivalence relation, so the sets must be EQUAL, not
  // merely mutually mentioning. "0 lists 0-1; 1 lists 0-2" describes no
  // arrangement of hardware, and a mutual-mention check would accept it.
  set_online("0-2");
  add_cpu(0, "0", "0", "0-1");
  add_cpu(1, "0", "0", "0-2");
  add_cpu(2, "0", "0", "0-2");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("different sibling sets"), std::string::npos);
}

TEST_F(CpuTopologyTest, SiblingsReportingDifferentCoresAreRefused) {
  // The sibling sets agree exactly; the per-CPU ids do not. One of the two is
  // wrong and there is no way to tell which, so neither is trusted.
  set_online("0-1");
  add_cpu(0, "0", "0", "0-1");
  add_cpu(1, "0", "1", "0-1");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("different core or package ids"), std::string::npos);
}

TEST_F(CpuTopologyTest, SiblingsInDifferentPackagesAreRefused) {
  set_online("0-1");
  add_cpu(0, "0", "0", "0-1");
  add_cpu(1, "1", "0", "0-1");

  auto topology = discover();
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(topology.error()).find("different core or package ids"), std::string::npos);
}

// --- CoreAddress -------------------------------------------------------------

TEST(CoreAddressTest, TheKeyIsExactAcrossTheWholeRangeOfBothHalves) {
  // Exact, not a hash: a collision would silently merge two cores into one, and
  // the core count is a number every later report is attributed against.
  EXPECT_EQ(key(CoreAddress{0, 0}), 0U);
  EXPECT_NE(key(CoreAddress{0, 1}), key(CoreAddress{1, 0}))
      << "package 0 core 1 is not package 1 core 0";
  EXPECT_NE(key(CoreAddress{1, 0}), key(CoreAddress{0, 0}));

  // The extremes, where a shift that lost a bit or a sign that crept in would
  // show. 0xFFFFFFFF in both halves is past any id the kernel assigns, but the
  // encoding must not be the thing that decides that.
  constexpr std::uint32_t kMax = 0xFFFFFFFFU;
  EXPECT_EQ(key(CoreAddress{kMax, kMax}), 0xFFFFFFFFFFFFFFFFULL);
  EXPECT_EQ(key(CoreAddress{kMax, 0}), 0xFFFFFFFF00000000ULL);
  EXPECT_EQ(key(CoreAddress{0, kMax}), 0x00000000FFFFFFFFULL);
}

TEST(CoreAddressTest, EqualityComparesBothHalves) {
  EXPECT_EQ((CoreAddress{1, 2}), (CoreAddress{1, 2}));
  EXPECT_NE((CoreAddress{1, 2}), (CoreAddress{1, 3}));
  EXPECT_NE((CoreAddress{1, 2}), (CoreAddress{2, 2}));
}

// --- describe ----------------------------------------------------------------

TEST(DiscoveryFailureTest, ASourceFailureRendersLikeTheSourceItCameFrom) {
  const DiscoveryFailure failure{DiscoveryFailure::Kind::kSource, Availability::kDenied,
                                 "/sys/x/core_id", "open(/sys/x/core_id): Permission denied"};
  EXPECT_EQ(describe(failure),
            "/sys/x/core_id: not permitted (open(/sys/x/core_id): Permission denied)");
}

TEST(DiscoveryFailureTest, AContradictionSaysTheMachineDisagreesWithItself) {
  // Deliberately different wording from a source failure: the remedy is not a
  // permission or a missing attribute, and a message that reads the same would
  // send a user hunting for an access problem that does not exist.
  const DiscoveryFailure failure{DiscoveryFailure::Kind::kContradiction, Availability::kPresent,
                                 "/sys/x/topology", "cpu0 and cpu1 disagree"};
  EXPECT_EQ(describe(failure),
            "the machine describes itself inconsistently at /sys/x/topology: "
            "cpu0 and cpu1 disagree");
}

// --- the real machine (T8) ---------------------------------------------------

TEST(RealCpuTopologyTest, TheRunningMachineDescribesItselfConsistently) {
  // The cross-checks, run against a kernel nobody arranged. If they were too
  // strict -- if any of them were false of real hardware -- this fails, which
  // is the only way to find that out.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  auto topology = CpuTopology::discover(filesystem);
  ASSERT_TRUE(topology.has_value()) << describe(topology.error());

  // Cross-checked against sysconf, which reaches the number by its own route,
  // so the agreement is evidence rather than this code agreeing with itself.
  EXPECT_EQ(topology.value().logical_cpu_count(),
            static_cast<std::size_t>(::sysconf(_SC_NPROCESSORS_ONLN)));

  // Relationships that hold of every machine, checked without assuming the
  // shape of the one running the test.
  EXPECT_GE(topology.value().logical_cpu_count(), 1U);
  EXPECT_GE(topology.value().physical_core_count(), 1U);
  EXPECT_LE(topology.value().physical_core_count(), topology.value().logical_cpu_count());
  EXPECT_GE(topology.value().package_count(), 1U);
  EXPECT_LE(topology.value().package_count(), topology.value().physical_core_count());
  EXPECT_EQ(topology.value().multithreaded(),
            topology.value().physical_core_count() < topology.value().logical_cpu_count());
}

TEST(RealCpuTopologyTest, ASysfsRootThatDoesNotExistIsAbsentRatherThanACrash) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  auto topology = CpuTopology::discover(filesystem, "/nonexistent-sysfs-root");
  ASSERT_FALSE(topology.has_value());
  EXPECT_EQ(topology.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(topology.error().availability, Availability::kAbsent);
}

}  // namespace
}  // namespace loadforge::topology
