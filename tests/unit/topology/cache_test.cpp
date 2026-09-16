// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cache hierarchy, and the fact that makes testing it possible at all:
// sysfs publishes one cache once per CPU that can see it. Four CPUs sharing an
// L3 produce four descriptions of one piece of silicon, and reading all four is
// what turns "what the kernel said" into something checkable.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Trees are built on disk and driven through RealSyscalls, for the reason given
// at length in cpu_topology_test.cpp: a fake would agree with whatever this
// code believes, which is worthless for testing disagreement (F21).

#include "topology/cache.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/byte_size.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

// --- parse_cache_size --------------------------------------------------------

TEST(ParseCacheSizeTest, TheFormatTheKernelActuallyWrites) {
  // Every value probed from a real machine: L1d, L1i, L2, L3.
  EXPECT_EQ(parse_cache_size("48K").value_or(0), 48U * 1024U);
  EXPECT_EQ(parse_cache_size("32K").value_or(0), 32U * 1024U);
  EXPECT_EQ(parse_cache_size("2048K").value_or(0), 2048U * 1024U);
  EXPECT_EQ(parse_cache_size("266240K").value_or(0), 266240ULL * 1024ULL);
}

TEST(ParseCacheSizeTest, KMeansKibibytesAsItDoesEverywhereInTheKernel) {
  EXPECT_EQ(parse_cache_size("1K").value_or(0), 1024U) << "not 1000";
}

TEST(ParseCacheSizeTest, BytesWithAndWithoutTheSuffix) {
  // The kernel's own formatter switches to B below 1 KiB.
  EXPECT_EQ(parse_cache_size("512B").value_or(0), 512U);
  EXPECT_EQ(parse_cache_size("512").value_or(0), 512U);
}

TEST(ParseCacheSizeTest, TheConfigurationParserCannotBeUsedForThis) {
  // This is why topology has its own parser, asserted rather than asserted-in-a
  // -comment. If someone "unifies" the two, this fails and says why.
  //
  // core::parse_byte_size reads what a USER writes in a config file: KiB/MiB
  // and, deliberately, a percentage. It rejects the bare K sysfs writes -- so
  // pointing it at a cache size would fail on every real machine -- and
  // teaching it K would make "70%" a parseable cache size, which is not a thing.
  EXPECT_FALSE(core::parse_byte_size("48K").has_value())
      << "the config parser accepts sysfs's format now; re-read why these are separate";
  EXPECT_TRUE(core::parse_byte_size("70%").has_value())
      << "the config parser takes percentages, which a cache size must never";
  EXPECT_FALSE(parse_cache_size("70%").has_value());
  EXPECT_FALSE(parse_cache_size("48KiB").has_value());
}

TEST(ParseCacheSizeTest, ShapesTheKernelNeverWritesAreRefused) {
  for (const std::string_view text : {"", "K", "-1K", " 48K", "48 K", "48M", "0x30K", "48k"}) {
    EXPECT_FALSE(parse_cache_size(text).has_value()) << "accepted \"" << text << "\"";
  }
  // value_or on a FAILURE takes the fallback arm, which every value_or(0) on
  // a success above leaves untaken.
  EXPECT_EQ(parse_cache_size("48M").value_or(7), 7U);
}

TEST(ParseCacheSizeTest, ASizeLargerThanAnyCacheIsRefusedRatherThanWrapped) {
  // Bounded before the multiply. Journal §4.6: the same guard written the other
  // way round was undefined behaviour that a test missed.
  EXPECT_FALSE(parse_cache_size("99999999999999999999K").has_value());

  // The bound is 1 TiB, which in the K units sysfs writes is 1073741824K --
  // not 1048576K, which is 1 GiB. Getting that wrong here first is the reason
  // the assertion names the byte count rather than trusting the literal.
  EXPECT_EQ(parse_cache_size("1073741824K").value_or(0), 1ULL << 40U) << "exactly the bound";
  EXPECT_FALSE(parse_cache_size("1073741825K").has_value()) << "one KiB past it";
  EXPECT_EQ(parse_cache_size("1099511627776").value_or(0), 1ULL << 40U) << "the same, in bytes";
  EXPECT_FALSE(parse_cache_size("1099511627777").has_value());
}

TEST(CacheValueTest, EqualityComparesTheIdentityFirst) {
  // Cache::operator== is defaulted and compares members in order, stopping at
  // the first difference. discover() only ever compares two caches it has
  // already matched BY identity, so the identity arm is unreachable through it
  // -- and a public equality operator whose first comparison no test has taken
  // is one a later change could break unnoticed.
  const Cache l1{CacheId{1, CacheType::kData, 0}, 48U * 1024U, 64, CpuList{}};
  const Cache l2{CacheId{2, CacheType::kData, 0}, 48U * 1024U, 64, CpuList{}};
  EXPECT_NE(l1, l2) << "same size, line and sharing; different level";
  EXPECT_EQ(l1, l1);
}

TEST(CacheTypeTest, EveryTypeRendersAsTheTextSysfsWrites) {
  EXPECT_EQ(describe(CacheType::kData), "Data");
  EXPECT_EQ(describe(CacheType::kInstruction), "Instruction");
  EXPECT_EQ(describe(CacheType::kUnified), "Unified");
  EXPECT_EQ(describe(static_cast<CacheType>(99)), "unrecognised cache type");
}

// --- discovery ---------------------------------------------------------------

class CacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("loadforge-cache-" + std::to_string(::getpid()) + "-" +
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

  std::filesystem::path cpu_dir(std::uint32_t id) {
    return root / "devices" / "system" / "cpu" / ("cpu" + std::to_string(id));
  }

  void set_online(const std::string& list) {
    write(root / "devices" / "system" / "cpu" / "online", list);
  }

  void add_cpu(std::uint32_t id, const std::string& package, const std::string& core,
               const std::string& siblings) {
    write(cpu_dir(id) / "topology" / "physical_package_id", package);
    write(cpu_dir(id) / "topology" / "core_id", core);
    write(cpu_dir(id) / "topology" / "thread_siblings_list", siblings);
  }

  /// One cache/indexN directory, exactly as the kernel lays it out.
  void add_cache(std::uint32_t cpu, std::uint32_t index, const std::string& level,
                 const std::string& type, const std::string& id, const std::string& size,
                 const std::string& shared, const std::string& line = "64") {
    const std::filesystem::path d = cpu_dir(cpu) / "cache" / ("index" + std::to_string(index));
    write(d / "level", level);
    write(d / "type", type);
    write(d / "id", id);
    write(d / "size", size);
    write(d / "shared_cpu_list", shared);
    write(d / "coherency_line_size", line);
  }

  /// The arrangement probed from a real 4-CPU machine: private L1d, L1i and L2
  /// per CPU, one L3 shared by all four.
  void build_typical_machine() {
    set_online("0-3");
    for (std::uint32_t id = 0; id < 4; ++id) {
      add_cpu(id, "0", std::to_string(id), std::to_string(id));
      const std::string self = std::to_string(id);
      add_cache(id, 0, "1", "Data", self, "48K", self);
      add_cache(id, 1, "1", "Instruction", self, "32K", self);
      add_cache(id, 2, "2", "Unified", self, "2048K", self);
      add_cache(id, 3, "3", "Unified", "0", "266240K", "0-3");
    }
  }

  core::Result<CacheHierarchy, DiscoveryFailure> discover() {
    auto cpus = CpuTopology::discover(filesystem, root.string());
    EXPECT_TRUE(cpus.has_value()) << "the CPU half must be sound before caches mean anything";
    return CacheHierarchy::discover(filesystem, cpus.value(), root.string());
  }

  std::filesystem::path root;
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
};

TEST_F(CacheTest, OneSharedL3IsCountedOnceNotFourTimes) {
  // The whole reason deduplication exists. Four CPUs each publish the L3; it is
  // one cache, and a tool reporting 1 GiB of L3 on a 260 MiB machine would be
  // wrong in the direction that flatters the hardware.
  build_typical_machine();

  auto caches = discover();
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());
  EXPECT_EQ(caches.value().count_at_level(1), 8U) << "four CPUs x (data + instruction)";
  EXPECT_EQ(caches.value().count_at_level(2), 4U);
  EXPECT_EQ(caches.value().count_at_level(3), 1U) << "one L3, not four";
  EXPECT_EQ(caches.value().deepest_level(), 3U);
  EXPECT_EQ(caches.value().total_bytes_at_level(3), 266240ULL * 1024ULL);
  EXPECT_EQ(caches.value().total_bytes_at_level(1), 4ULL * (48U + 32U) * 1024U);
}

TEST_F(CacheTest, DataAndInstructionCachesAtOneLevelShareAnIdAndAreStillTwoCaches) {
  // The case CacheId's `type` member exists for. Both are L1 id=0 on cpu0.
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "0", "48K", "0");
  add_cache(0, 1, "1", "Instruction", "0", "32K", "0");

  auto caches = discover();
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());
  EXPECT_EQ(caches.value().count_at_level(1), 2U);
  EXPECT_EQ(caches.value().total_bytes_at_level(1), (48U + 32U) * 1024U);
}

TEST_F(CacheTest, AMachineWithNoL3IsAMachineWithNoL3) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "0", "48K", "0");
  add_cache(0, 1, "2", "Unified", "0", "2048K", "0");

  auto caches = discover();
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());
  EXPECT_EQ(caches.value().deepest_level(), 2U);
  EXPECT_EQ(caches.value().count_at_level(3), 0U) << "absent, not an error";
  EXPECT_EQ(caches.value().total_bytes_at_level(3), 0U);
}

TEST_F(CacheTest, AMachineWithNoCacheDirectoriesAtAllIsEmptyRatherThanAFailure) {
  // Some virtualised machines publish no cache information. That is a fact
  // about the machine, not a failure to read it.
  set_online("0");
  add_cpu(0, "0", "0", "0");

  auto caches = discover();
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());
  EXPECT_EQ(caches.value().count(), 0U);
  EXPECT_EQ(caches.value().deepest_level(), 0U);
}

TEST_F(CacheTest, CachesAreOrderedByLevelThenId) {
  build_typical_machine();
  auto caches = discover();
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());

  std::uint32_t previous = 0;
  for (const Cache& cache : caches.value().caches()) {
    EXPECT_GE(cache.identity.level, previous) << "levels must not go backwards";
    previous = cache.identity.level;
  }
  ASSERT_FALSE(caches.value().caches().empty());
  EXPECT_EQ(caches.value().caches().back().identity.level, 3U);
}

// --- a source that will not answer -------------------------------------------

TEST_F(CacheTest, AnUnreadableIndexIsNotMistakenForTheEndOfTheList) {
  // The subtle one. The scan stops at the first ABSENT index; if it stopped at
  // the first index it could not read, a machine whose L3 we were denied would
  // silently report as a machine with no L3 -- a plausible wrong answer, which
  // is exactly what this module exists to avoid.
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "0", "48K", "0");
  // index1/level is a DIRECTORY, so opening succeeds and reading fails EISDIR.
  std::filesystem::create_directories(cpu_dir(0) / "cache" / "index1" / "level");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(caches.error().availability, Availability::kUnreadable);
}

TEST_F(CacheTest, EveryOtherMissingAttributeIsASourceFailureToo) {
  // One test per attribute rather than one representative, because each is a
  // separate read with its own failure arm, and a representative would leave
  // the others as arms no test has taken.
  for (const char* missing : {"type", "id", "coherency_line_size", "shared_cpu_list"}) {
    TearDown();
    SetUp();
    set_online("0");
    add_cpu(0, "0", "0", "0");
    add_cache(0, 0, "1", "Data", "0", "48K", "0");
    ASSERT_TRUE(std::filesystem::remove(cpu_dir(0) / "cache" / "index0" / missing));

    auto caches = discover();
    ASSERT_FALSE(caches.has_value()) << "succeeded without " << missing;
    EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kSource) << missing;
    EXPECT_EQ(caches.error().availability, Availability::kAbsent) << missing;
  }
}

TEST_F(CacheTest, AnIdOfMinusOneIsRefused) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "-1", "48K", "0");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("negative"), std::string::npos);
}

TEST_F(CacheTest, AnIdPastAnythingACacheAttributeHoldsIsRefused) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "99999999", "48K", "0");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("past anything"), std::string::npos);
}

TEST_F(CacheTest, ALevelDeeperThanAnyCacheHierarchyIsRefused) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "9", "Unified", "0", "48K", "0");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("not a cache level"), std::string::npos);
}

TEST_F(CacheTest, ASizeThatIsNotASizeIsRefusedWithTheParsersReason) {
  // Distinct from zero: zero parses and is then refused as a fact; this does
  // not parse at all, and the report must carry the parser's own wording.
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "0", "48M", "0");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("ends in K, B or nothing"), std::string::npos);
}

TEST_F(CacheTest, TheScanStopsAtTheIndexBoundRatherThanWalkingForever) {
  // Sixteen valid entries, so the loop exits by exhausting kMaxCacheIndex
  // rather than by finding an absent one. Nothing real has this many; the
  // bound exists so a tree that is not what we think cannot become a walk.
  set_online("0");
  add_cpu(0, "0", "0", "0");
  for (std::uint32_t index = 0; index < CacheHierarchy::kMaxCacheIndex; ++index) {
    add_cache(0, index, "1", "Data", std::to_string(index), "1K", "0");
  }
  // A seventeenth exists on disk and must NOT be read.
  add_cache(0, CacheHierarchy::kMaxCacheIndex, "1", "Data", "999", "1K", "0");

  auto caches = discover();
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());
  EXPECT_EQ(caches.value().count(), CacheHierarchy::kMaxCacheIndex);
}

TEST_F(CacheTest, TwoReadingsOfOneCacheMustAgreeOnEveryField) {
  // Cache::operator== is defaulted, which compares each member in turn and
  // stops at the first difference. The "described differently" test above
  // differs in SIZE, the second member; these differ in the third and fourth,
  // so every comparison arm is one a test has actually taken.
  set_online("0-1");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "0", "1", "1");

  add_cache(0, 0, "3", "Unified", "0", "8192K", "0-1", "64");
  add_cache(1, 0, "3", "Unified", "0", "8192K", "0-1", "128");  // line size differs
  auto by_line = discover();
  ASSERT_FALSE(by_line.has_value());
  EXPECT_NE(describe(by_line.error()).find("described differently"), std::string::npos);

  TearDown();
  SetUp();
  set_online("0-2");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "0", "1", "1");
  add_cpu(2, "0", "2", "2");
  add_cache(0, 0, "3", "Unified", "0", "8192K", "0-2");
  add_cache(1, 0, "3", "Unified", "0", "8192K", "0-2");
  add_cache(2, 0, "3", "Unified", "0", "8192K", "0-1");  // cpu2 shared set differs
  auto by_shared = discover();
  ASSERT_FALSE(by_shared.has_value());
  // cpu2's L3 does not list cpu2 itself, so the self-containment check fires
  // first -- which is the right order, and the one the test pins.
  EXPECT_NE(describe(by_shared.error()).find("sharing its own cache"), std::string::npos);

  TearDown();
  SetUp();
  set_online("0-2");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "0", "1", "1");
  add_cpu(2, "0", "2", "2");
  add_cache(0, 0, "3", "Unified", "0", "8192K", "0-2");
  add_cache(1, 0, "3", "Unified", "0", "8192K", "1-2");  // cpu1 omits cpu0: differs, self-contained
  add_cache(2, 0, "3", "Unified", "0", "8192K", "0-2");
  auto by_set = discover();
  ASSERT_FALSE(by_set.has_value());
  EXPECT_NE(describe(by_set.error()).find("described differently"), std::string::npos);
}

TEST_F(CacheTest, AMissingSizeIsASourceFailure) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  const std::filesystem::path d = cpu_dir(0) / "cache" / "index0";
  write(d / "level", "1");
  write(d / "type", "Data");
  write(d / "id", "0");
  write(d / "shared_cpu_list", "0");
  write(d / "coherency_line_size", "64");
  // size deliberately not written.

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kSource);
  EXPECT_EQ(caches.error().availability, Availability::kAbsent);
}

// --- the machine contradicting itself ----------------------------------------

TEST_F(CacheTest, ATypeTheKernelNeverWritesIsRefusedRatherThanGuessed) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "data", "0", "48K", "0");  // lowercase: not what sysfs writes

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("Data, Instruction or Unified"), std::string::npos);
}

TEST_F(CacheTest, ALevelThatIsNotALevelIsRefused) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "0", "Data", "0", "48K", "0");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("not a cache level"), std::string::npos);
}

TEST_F(CacheTest, ACacheOfZeroBytesIsRefused) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "1", "Data", "0", "0K", "0");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("zero bytes"), std::string::npos);
}

TEST_F(CacheTest, ACpuMissingFromItsOwnCachesSharedListIsRefused) {
  set_online("0-1");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "0", "1", "1");
  add_cache(0, 0, "1", "Data", "0", "48K", "1");  // cpu0's cache says it serves cpu1 only

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("not among the CPUs sharing its own cache"),
            std::string::npos);
}

TEST_F(CacheTest, ACacheServingACpuThatIsNotOnlineIsRefused) {
  set_online("0");
  add_cpu(0, "0", "0", "0");
  add_cache(0, 0, "3", "Unified", "0", "8192K", "0-1");  // cpu1 is not online

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("not online"), std::string::npos);
}

TEST_F(CacheTest, TwoCpusDescribingOneCacheDifferentlyAreRefused) {
  // Sharing a cache is an equivalence relation, exactly as sharing a core is.
  // Two readings of one identity must be IDENTICAL, not merely compatible --
  // otherwise deduplication would keep whichever it saw first, and which one
  // that is would depend on iteration order.
  set_online("0-1");
  add_cpu(0, "0", "0", "0");
  add_cpu(1, "0", "1", "1");
  add_cache(0, 0, "3", "Unified", "0", "8192K", "0-1");
  add_cache(1, 0, "3", "Unified", "0", "4096K", "0-1");  // same cache, different size

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("described differently"), std::string::npos);
}

TEST_F(CacheTest, ACacheThatDoesNotNestInsideTheNextLevelIsRefused) {
  // The defining structural property of a hierarchy: the CPUs that see my L1
  // are among those that see my L2. Here cpu0's L1 is shared with cpu1 while
  // its L2 is private, which describes no arrangement of silicon.
  set_online("0-1");
  add_cpu(0, "0", "0", "0-1");
  add_cpu(1, "0", "0", "0-1");
  add_cache(0, 0, "1", "Data", "0", "48K", "0-1");
  add_cache(0, 1, "2", "Unified", "0", "2048K", "0");
  add_cache(1, 0, "1", "Data", "0", "48K", "0-1");
  add_cache(1, 1, "2", "Unified", "1", "2048K", "1");

  auto caches = discover();
  ASSERT_FALSE(caches.has_value());
  EXPECT_EQ(caches.error().kind, DiscoveryFailure::Kind::kContradiction);
  EXPECT_NE(describe(caches.error()).find("caches nest"), std::string::npos);
}

// --- the real machine (T8) ---------------------------------------------------

TEST(RealCacheTest, TheRunningMachinesCachesDescribeThemselvesConsistently) {
  // The cross-checks against a kernel nobody arranged. If any of them were too
  // strict -- false of real hardware -- this is the test that says so, and it
  // is the reason they are conservative: the nesting check is here, but "every
  // CPU has the same levels" and "sizes grow with level" deliberately are not.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};

  auto cpus = CpuTopology::discover(filesystem);
  ASSERT_TRUE(cpus.has_value()) << describe(cpus.error());

  auto caches = CacheHierarchy::discover(filesystem, cpus.value());
  ASSERT_TRUE(caches.has_value()) << describe(caches.error());

  // Relationships that hold of any machine, asserted without assuming the shape
  // of the one running the test. A machine publishing no cache information at
  // all is legitimate, so everything below is conditional on there being some.
  for (const Cache& cache : caches.value().caches()) {
    EXPECT_GE(cache.identity.level, 1U);
    EXPECT_GT(cache.size_bytes, 0U);
    EXPECT_FALSE(cache.shared_with.empty());
    EXPECT_LE(cache.shared_with.size(), cpus.value().logical_cpu_count())
        << "a cache cannot serve more CPUs than the machine has";
    if (cache.line_bytes != 0) {
      EXPECT_GE(cache.line_bytes, 8U) << "a cache line smaller than a word is not a cache line";
    }
  }

  if (caches.value().count() > 0) {
    EXPECT_GE(caches.value().deepest_level(), 1U);
    EXPECT_GT(caches.value().total_bytes_at_level(1), 0U) << "a machine with caches has an L1";
  }
}

}  // namespace
}  // namespace loadforge::topology
