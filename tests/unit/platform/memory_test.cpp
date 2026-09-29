// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mapping memory the way a workload asked for it, and saying exactly which
// step the kernel refused.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The hostile state this module's capability paths need is a huge-page pool:
// reserved and sufficient, reserved but too small, or not reserved at all.
// That is a kernel setting -- /proc/sys/vm/nr_hugepages -- which a test suite
// must not change on a host it does not own, and which CI cannot commit. So it
// is BUILT, in FakeSyscalls, which models the pool the way the kernel accounts
// for it: consumed in whole pages by every MAP_HUGETLB mapping, refilled on
// unmap, and refused with ENOMEM when the request is larger than what is left.
// The real kernel's answer for whatever pool the machine happens to have is
// asserted in real_syscalls_test.cpp, against each of the three states it can
// be in.

#include "platform/memory.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::platform {
namespace {

using loadforge::testing::FakeSyscalls;

constexpr std::size_t kOneMiB = std::size_t{1} << 20;

/// A request built field by field, because an aggregate with a defaulted
/// optional trips -Wmissing-field-initializers however it is spelled.
MappingRequest request(std::size_t bytes, HugePages huge_pages = HugePages::kNone,
                       bool populate = false, bool lock = false,
                       std::optional<std::uint32_t> node = std::nullopt) {
  MappingRequest built;
  built.bytes = bytes;
  built.huge_pages = huge_pages;
  built.populate = populate;
  built.lock = lock;
  built.node = node;
  return built;
}

NodeMask nodes(std::initializer_list<std::uint32_t> ids) {
  NodeMask mask;
  for (const std::uint32_t id : ids) {
    mask.nodes.set(id);
  }
  return mask;
}

// --- pure functions ----------------------------------------------------------

TEST(MemoryWordsTest, EveryEnumerationHasItsOwnWordsAndTheTablesAreTotal) {
  EXPECT_EQ(describe(HugePages::kNone), "ordinary pages");
  EXPECT_EQ(describe(HugePages::kTransparent), "transparent huge pages");
  EXPECT_EQ(describe(HugePages::kExplicit), "explicit huge pages");
  EXPECT_EQ(describe(static_cast<HugePages>(99)), "unrecognised huge-page strategy");

  EXPECT_EQ(describe(Stage::kMap), "mapping");
  EXPECT_EQ(describe(Stage::kBind), "binding to the NUMA node");
  EXPECT_EQ(describe(Stage::kAdvise), "advising transparent huge pages");
  EXPECT_EQ(describe(Stage::kPopulate), "populating");
  EXPECT_EQ(describe(Stage::kLock), "locking");
  EXPECT_EQ(describe(static_cast<Stage>(99)), "unrecognised stage");

  EXPECT_EQ(describe(Refusal::kBadRequest), "the request is not one the kernel accepts");
  EXPECT_EQ(describe(Refusal::kExhausted),
            "the memory, huge pages or lock allowance are exhausted");
  EXPECT_EQ(describe(Refusal::kNotPermitted), "not permitted");
  EXPECT_EQ(describe(Refusal::kUnsupported), "this kernel does not support it");
  EXPECT_EQ(describe(Refusal::kUnexpected),
            "the kernel refused for a reason this code does not expect");
  EXPECT_EQ(describe(static_cast<Refusal>(99)), "unrecognised refusal");
}

TEST(MemoryWordsTest, ARequestRendersEveryChoiceItCarries) {
  EXPECT_EQ(describe(request(kOneMiB)), "1048576 bytes, ordinary pages");
  EXPECT_EQ(describe(request(4096, HugePages::kTransparent, true, true, 3)),
            "4096 bytes, transparent huge pages, populated, locked, on node 3");
  EXPECT_EQ(describe(request(4096, HugePages::kExplicit, false, true, std::nullopt)),
            "4096 bytes, explicit huge pages, locked");
}

TEST(MemoryWordsTest, ClassifiesEachDocumentedErrno) {
  auto err = [](int number) { return SyscallError{number, "mmap", "4096 bytes"}; };
  EXPECT_EQ(classify_refusal(err(EINVAL)), Refusal::kBadRequest);
  EXPECT_EQ(classify_refusal(err(ENOMEM)), Refusal::kExhausted);
  EXPECT_EQ(classify_refusal(err(EAGAIN)), Refusal::kExhausted);
  EXPECT_EQ(classify_refusal(err(EPERM)), Refusal::kNotPermitted);
  EXPECT_EQ(classify_refusal(err(EACCES)), Refusal::kNotPermitted);
  EXPECT_EQ(classify_refusal(err(ENOSYS)), Refusal::kUnsupported);
  EXPECT_EQ(classify_refusal(err(EOPNOTSUPP)), Refusal::kUnsupported);
  EXPECT_EQ(classify_refusal(err(EFAULT)), Refusal::kUnexpected);
  EXPECT_EQ(classify_refusal(err(EIO)), Refusal::kUnexpected);
}

TEST(MemoryWordsTest, RoundsUpToWholeHugePagesAndRefusesToWrap) {
  EXPECT_EQ(round_up_to_huge_pages(0), 0U);
  EXPECT_EQ(round_up_to_huge_pages(1), kExplicitHugePageBytes);
  EXPECT_EQ(round_up_to_huge_pages(kExplicitHugePageBytes), kExplicitHugePageBytes);
  EXPECT_EQ(round_up_to_huge_pages(kExplicitHugePageBytes + 1), 2 * kExplicitHugePageBytes);

  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  constexpr std::size_t kLargest = kMax - (kExplicitHugePageBytes - 1);
  EXPECT_EQ(round_up_to_huge_pages(kLargest), kLargest) << "the last size that still rounds";
  EXPECT_FALSE(round_up_to_huge_pages(kLargest + 1).has_value()) << "one past it would wrap";
  EXPECT_FALSE(round_up_to_huge_pages(kMax).has_value());
}

TEST(MemoryWordsTest, RequestEqualityIsFieldByField) {
  // A refusal carries the request back to its caller; the comparison the
  // tests rely on has to notice a change in any one field.
  const MappingRequest base = request(4096, HugePages::kNone, false, false, std::nullopt);
  EXPECT_EQ(base, request(4096, HugePages::kNone, false, false, std::nullopt));
  EXPECT_NE(base, request(8192, HugePages::kNone, false, false, std::nullopt));
  EXPECT_NE(base, request(4096, HugePages::kExplicit, false, false, std::nullopt));
  EXPECT_NE(base, request(4096, HugePages::kNone, true, false, std::nullopt));
  EXPECT_NE(base, request(4096, HugePages::kNone, false, true, std::nullopt));
  EXPECT_NE(base, request(4096, HugePages::kNone, false, false, 0));
}

TEST(MemoryWordsTest, ANodeMaskNamesOneNodeAndRefusesOnePastTheMask) {
  EXPECT_EQ(node_mask_of(0).value(), nodes({0}));
  EXPECT_EQ(node_mask_of(1023).value(), nodes({1023})) << "the last representable id";
  EXPECT_NE(nodes({0}), nodes({1})) << "equality is bitwise";
  auto past = node_mask_of(1024);
  ASSERT_FALSE(past.has_value());
  EXPECT_NE(std::string{past.error()}.find("past what any Linux kernel"), std::string::npos);
}

TEST(MemoryWordsTest, ARefusalRendersStageKindRequestAndCause) {
  const MappingRefused refused{Stage::kLock, Refusal::kExhausted, request(kOneMiB),
                               SyscallError{ENOMEM, "mlock", "1048576 bytes"}};
  EXPECT_EQ(
      describe(refused),
      "cannot map 1048576 bytes, ordinary pages: locking failed, the memory, huge pages or "
      "lock allowance are exhausted (mlock(1048576 bytes): Cannot allocate memory (errno 12))");
}

// --- Mapping -------------------------------------------------------------------

class MappingTest : public ::testing::Test {
 protected:
  FakeSyscalls syscalls;
  Memory memory{syscalls};
};

TEST_F(MappingTest, AMappingIsWritableAndUnmapsItselfWhenItGoesOutOfScope) {
  {
    auto mapped = memory.map(request(kOneMiB));
    ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
    const Mapping mapping = std::move(mapped).value();
    ASSERT_NE(mapping.address(), nullptr);
    EXPECT_EQ(mapping.bytes(), kOneMiB);
    std::memset(mapping.address(), 0x5a, mapping.bytes());
    EXPECT_EQ(syscalls.live_mappings(), 1U);
    EXPECT_EQ(syscalls.map_flags(), (std::vector<int>{0})) << "ordinary pages ask for nothing";
  }
  EXPECT_EQ(syscalls.live_mappings(), 0U) << "released by the destructor";
  EXPECT_EQ(syscalls.unmap_count(), 1);
}

TEST_F(MappingTest, ReleaseReportsTheUnmapAndDisarmsTheDestructor) {
  syscalls.fail_unmap(EINVAL);
  auto mapped = memory.map(request(kOneMiB));
  ASSERT_TRUE(mapped.has_value());
  Mapping mapping = std::move(mapped).value();

  auto released = mapping.release();
  ASSERT_FALSE(released.has_value()) << "the caller who asked is told";
  EXPECT_EQ(released.error().number, EINVAL);
  EXPECT_EQ(released.error().call, "munmap");
  EXPECT_EQ(mapping.address(), nullptr);
  EXPECT_EQ(mapping.bytes(), 0U);

  EXPECT_TRUE(mapping.release().has_value()) << "nothing left to release is not a failure";
  EXPECT_EQ(syscalls.unmap_count(), 1) << "one attempt: release, and not the destructor again";
}

TEST_F(MappingTest, ReleaseSucceedsWhenTheUnmapDoes) {
  auto mapped = memory.map(request(kOneMiB));
  ASSERT_TRUE(mapped.has_value());
  Mapping mapping = std::move(mapped).value();
  EXPECT_TRUE(mapping.release().has_value());
  EXPECT_EQ(syscalls.live_mappings(), 0U);
}

TEST_F(MappingTest, MovingTransfersOwnershipSoOnlyOneUnmapHappens) {
  auto first = memory.map(request(kOneMiB));
  ASSERT_TRUE(first.has_value());
  Mapping a = std::move(first).value();
  void* const address = a.address();

  Mapping b(std::move(a));
  EXPECT_EQ(a.address(),
            nullptr);  // NOLINT(bugprone-use-after-move): the moved-from state is the assertion
  EXPECT_EQ(b.address(), address);

  auto second = memory.map(request(4096));
  ASSERT_TRUE(second.has_value());
  Mapping c = std::move(second).value();
  EXPECT_EQ(syscalls.live_mappings(), 2U);

  c = std::move(b);
  EXPECT_EQ(syscalls.live_mappings(), 1U) << "assignment unmapped what c held";
  EXPECT_EQ(c.address(), address);
  EXPECT_EQ(c.bytes(), kOneMiB);

  Mapping* const self = &c;  // Through a pointer, so the self-move is the test and not a warning.
  c = std::move(*self);
  EXPECT_EQ(c.address(), address);
  EXPECT_EQ(syscalls.live_mappings(), 1U);

  Mapping d(std::move(c));
  d = std::move(a);  // Assigning a moved-from mapping unmaps d's and holds nothing.
  EXPECT_EQ(syscalls.live_mappings(), 0U);
  EXPECT_EQ(d.address(), nullptr);

  auto again = memory.map(request(4096));
  ASSERT_TRUE(again.has_value());
  d = std::move(again).value();  // Into an empty mapping: nothing to unmap first.
  EXPECT_EQ(syscalls.live_mappings(), 1U);
  EXPECT_EQ(d.bytes(), 4096U);
}

// --- Memory against the fake (T2) --------------------------------------------

TEST_F(MappingTest, AZeroByteRequestIsSentToTheKernelAndRefusedByIt) {
  // Deliberately not pre-checked above the seam (journal §2.12), and the fake
  // is no laxer than the kernel about it.
  auto mapped = memory.map(request(0));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kMap);
  EXPECT_EQ(mapped.error().kind, Refusal::kBadRequest);
  EXPECT_EQ(mapped.error().cause.number, EINVAL);
  EXPECT_EQ(syscalls.map_flags().size(), 1U) << "the call was made";
}

TEST_F(MappingTest, AMapFailureIsClassifiedAndCarriedThroughFieldByField) {
  syscalls.fail_map(ENOMEM);
  const MappingRequest wanted = request(kOneMiB, HugePages::kNone, true, true, 0);
  auto mapped = memory.map(wanted);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kMap);
  EXPECT_EQ(mapped.error().kind, Refusal::kExhausted);
  EXPECT_EQ(mapped.error().request, wanted);
  EXPECT_EQ(mapped.error().cause.number, ENOMEM);
  EXPECT_EQ(mapped.error().cause.call, "mmap");
  EXPECT_EQ(mapped.error().cause.subject, "1048576 bytes");
  EXPECT_TRUE(syscalls.binds().empty()) << "nothing after a failed map";
  EXPECT_TRUE(syscalls.advice_given().empty());
  EXPECT_EQ(syscalls.locked_bytes(), 0U);
}

TEST_F(MappingTest, ExplicitHugePagesComeFromThePoolInWholePagesAndGoBackOnUnmap) {
  syscalls.set_huge_pages_free(2);
  const MappingRequest one_byte = request(1, HugePages::kExplicit);
  auto first = memory.map(one_byte);
  ASSERT_TRUE(first.has_value()) << describe(first.error());
  EXPECT_EQ(first.value().bytes(), kExplicitHugePageBytes)
      << "the mapping remembers the rounded length, which is what munmap needs";
  EXPECT_EQ(syscalls.huge_pages_free(), 1U) << "one byte took a whole page";
  EXPECT_EQ(syscalls.map_flags().back() & MAP_HUGETLB, MAP_HUGETLB);
  EXPECT_EQ(syscalls.map_flags().back() >> MAP_HUGE_SHIFT, 21) << "2 MiB, named explicitly";

  auto second = memory.map(request(kExplicitHugePageBytes + 1, HugePages::kExplicit));
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().stage, Stage::kMap);
  EXPECT_EQ(second.error().kind, Refusal::kExhausted)
      << "present but too small: one page left, two needed";
  EXPECT_EQ(second.error().cause.number, ENOMEM);

  auto third = memory.map(one_byte);
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(syscalls.huge_pages_free(), 0U);

  auto fourth = memory.map(one_byte);
  ASSERT_FALSE(fourth.has_value()) << "the pool is empty";
  EXPECT_EQ(fourth.error().kind, Refusal::kExhausted);

  ASSERT_TRUE(std::move(first).value().release().has_value());
  EXPECT_EQ(syscalls.huge_pages_free(), 1U) << "unmapping returns the page";
  EXPECT_TRUE(memory.map(one_byte).has_value()) << "and it can be taken again";
}

TEST_F(MappingTest, AnExplicitRequestThatCannotBeRoundedIsRefusedWithoutACall) {
  const MappingRequest wrapping =
      request(std::numeric_limits<std::size_t>::max(), HugePages::kExplicit);
  auto mapped = memory.map(wrapping);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kMap);
  EXPECT_EQ(mapped.error().kind, Refusal::kBadRequest);
  EXPECT_EQ(mapped.error().cause.call, "huge-page rounding") << "names the check, not a syscall";
  EXPECT_NE(mapped.error().cause.subject.find("rounds past the largest size"), std::string::npos);
  EXPECT_TRUE(syscalls.map_flags().empty()) << "no call was made";
}

TEST_F(MappingTest, TransparentHugePagesAreAdvisedBeforeAnythingTouchesTheRange) {
  auto mapped = memory.map(request(kOneMiB, HugePages::kTransparent, true));
  ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
  EXPECT_EQ(syscalls.advice_given(), (std::vector<int>{MADV_HUGEPAGE, MADV_POPULATE_WRITE}))
      << "advice first, then the populate that faults the pages";
  EXPECT_EQ(syscalls.map_flags(), (std::vector<int>{0})) << "not MAP_HUGETLB";
}

TEST_F(MappingTest, AFailedAdviceUnmapsAndNamesTheAdviseStage) {
  syscalls.fail_advice(MADV_HUGEPAGE, EINVAL);
  auto mapped = memory.map(request(kOneMiB, HugePages::kTransparent, true, true));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kAdvise);
  EXPECT_EQ(mapped.error().kind, Refusal::kBadRequest);
  EXPECT_EQ(mapped.error().cause.call, "madvise");
  EXPECT_EQ(syscalls.live_mappings(), 0U) << "the partial result is not kept";
  EXPECT_EQ(syscalls.advice_given(), (std::vector<int>{MADV_HUGEPAGE})) << "and nothing followed";
  EXPECT_EQ(syscalls.locked_bytes(), 0U);
}

TEST_F(MappingTest, AFailedPopulateIsThePopulateStageNotTheAdviseStage) {
  // The same syscall serves both steps; the report must not blur them.
  syscalls.fail_advice(MADV_POPULATE_WRITE, ENOMEM);
  auto mapped = memory.map(request(kOneMiB, HugePages::kTransparent, true, true));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kPopulate);
  EXPECT_EQ(mapped.error().kind, Refusal::kExhausted);
  EXPECT_EQ(syscalls.advice_given(), (std::vector<int>{MADV_HUGEPAGE, MADV_POPULATE_WRITE}));
  EXPECT_EQ(syscalls.live_mappings(), 0U);
  EXPECT_EQ(syscalls.locked_bytes(), 0U) << "the lock step was never reached";
}

TEST_F(MappingTest, LockingPinsTheWholeMappingAfterPopulating) {
  auto mapped = memory.map(request(kOneMiB, HugePages::kNone, true, true));
  ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
  EXPECT_EQ(syscalls.locked_bytes(), kOneMiB);
  EXPECT_EQ(syscalls.advice_given(), (std::vector<int>{MADV_POPULATE_WRITE}));
  ASSERT_TRUE(std::move(mapped).value().release().has_value());
  EXPECT_EQ(syscalls.locked_bytes(), 0U) << "unmapping unlocks";
}

TEST_F(MappingTest, ALockPastTheAllowanceIsExhaustedAndALockWithNoAllowanceIsNotPermitted) {
  // RLIMIT_MEMLOCK as an unprivileged process meets it, both ways the kernel
  // says no. Corroborated in real_syscalls_test.cpp from a child that drops
  // privileges.
  syscalls.set_lock_limit(4096);
  auto too_much = memory.map(request(kOneMiB, HugePages::kNone, false, true));
  ASSERT_FALSE(too_much.has_value());
  EXPECT_EQ(too_much.error().stage, Stage::kLock);
  EXPECT_EQ(too_much.error().kind, Refusal::kExhausted);
  EXPECT_EQ(too_much.error().cause.number, ENOMEM);
  EXPECT_EQ(syscalls.live_mappings(), 0U);

  syscalls.set_lock_limit(0);
  auto none = memory.map(request(4096, HugePages::kNone, false, true));
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().stage, Stage::kLock);
  EXPECT_EQ(none.error().kind, Refusal::kNotPermitted);
  EXPECT_EQ(none.error().cause.number, EPERM);
  EXPECT_EQ(syscalls.live_mappings(), 0U);
}

TEST_F(MappingTest, AnUnexpectedLockErrnoIsCarriedThroughVerbatim) {
  syscalls.fail_lock(EFAULT);
  auto mapped = memory.map(request(4096, HugePages::kNone, false, true));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kLock);
  EXPECT_EQ(mapped.error().kind, Refusal::kUnexpected);
  EXPECT_EQ(mapped.error().cause.number, EFAULT);
  EXPECT_NE(describe(mapped.error()).find("does not expect"), std::string::npos);
}

TEST_F(MappingTest, BindingHappensBeforeAdviceOrPopulateAndNamesTheNode) {
  syscalls.permit_nodes(nodes({0, 1}));
  auto mapped = memory.map(request(kOneMiB, HugePages::kTransparent, true, false, 1));
  ASSERT_TRUE(mapped.has_value()) << describe(mapped.error());
  ASSERT_EQ(syscalls.binds().size(), 1U);
  EXPECT_EQ(syscalls.binds().front(), nodes({1}));
  EXPECT_EQ(syscalls.advice_given().size(), 2U) << "the bind preceded both";
}

TEST_F(MappingTest, ANodeWithoutMemoryIsRefusedByTheKernelAtTheBindStage) {
  auto mapped = memory.map(request(kOneMiB, HugePages::kNone, false, false, 7));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kBind);
  EXPECT_EQ(mapped.error().kind, Refusal::kBadRequest);
  EXPECT_EQ(mapped.error().cause.call, "mbind");
  EXPECT_EQ(mapped.error().cause.number, EINVAL);
  EXPECT_EQ(syscalls.live_mappings(), 0U);
  EXPECT_TRUE(syscalls.advice_given().empty()) << "nothing after a failed bind";
}

TEST_F(MappingTest, ANodePastTheMaskIsRefusedWithoutAskingTheKernel) {
  auto mapped = memory.map(request(kOneMiB, HugePages::kNone, false, false, 1024));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kBind);
  EXPECT_EQ(mapped.error().kind, Refusal::kBadRequest);
  EXPECT_EQ(mapped.error().cause.call, "node mask") << "names the check, not a syscall";
  EXPECT_EQ(mapped.error().cause.subject,
            "node 1024 is a node id past what any Linux kernel can number");
  EXPECT_TRUE(syscalls.binds().empty()) << "mbind was not called";
  EXPECT_EQ(syscalls.live_mappings(), 0U) << "the mapping made first was released";
}

TEST_F(MappingTest, AKernelWithoutNumaIsReportedAsUnsupported) {
  syscalls.fail_bind(ENOSYS);
  auto mapped = memory.map(request(kOneMiB, HugePages::kNone, false, false, 0));
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().stage, Stage::kBind);
  EXPECT_EQ(mapped.error().kind, Refusal::kUnsupported);
  EXPECT_NE(describe(mapped.error()).find("does not support"), std::string::npos);
}

TEST_F(MappingTest, TheFakeIsNoLaxerThanTheKernelAboutRangesItNeverMapped) {
  // The seam's contract, asserted against the model that tests rely on:
  // mlock and madvise on an unmapped range are ENOMEM, mbind is EFAULT, and
  // an unmap of an unknown address is refused (stricter than Linux, on
  // purpose). Each verified against the real kernel in real_syscalls_test.cpp.
  int local = 0;
  void* const never_mapped = &local;
  EXPECT_EQ(syscalls.lock_memory(never_mapped, 4096).error().number, ENOMEM);
  EXPECT_EQ(syscalls.advise_memory(never_mapped, 4096, MADV_HUGEPAGE).error().number, ENOMEM);
  EXPECT_EQ(syscalls.bind_memory(never_mapped, 4096, nodes({0})).error().number, EFAULT);
  EXPECT_EQ(syscalls.unmap(never_mapped, 4096).error().number, EINVAL);

  // And the huge-page length trap itself: the unrounded length is EINVAL and
  // the mapping stays, exactly as observed.
  syscalls.set_huge_pages_free(1);
  auto huge = syscalls.map_anonymous(1, MAP_HUGETLB);
  ASSERT_TRUE(huge.has_value());
  EXPECT_EQ(syscalls.unmap(huge.value(), 1).error().number, EINVAL);
  EXPECT_EQ(syscalls.live_mappings(), 1U);
  EXPECT_EQ(syscalls.huge_pages_free(), 0U);
  EXPECT_TRUE(syscalls.unmap(huge.value(), FakeSyscalls::kFakeHugePageBytes).has_value());
  EXPECT_EQ(syscalls.huge_pages_free(), 1U);

  // An empty node mask is EINVAL, as on Linux.
  auto plain = syscalls.map_anonymous(4096, 0);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(syscalls.bind_memory(plain.value(), 4096, NodeMask{}).error().number, EINVAL);
  EXPECT_TRUE(syscalls.unmap(plain.value(), 4096).has_value());
}

}  // namespace
}  // namespace loadforge::platform
