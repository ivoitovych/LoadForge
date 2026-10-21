// SPDX-License-Identifier: GPL-3.0-or-later
//
// One mapping made under a lock policy (runner.hpp, LockPolicy; contract change
// of the M2 corrections, 2): what each policy asks of the kernel, which refusals
// kTry survives and which it does not, and what is left mapped either way.

#include "verification/integrity/locking.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include "unit/verification/integrity/integrity_support.hpp"

namespace loadforge::verification::integrity {
namespace {

using integrity_testing::SteppedSyscalls;
using Call = SteppedSyscalls::Call;

platform::MappingRequest a_request() {
  platform::MappingRequest request;
  request.bytes = 8192;
  request.populate = true;
  request.lock = false;  // The policy decides, whatever this says.
  return request;
}

TEST(LockingTest, RequireLocksAndSaysSo) {
  SteppedSyscalls syscalls;
  platform::Memory memory(syscalls);
  const auto placed = map_by_policy(memory, a_request(), LockPolicy::kRequire);
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(placed.value().outcome, LockOutcome::kLocked);
  EXPECT_TRUE(placed.value().detail.empty());
  EXPECT_EQ(syscalls.fake.locked_bytes(), 8192U);
}

TEST(LockingTest, RequireRefusedIsTheRefusalAsItCame) {
  for (const int error : {ENOMEM, EPERM, EAGAIN, EINVAL}) {
    SteppedSyscalls syscalls;
    syscalls.fail(Call::kLock, 1, error);
    platform::Memory memory(syscalls);
    const auto placed = map_by_policy(memory, a_request(), LockPolicy::kRequire);
    ASSERT_FALSE(placed.has_value()) << error;
    EXPECT_EQ(placed.error().stage, platform::Stage::kLock);
    EXPECT_EQ(placed.error().cause.number, error);
    EXPECT_EQ(syscalls.fake.live_mappings(), 0U);
  }
}

TEST(LockingTest, NoneNeverAsksTheKernelToLock) {
  SteppedSyscalls syscalls;
  syscalls.fake.set_lock_limit(0);  // A lock would be refused: none is asked.
  platform::Memory memory(syscalls);
  auto request = a_request();
  request.lock = true;  // Ignored: the policy decides.
  const auto placed = map_by_policy(memory, request, LockPolicy::kNone);
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(placed.value().outcome, LockOutcome::kNotLocked);
  EXPECT_TRUE(placed.value().detail.empty());
  EXPECT_EQ(syscalls.fake.locked_bytes(), 0U);
  EXPECT_EQ(syscalls.fake.live_mappings(), 1U) << "still mapped, still populated";
}

TEST(LockingTest, TryThatLocksIsLocked) {
  SteppedSyscalls syscalls;
  platform::Memory memory(syscalls);
  const auto placed = map_by_policy(memory, a_request(), LockPolicy::kTry);
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(placed.value().outcome, LockOutcome::kLocked);
  EXPECT_TRUE(placed.value().detail.empty());
}

TEST(LockingTest, TryFallsBackWhenTheLockIsRefusedAndKeepsTheKernelsWords) {
  for (const int error : {ENOMEM, EPERM, EAGAIN, EINVAL}) {
    SteppedSyscalls syscalls;
    syscalls.fail(Call::kLock, 1, error);
    platform::Memory memory(syscalls);
    const auto placed = map_by_policy(memory, a_request(), LockPolicy::kTry);
    ASSERT_TRUE(placed.has_value()) << error;
    EXPECT_EQ(placed.value().outcome, LockOutcome::kFellBack);
    EXPECT_NE(placed.value().detail.find("locking failed"), std::string::npos)
        << placed.value().detail;
    EXPECT_NE(placed.value().detail.find("mlock"), std::string::npos) << placed.value().detail;
    EXPECT_EQ(syscalls.fake.live_mappings(), 1U) << "the refused mapping was unmapped";
    EXPECT_EQ(syscalls.fake.locked_bytes(), 0U);
  }
}

TEST(LockingTest, TryDoesNotSurviveARefusalAtAnyOtherStep) {
  const struct {
    Call call;
    platform::Stage stage;
    std::vector<int> errors;
  } calls[] = {
      {Call::kMap, platform::Stage::kMap, {EINVAL, ENOMEM, EAGAIN, EPERM}},
      {Call::kBind, platform::Stage::kBind, {EFAULT, EINVAL, EIO, ENOMEM, EPERM}},
      {Call::kPopulate, platform::Stage::kPopulate, {EINVAL, ENOMEM, EFAULT, EPERM, EINTR}},
  };
  for (const auto& call : calls) {
    for (const int error : call.errors) {
      SteppedSyscalls syscalls;
      syscalls.fail(call.call, 1, error);
      platform::Memory memory(syscalls);
      auto request = a_request();
      request.node = 0;
      const auto placed = map_by_policy(memory, request, LockPolicy::kTry);
      ASSERT_FALSE(placed.has_value()) << error;
      EXPECT_EQ(placed.error().stage, call.stage);
      EXPECT_EQ(placed.error().cause.number, error);
      EXPECT_LE(syscalls.mapped().size(), 1U) << "no second attempt was made";
    }
  }
}

TEST(LockingTest, TryWhoseSecondAttemptIsRefusedToIsTheSecondRefusal) {
  SteppedSyscalls syscalls;
  syscalls.fail(Call::kLock, 1, ENOMEM);
  syscalls.fail(Call::kMap, 2, EAGAIN);  // The retry's mapping.
  platform::Memory memory(syscalls);
  const auto placed = map_by_policy(memory, a_request(), LockPolicy::kTry);
  ASSERT_FALSE(placed.has_value());
  EXPECT_EQ(placed.error().stage, platform::Stage::kMap);
  EXPECT_EQ(placed.error().cause.number, EAGAIN);
  EXPECT_EQ(syscalls.fake.live_mappings(), 0U);
}

}  // namespace
}  // namespace loadforge::verification::integrity
