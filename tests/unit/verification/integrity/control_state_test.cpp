// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control state's placement and its refusals (P1, P2). What a corrupted
// copy does to a vote is the fault-injection tier's (control_state_fault_test).

#include "verification/integrity/control_state.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/crc32.hpp"
#include "unit/verification/integrity/integrity_support.hpp"

namespace loadforge::verification::integrity {
namespace {

using integrity_testing::SteppedSyscalls;
using Call = SteppedSyscalls::Call;

ControlWords sample_words() { return ControlWords{0xC0FFEE, 1, 41, 3, 5, 128}; }

platform::NodeMask only(std::uint32_t node) {
  platform::NodeMask mask;
  mask.nodes.set(node);
  return mask;
}

TEST(ControlStateTest, ThreeCopiesEachInItsOwnPopulatedLockedMapping) {
  SteppedSyscalls syscalls;
  const auto state = ControlState::create(syscalls, sample_words(), {});
  ASSERT_TRUE(state.has_value());
  ASSERT_EQ(syscalls.mapped().size(), kCopies);
  std::set<std::byte*> addresses;
  for (std::size_t copy = 0; copy < kCopies; ++copy) {
    EXPECT_EQ(syscalls.mapped()[copy].bytes, kCopyBytes);
    addresses.insert(syscalls.mapped()[copy].address);
  }
  EXPECT_EQ(addresses.size(), kCopies) << "no two copies share a mapping";
  EXPECT_EQ(std::count(syscalls.fake.advice_given().begin(), syscalls.fake.advice_given().end(),
                       MADV_POPULATE_WRITE),
            3);
  EXPECT_EQ(syscalls.fake.locked_bytes(), kCopies * kCopyBytes);
  EXPECT_TRUE(syscalls.fake.binds().empty()) << "no node given, none bound";
  for (const int flags : syscalls.fake.map_flags()) {
    EXPECT_EQ(flags, 0) << "ordinary pages";
  }
}

TEST(ControlStateTest, EveryCopyHoldsTheSameBytesWrittenByItsOwnPass) {
  SteppedSyscalls syscalls;
  auto state = ControlState::create(syscalls, sample_words(), {});
  ASSERT_TRUE(state.has_value());
  const std::span<std::byte> first = syscalls.mapping(0);
  for (std::size_t copy = 1; copy < kCopies; ++copy) {
    EXPECT_EQ(std::memcmp(first.data(), syscalls.mapping(copy).data(), kCopyBytes), 0) << copy;
  }
  // Little-endian words, then the checksum of those 48 bytes.
  EXPECT_EQ(first[0], std::byte{0xEE});
  EXPECT_EQ(first[1], std::byte{0xFF});
  EXPECT_EQ(first[2], std::byte{0xC0});
  EXPECT_EQ(first[16], std::byte{41}) << "word 2, the index";
  const std::uint32_t crc = core::crc32(first.first(kControlWords * 8));
  EXPECT_EQ(first[48], static_cast<std::byte>(crc));
  EXPECT_EQ(first[51], static_cast<std::byte>(crc >> 24U));
}

TEST(ControlStateTest, WhatIsWrittenIsWhatIsRead) {
  SteppedSyscalls syscalls;
  auto state = ControlState::create(syscalls, sample_words(), {});
  ASSERT_TRUE(state.has_value());
  ControlState control = std::move(state).value();
  const auto initial = control.read();
  ASSERT_TRUE(initial.has_value());
  EXPECT_EQ(initial.value(), sample_words());
  ControlWords next = sample_words();
  set_word(next, ControlWord::kIndex, ~std::uint64_t{0});
  control.write(next);
  const auto read = control.read();
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(word(read.value(), ControlWord::kIndex), ~std::uint64_t{0});
  EXPECT_EQ(word(read.value(), ControlWord::kSeed), 0xC0FFEEU);
  EXPECT_EQ(control.repairs(), 0U) << "a clean vote repairs nothing";
}

TEST(ControlStateTest, TheCopiesAreSpreadRoundRobinOverTheNodesGiven) {
  SteppedSyscalls syscalls;
  syscalls.fake.permit_nodes(platform::NodeMask{only(0).nodes | only(1).nodes});
  const std::vector<std::uint32_t> two{0, 1};
  ASSERT_TRUE(ControlState::create(syscalls, sample_words(), two).has_value());
  ASSERT_EQ(syscalls.fake.binds().size(), 3U);
  EXPECT_EQ(syscalls.fake.binds()[0], only(0));
  EXPECT_EQ(syscalls.fake.binds()[1], only(1));
  EXPECT_EQ(syscalls.fake.binds()[2], only(0));

  SteppedSyscalls single;
  const std::vector<std::uint32_t> one{0};
  ASSERT_TRUE(ControlState::create(single, sample_words(), one).has_value());
  ASSERT_EQ(single.fake.binds().size(), 3U);
  for (const platform::NodeMask& mask : single.fake.binds()) {
    EXPECT_EQ(mask, only(0)) << "one node: every copy on it";
  }
}

TEST(ControlStateTest, ACopyThatCannotBeMappedIsNamedAndTheEarlierOnesAreUnmapped) {
  // One test per errno per call, for each copy: the call's documented errors.
  const struct {
    Call call;
    platform::Stage stage;
    std::vector<int> errors;
  } calls[] = {
      {Call::kMap, platform::Stage::kMap, {EINVAL, ENOMEM, EAGAIN, EPERM}},
      {Call::kBind, platform::Stage::kBind, {EFAULT, EINVAL, EIO, ENOMEM, EPERM}},
      {Call::kPopulate,
       platform::Stage::kPopulate,
       {EINVAL, ENOMEM, EFAULT, EPERM, EINTR, EAGAIN, EHWPOISON}},
      {Call::kLock, platform::Stage::kLock, {ENOMEM, EPERM, EAGAIN, EINVAL}},
  };
  const std::vector<std::uint32_t> node{0};
  for (const auto& call : calls) {
    for (const int error : call.errors) {
      for (std::size_t copy = 0; copy < kCopies; ++copy) {
        SCOPED_TRACE(std::string(platform::describe(call.stage)) + ", errno " +
                     std::to_string(error) + ", copy " + std::to_string(copy));
        SteppedSyscalls syscalls;
        syscalls.fail(call.call, static_cast<int>(copy) + 1, error);
        const auto state = ControlState::create(syscalls, sample_words(), node);
        ASSERT_FALSE(state.has_value());
        EXPECT_EQ(state.error().copy, copy);
        EXPECT_EQ(state.error().refused.stage, call.stage);
        EXPECT_EQ(state.error().refused.cause.number, error);
        EXPECT_EQ(state.error().refused.request.bytes, kCopyBytes);
        EXPECT_TRUE(state.error().refused.request.populate);
        EXPECT_TRUE(state.error().refused.request.lock);
        EXPECT_EQ(syscalls.fake.live_mappings(), 0U) << "nothing left mapped";
        EXPECT_EQ(syscalls.fake.locked_bytes(), 0U);
      }
    }
  }
}

TEST(ControlStateTest, TheVoteFaultsDescribeThemselves) {
  EXPECT_EQ(describe(VoteFault::kDisagreement),
            "the verified copies of the control state disagree");
  EXPECT_EQ(describe(VoteFault::kTooFewVerified),
            "fewer than two copies of the control state pass their checksums");
  EXPECT_EQ(describe(static_cast<VoteFault>(7)), "unrecognised vote fault");
  EXPECT_EQ(describe(VoteFailure{VoteFault::kTooFewVerified, {true, false, false}}),
            "fewer than two copies of the control state pass their checksums (copy 0 verified, "
            "copy 1 fails its checksum, copy 2 fails its checksum)");
  EXPECT_EQ(describe(VoteFailure{VoteFault::kDisagreement, {true, true, true}}),
            "the verified copies of the control state disagree (copy 0 verified, copy 1 "
            "verified, copy 2 verified)");
}

TEST(ControlStateTest, NamedWordsAreTheirPositions) {
  ControlWords words{};
  set_word(words, ControlWord::kLineBytes, 64);
  EXPECT_EQ(words[5], 64U);
  EXPECT_EQ(word(words, ControlWord::kLineBytes), 64U);
  EXPECT_EQ(word(words, ControlWord::kSeed), 0U);
}

}  // namespace
}  // namespace loadforge::verification::integrity
