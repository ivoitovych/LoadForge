// SPDX-License-Identifier: GPL-3.0-or-later
//
// SIGINT and SIGTERM as the controller sees them: bytes in a pipe, over the fake. The
// handler itself is tested against the kernel in real_syscalls_test.cpp.

#include "controller/stop_signals.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <string>
#include <vector>

#include "support/fake_syscalls.hpp"

namespace loadforge::controller {
namespace {

using loadforge::testing::FakeSyscalls;

constexpr int kReadEnd = 200;
constexpr int kWriteEnd = 201;

TEST(StopSignals, InstallingDivertsBothSignalsIntoTheOnePipe) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  EXPECT_EQ(stop.fd(), -1) << "nothing before install";

  ASSERT_TRUE(stop.install().has_value());

  EXPECT_EQ(stop.fd(), kReadEnd);
  EXPECT_EQ(syscalls.signals_diverted().size(), 2U);
  EXPECT_EQ(syscalls.signals_diverted().at(SIGINT), kWriteEnd);
  EXPECT_EQ(syscalls.signals_diverted().at(SIGTERM), kWriteEnd);
  EXPECT_TRUE(stop.received().empty());
  EXPECT_FALSE(stop.first().has_value());
}

TEST(StopSignals, ASignalIsAbsorbedAndTheFirstOfThemIsTheOneThatCameFirst) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());

  auto none = stop.absorb();
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none.value(), 0U) << "a look, not a wait: an empty pipe is nothing";

  ASSERT_TRUE(syscalls.raise_diverted(SIGTERM));
  ASSERT_TRUE(syscalls.raise_diverted(SIGINT));
  auto two = stop.absorb();
  ASSERT_TRUE(two.has_value());
  EXPECT_EQ(two.value(), 2U);
  EXPECT_EQ(stop.received(), (std::vector<int>{SIGTERM, SIGINT}));
  ASSERT_TRUE(stop.first().has_value());
  EXPECT_EQ(*stop.first(), SIGTERM);

  ASSERT_TRUE(syscalls.raise_diverted(SIGINT));
  EXPECT_EQ(stop.absorb().value(), 1U);
  EXPECT_EQ(stop.received().size(), 3U) << "the list keeps every one, across looks";
  EXPECT_EQ(*stop.first(), SIGTERM);
}

TEST(StopSignals, MoreSignalsThanOneLookTakesAreTakenByTheNext) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());
  constexpr std::size_t kMoreThanOneLook = 20;
  for (std::size_t each = 0; each < kMoreThanOneLook; ++each) {
    ASSERT_TRUE(syscalls.raise_diverted(SIGINT));
  }
  const auto first = stop.absorb();
  ASSERT_TRUE(first.has_value());
  EXPECT_LT(first.value(), kMoreThanOneLook);
  ASSERT_TRUE(stop.absorb().has_value());
  EXPECT_EQ(stop.received().size(), kMoreThanOneLook);
}

TEST(StopSignals, ReleasingPutsBothSignalsBackAndClosesThePipe) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());

  stop.release();

  EXPECT_EQ(stop.fd(), -1);
  EXPECT_TRUE(syscalls.signals_diverted().empty());
  EXPECT_EQ(syscalls.signals_defaulted(), (std::vector<int>{SIGINT, SIGTERM}));
  EXPECT_TRUE(syscalls.all_descriptors_closed());
  EXPECT_FALSE(syscalls.raise_diverted(SIGINT)) << "the default action would have run";
}

TEST(StopSignals, ReleasingTwiceOrBeforeInstallingDoesNothing) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  stop.release();
  EXPECT_TRUE(syscalls.signals_defaulted().empty());
  ASSERT_TRUE(stop.install().has_value());
  stop.release();
  stop.release();
  EXPECT_EQ(syscalls.signals_defaulted().size(), 2U);
}

TEST(StopSignals, TheDestructorReleases) {
  FakeSyscalls syscalls;
  {
    StopSignals stop(syscalls);
    ASSERT_TRUE(stop.install().has_value());
  }
  EXPECT_TRUE(syscalls.signals_diverted().empty());
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST(StopSignals, ReleasingIsSilentWhateverTheCallsAnswer) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());
  syscalls.fail_default_signal(EINVAL);
  syscalls.fail_close_of(kReadEnd, EIO);
  syscalls.fail_close_of(kWriteEnd, EBADF);

  stop.release();

  EXPECT_EQ(stop.fd(), -1) << "released as far as this object can";
  EXPECT_EQ(syscalls.signals_defaulted().size(), 2U) << "each tried, the first refusal not the end";
}

// ---- every failure of every call --------------------------------------------------

class InstallPipeFails : public ::testing::TestWithParam<int> {};

TEST_P(InstallPipeFails, NothingIsInstalledAndTheErrorIsTheKernels) {
  FakeSyscalls syscalls;
  syscalls.fail_create_pipe(GetParam());
  StopSignals stop(syscalls);

  auto installed = stop.install();

  ASSERT_FALSE(installed.has_value());
  EXPECT_EQ(installed.error().number, GetParam());
  EXPECT_EQ(installed.error().call, "pipe2");
  EXPECT_EQ(stop.fd(), -1);
  EXPECT_EQ(syscalls.divert_calls(), 0);
  EXPECT_TRUE(syscalls.signals_defaulted().empty());
}

// pipe2(2): EFAULT, EINVAL, EMFILE, ENFILE, ENOPKG (not for pipes).
INSTANTIATE_TEST_SUITE_P(Pipe2Errors, InstallPipeFails,
                         ::testing::Values(EFAULT, EINVAL, EMFILE, ENFILE));

class InstallDivertFails : public ::testing::TestWithParam<std::pair<int, int>> {};

TEST_P(InstallDivertFails, NothingStaysInstalledAndTheDescriptorsAreBack) {
  const auto [which, error] = GetParam();
  FakeSyscalls syscalls;
  syscalls.fail_divert_signal_on(which, error);
  StopSignals stop(syscalls);

  auto installed = stop.install();

  ASSERT_FALSE(installed.has_value());
  EXPECT_EQ(installed.error().number, error);
  EXPECT_EQ(installed.error().call, "sigaction");
  EXPECT_EQ(stop.fd(), -1);
  EXPECT_TRUE(syscalls.signals_diverted().empty()) << "a half-installed pair is undone";
  EXPECT_TRUE(syscalls.all_descriptors_closed());
  EXPECT_EQ(syscalls.divert_calls(), which) << "the first refusal ends it";
}

// sigaction(2): EFAULT, EINVAL; the first call is SIGINT's, the second SIGTERM's.
INSTANTIATE_TEST_SUITE_P(SigactionErrors, InstallDivertFails,
                         ::testing::Values(std::pair{1, EFAULT}, std::pair{1, EINVAL},
                                           std::pair{2, EFAULT}, std::pair{2, EINVAL}));

class AbsorbPollFails : public ::testing::TestWithParam<int> {};

TEST_P(AbsorbPollFails, TheLookReportsTheKernelsError) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());
  syscalls.fail_poll(GetParam());

  auto absorbed = stop.absorb();

  ASSERT_FALSE(absorbed.has_value());
  EXPECT_EQ(absorbed.error().number, GetParam());
  EXPECT_EQ(absorbed.error().call, "poll");
  EXPECT_TRUE(stop.received().empty());
}

// poll(2): EFAULT, EINTR (answered by RealSyscalls as nothing ready, and still an
// error from a seam that says otherwise), EINVAL, ENOMEM.
INSTANTIATE_TEST_SUITE_P(PollErrors, AbsorbPollFails,
                         ::testing::Values(EFAULT, EINTR, EINVAL, ENOMEM));

class AbsorbReadFails : public ::testing::TestWithParam<int> {};

TEST_P(AbsorbReadFails, TheLookReportsTheKernelsErrorAndKeepsWhatItHad) {
  FakeSyscalls syscalls;
  StopSignals stop(syscalls);
  ASSERT_TRUE(stop.install().has_value());
  ASSERT_TRUE(syscalls.raise_diverted(SIGINT));
  syscalls.script_reads({FakeSyscalls::ReadStep{"", GetParam()}});

  auto absorbed = stop.absorb();

  ASSERT_FALSE(absorbed.has_value());
  EXPECT_EQ(absorbed.error().number, GetParam());
  EXPECT_EQ(absorbed.error().call, "read");
  EXPECT_TRUE(stop.received().empty());
  EXPECT_EQ(stop.absorb().value(), 1U) << "the byte was not taken, so the next look has it";
}

// read(2): EAGAIN, EBADF, EFAULT, EINTR, EINVAL, EIO, EISDIR.
INSTANTIATE_TEST_SUITE_P(ReadErrors, AbsorbReadFails,
                         ::testing::Values(EAGAIN, EBADF, EFAULT, EINTR, EINVAL, EIO, EISDIR));

}  // namespace
}  // namespace loadforge::controller
