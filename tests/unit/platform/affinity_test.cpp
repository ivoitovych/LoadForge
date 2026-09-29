// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a thread may run, and the gap between "the machine has this CPU" and
// "this process may use it".
//
// LOADFORGE P7 FIXTURE BUILDER
//
// The hostile state this module's capability paths need is not a tree on disk
// but a cpuset: a kernel that permits fewer CPUs than the topology found. That
// cannot be committed and cannot be arranged in CI, which runs as root inside
// a container whose cpuset is whatever the runner gave it. So it is BUILT, in
// FakeSyscalls, which models sched_setaffinity(2) the way the kernel behaves:
// the mask is intersected with what the cpuset permits and refused with EINVAL
// when nothing is left. The real kernel's own answers -- EINVAL for an empty
// or unknown mask, ESRCH for a vanished thread -- are asserted in
// real_syscalls_test.cpp, against the calling process only.

#include "platform/affinity.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::platform {
namespace {

using loadforge::testing::FakeSyscalls;

CpuMask mask(std::initializer_list<std::uint32_t> ids) {
  return mask_of(std::vector<std::uint32_t>(ids)).value();
}

// --- masks -------------------------------------------------------------------

TEST(CpuMaskTest, IdsRoundTripThroughAMask) {
  EXPECT_EQ(ids_of(mask({0, 2, 3})), (std::vector<std::uint32_t>{0, 2, 3}));
  EXPECT_EQ(ids_of(mask({})), (std::vector<std::uint32_t>{}));
  EXPECT_EQ(ids_of(mask({1023})), (std::vector<std::uint32_t>{1023}))
      << "the last representable id";
}

TEST(CpuMaskTest, AnIdPastTheMaskIsRefusedNotDropped) {
  auto result = mask_of({0, 1024});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(std::string{result.error()}.find("past what an affinity mask"), std::string::npos);
}

TEST(CpuMaskTest, EqualityIsBitwise) {
  EXPECT_EQ(mask({0, 2}), mask({0, 2}));
  EXPECT_NE(mask({0, 2}), mask({0, 3})) << "one bit apart";
  EXPECT_NE(mask({}), mask({0}));
}

TEST(CpuMaskTest, DescribeListsTheIds) {
  EXPECT_EQ(describe(mask({0, 2, 3})), "0,2,3");
  EXPECT_EQ(describe(mask({})), "(no CPUs)");
}

TEST(CpuMaskTest, MissingFromIsWantedMinusPermitted) {
  // The capability question: the topology found 0-3, the cpuset permits 0-1.
  EXPECT_EQ(ids_of(missing_from(mask({0, 1, 2, 3}), mask({0, 1}))),
            (std::vector<std::uint32_t>{2, 3}));
  EXPECT_TRUE(ids_of(missing_from(mask({0, 1}), mask({0, 1, 2, 3}))).empty())
      << "wanting less than permitted is not a gap";
  EXPECT_TRUE(ids_of(missing_from(mask({}), mask({}))).empty());
}

// --- classification ----------------------------------------------------------

TEST(PlacementTest, EveryKindHasItsOwnWordsAndTheTableIsTotal) {
  EXPECT_EQ(describe(Placement::kNoUsableCpu), "no CPU in the mask is one this thread may run on");
  EXPECT_EQ(describe(Placement::kThreadGone), "the thread no longer exists");
  EXPECT_EQ(describe(Placement::kNotPermitted), "not permitted to move this thread");
  EXPECT_EQ(describe(Placement::kUnexpected),
            "the kernel refused for a reason this code does not expect");
  EXPECT_EQ(describe(static_cast<Placement>(99)), "unrecognised placement failure");
}

TEST(PlacementTest, ClassifiesEachDocumentedErrno) {
  auto err = [](int number) { return SyscallError{number, "sched_setaffinity", "pid 0"}; };
  EXPECT_EQ(classify_placement(err(EINVAL)), Placement::kNoUsableCpu);
  EXPECT_EQ(classify_placement(err(ESRCH)), Placement::kThreadGone);
  EXPECT_EQ(classify_placement(err(EPERM)), Placement::kNotPermitted);
  EXPECT_EQ(classify_placement(err(EFAULT)), Placement::kUnexpected);
  EXPECT_EQ(classify_placement(err(EIO)), Placement::kUnexpected);
}

// --- Affinity against the fake (T2) ------------------------------------------

class AffinityTest : public ::testing::Test {
 protected:
  FakeSyscalls syscalls;
  Affinity affinity{syscalls};
};

TEST_F(AffinityTest, PermittedReportsWhatTheKernelSays) {
  syscalls.set_current_affinity(mask({0, 1, 2, 3}));
  auto permitted = affinity.permitted();
  ASSERT_TRUE(permitted.has_value());
  EXPECT_EQ(ids_of(permitted.value()), (std::vector<std::uint32_t>{0, 1, 2, 3}));
  EXPECT_EQ(syscalls.affinity_pids(), (std::vector<pid_t>{0})) << "pid 0 is the calling thread";
}

TEST_F(AffinityTest, PermittedPropagatesAFailure) {
  syscalls.fail_get_affinity(ESRCH);
  auto permitted = affinity.permitted(4242);
  ASSERT_FALSE(permitted.has_value());
  EXPECT_EQ(permitted.error().number, ESRCH);
  EXPECT_EQ(permitted.error().call, "sched_getaffinity");
  EXPECT_EQ(permitted.error().subject, "pid 4242");
}

TEST_F(AffinityTest, PlacingOnPermittedCpusSucceedsAndIsVisibleAfterwards) {
  auto placed = affinity.place(mask({2}));
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(ids_of(syscalls.current_affinity()), (std::vector<std::uint32_t>{2}));
}

TEST_F(AffinityTest, TheKernelConfinesToTheIntersectionWhenTheCpusetIsNarrower) {
  // The P7 state itself. The topology would say 0-3 exist; the cpuset permits
  // only 0-1; the caller asks for 1-2. The kernel does NOT refuse -- it
  // silently grants {1}. That silence is why missing_from exists: a caller that
  // wants to know must ask, before or after.
  syscalls.permit_cpus(mask({0, 1}));
  auto placed = affinity.place(mask({1, 2}));
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(ids_of(syscalls.current_affinity()), (std::vector<std::uint32_t>{1}))
      << "confined to what the cpuset permits, without a word";

  auto now = affinity.permitted();
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(ids_of(missing_from(mask({1, 2}), now.value())), (std::vector<std::uint32_t>{2}))
      << "asking afterwards reveals what was withheld";
}

TEST_F(AffinityTest, AMaskEntirelyOutsideTheCpusetIsRefusedAsNoUsableCpu) {
  syscalls.permit_cpus(mask({0, 1}));
  auto placed = affinity.place(mask({2, 3}));
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().kind, Placement::kNoUsableCpu);
  EXPECT_EQ(placed.error().cause.number, EINVAL);
  EXPECT_EQ(describe(placed.error().requested), "2,3");
  EXPECT_NE(describe(placed.error()).find("CPU(s) 2,3"), std::string::npos)
      << "the message names what was asked, which is how a reader tells the three EINVALs apart";
}

TEST_F(AffinityTest, AnEmptyMaskIsSentToTheKernelAndRefusedByIt) {
  // Deliberately not pre-checked above the seam: the kernel is the authority,
  // and this asserts the fake is no laxer than it.
  auto placed = affinity.place(mask({}));
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().kind, Placement::kNoUsableCpu);
  EXPECT_EQ(syscalls.affinity_pids().size(), 1U) << "the call was made";
}

TEST_F(AffinityTest, AVanishedThreadIsReportedAsGoneNotAsABadMask) {
  syscalls.fail_set_affinity(ESRCH);
  auto placed = affinity.place(mask({0}), 4242);
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().kind, Placement::kThreadGone);
  EXPECT_EQ(placed.error().cause.subject, "pid 4242");
}

TEST_F(AffinityTest, AnotherUsersThreadIsNotPermitted) {
  syscalls.fail_set_affinity(EPERM);
  auto placed = affinity.place(mask({0}), 1);
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().kind, Placement::kNotPermitted);
}

TEST_F(AffinityTest, AnUnexpectedErrnoIsCarriedThroughVerbatim) {
  syscalls.fail_set_affinity(EFAULT);
  auto placed = affinity.place(mask({0}));
  ASSERT_FALSE(placed.has_value());
  // Field by field: this failure is copied out of one Result into another, the
  // shape clang-analyzer distrusts. Every field is asserted, not just the kind.
  EXPECT_EQ(placed.error().kind, Placement::kUnexpected);
  EXPECT_EQ(placed.error().cause.number, EFAULT);
  EXPECT_EQ(placed.error().cause.call, "sched_setaffinity");
  EXPECT_EQ(placed.error().requested, mask({0}));
  EXPECT_NE(describe(placed.error()).find("does not expect"), std::string::npos);
}

TEST_F(AffinityTest, PermittingFewerCpusNarrowsTheCurrentMaskAsTheKernelWould) {
  // A cgroup change under a running process narrows what it is running on;
  // the fake models that so a test can stage "the cpuset shrank mid-run".
  syscalls.set_current_affinity(mask({0, 1, 2, 3}));
  syscalls.permit_cpus(mask({2, 3}));
  auto now = affinity.permitted();
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(ids_of(now.value()), (std::vector<std::uint32_t>{2, 3}));
}

}  // namespace
}  // namespace loadforge::platform
