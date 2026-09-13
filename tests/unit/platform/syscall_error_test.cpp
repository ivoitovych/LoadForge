// SPDX-License-Identifier: GPL-3.0-or-later
//
// The error renderer is small and entirely user-facing: what it produces is
// what someone reads when their machine does not behave. Every branch here is
// about a message being actionable rather than merely present.

#include "platform/syscall_error.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

namespace loadforge::platform {
namespace {

TEST(SyscallError, NamesTheCallTheSubjectAndTheReason) {
  const SyscallError error{EACCES, "open", "/sys/class/powercap/intel-rapl:0/energy_uj"};

  const std::string message = describe(error);

  EXPECT_NE(message.find("open"), std::string::npos) << message;
  EXPECT_NE(message.find("energy_uj"), std::string::npos) << message;
  EXPECT_NE(message.find("Permission denied"), std::string::npos) << message;
}

TEST(SyscallError, OmitsTheParenthesesWhenThereIsNoSubject) {
  const SyscallError error{EINVAL, "prctl", ""};

  const std::string message = describe(error);

  // No subject parentheses -- the message opens straight onto the reason.
  // The trailing "(errno N)" is always present and is a different thing.
  EXPECT_EQ(message.rfind("prctl: ", 0), 0U) << message;
  EXPECT_EQ(message.find("()"), std::string::npos) << message;
}

TEST(SyscallError, AlwaysCarriesTheErrnoNumber) {
  // The number survives translation and truncation, it is what a user searches
  // for, and it is what makes a report from an unfamiliar machine actionable.
  // It is present even when the description is perfectly clear.
  const SyscallError error{EACCES, "open", "/sys/a"};

  EXPECT_NE(describe(error).find("(errno 13)"), std::string::npos) << describe(error);
}

TEST(SyscallError, AnUnrecognisedErrnoStillSaysSomething) {
  // Silence about a failure is the one thing this project does not do (F20).
  // A message ending in ": " would read as success to anyone skimming a log.
  const SyscallError error{999999, "read", "fd 7"};

  const std::string message = describe(error);

  EXPECT_NE(message.find("read(fd 7)"), std::string::npos) << message;
  EXPECT_NE(message.find("999999"), std::string::npos) << message;
  EXPECT_GT(message.size(), std::string("read(fd 7): ").size()) << message;
}

TEST(SyscallError, ComparesByEveryField) {
  const SyscallError base{EACCES, "open", "/sys/a"};

  EXPECT_EQ(base, (SyscallError{EACCES, "open", "/sys/a"}));
  EXPECT_NE(base, (SyscallError{EIO, "open", "/sys/a"}));
  EXPECT_NE(base, (SyscallError{EACCES, "read", "/sys/a"}));
  EXPECT_NE(base, (SyscallError{EACCES, "open", "/sys/b"}));
}

TEST(SyscallError, DefaultsToNoError) {
  const SyscallError error;

  EXPECT_EQ(error.number, 0);
  EXPECT_TRUE(error.call.empty());
  EXPECT_TRUE(error.subject.empty());
}

}  // namespace
}  // namespace loadforge::platform
