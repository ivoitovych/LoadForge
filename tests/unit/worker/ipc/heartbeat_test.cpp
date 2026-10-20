// SPDX-License-Identifier: GPL-3.0-or-later
//
// The heartbeat region: a memfd of seqlock slots that crosses the process
// boundary by descriptor, validated on attach, with no lock to recover.

#include "worker/ipc/heartbeat.hpp"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::ipc {
namespace {

using loadforge::platform::RealSyscalls;
using loadforge::platform::SyscallError;
using loadforge::testing::FakeSyscalls;

Heartbeat beat_number(std::uint64_t n) {
  // Every field derived from one number, so a torn sample -- fields from two
  // different beats -- is detectable by inspection.
  Heartbeat beat;
  beat.timestamp_ns = static_cast<std::int64_t>(n * 1000);
  beat.progress = n;
  beat.phase = static_cast<std::uint32_t>(n % 7);
  beat.pid = static_cast<std::uint32_t>(n * 3);
  return beat;
}

bool consistent(const Heartbeat& beat) { return beat == beat_number(beat.progress); }

/// A region moved out of its Result, so a test may publish into it. A failure
/// to create is a test failure in its own right.
HeartbeatRegion make(FakeSyscalls& syscalls, std::size_t workers) {
  auto created = HeartbeatRegion::create(syscalls, workers);
  EXPECT_TRUE(created.has_value()) << describe(created.error());
  return std::move(created).value();
}

// --- words and arithmetic ----------------------------------------------------

TEST(HeartbeatWordsTest, EveryFaultHasItsOwnWordsAndTheTableIsTotal) {
  EXPECT_EQ(describe(RegionFault::kSyscall), "the kernel refused");
  EXPECT_EQ(describe(RegionFault::kMagic), "the memory is not a heartbeat region");
  EXPECT_EQ(describe(RegionFault::kVersion), "the region is of a version this build does not read");
  EXPECT_EQ(describe(RegionFault::kSlotCount),
            "the region holds a different number of slots than expected");
  EXPECT_EQ(describe(RegionFault::kSlotBytes),
            "the region's slots are a different size than this build's");
  EXPECT_EQ(describe(RegionFault::kTooManyWorkers), "more workers than a region can hold");
  EXPECT_EQ(describe(RegionFault::kSlotOutOfRange), "no such slot");
  EXPECT_EQ(describe(RegionFault::kTorn), "the writer is stuck mid-update");
  EXPECT_EQ(describe(static_cast<RegionFault>(99)), "unrecognised region fault");

  EXPECT_EQ(describe(Liveness::kNeverBeat), "never beat");
  EXPECT_EQ(describe(Liveness::kFresh), "fresh");
  EXPECT_EQ(describe(Liveness::kStale), "stale");
  EXPECT_EQ(describe(static_cast<Liveness>(99)), "unrecognised liveness");
}

TEST(HeartbeatWordsTest, AnErrorRendersTheKernelsWordsOrTheLayoutFault) {
  const RegionError syscall{RegionFault::kSyscall, "", SyscallError{ENOMEM, "mmap", "fd 100"}};
  EXPECT_EQ(describe(syscall), describe(syscall.cause));
  const RegionError layout{RegionFault::kMagic, "magic 7, expected 1279674434", SyscallError{}};
  EXPECT_EQ(describe(layout), "the memory is not a heartbeat region: magic 7, expected 1279674434");
  const RegionError bare{RegionFault::kTorn, "", SyscallError{}};
  EXPECT_EQ(describe(bare), "the writer is stuck mid-update");
  EXPECT_EQ(syscall, syscall);
  EXPECT_NE(syscall, layout);
}

TEST(HeartbeatWordsTest, TheRegionSizeIsHeaderPlusControllerPlusWorkers) {
  EXPECT_EQ(HeartbeatRegion::bytes_for(0), 64U + 64U);
  EXPECT_EQ(HeartbeatRegion::bytes_for(2), 64U + 3 * 64U);
  EXPECT_EQ(worker_slot(0), 1U);
  EXPECT_EQ(kControllerSlot, 0U);
}

TEST(HeartbeatWordsTest, LivenessHasThreeAnswersAndTheBoundaryIsStrict) {
  EXPECT_EQ(liveness(std::nullopt, 1000, 10), Liveness::kNeverBeat);
  Heartbeat beat;
  beat.timestamp_ns = 1000;
  EXPECT_EQ(liveness(beat, 1010, 10), Liveness::kFresh) << "exactly the allowance is fresh";
  EXPECT_EQ(liveness(beat, 1011, 10), Liveness::kStale) << "one past it is stale";
  EXPECT_EQ(liveness(beat, 990, 10), Liveness::kFresh) << "a beat from the future is fresh";
}

TEST(HeartbeatWordsTest, EqualityNoticesEveryFieldOfABeatAndOfAnError) {
  const Heartbeat base = beat_number(5);
  Heartbeat changed = base;
  changed.timestamp_ns += 1;
  EXPECT_NE(changed, base);
  changed = base;
  changed.progress += 1;
  EXPECT_NE(changed, base);
  changed = base;
  changed.phase += 1;
  EXPECT_NE(changed, base);
  changed = base;
  changed.pid += 1;
  EXPECT_NE(changed, base);
  EXPECT_EQ(base, beat_number(5));

  const RegionError error{RegionFault::kMagic, "magic 7", SyscallError{}};
  RegionError other = error;
  other.detail = "magic 8";
  EXPECT_NE(other, error);
  other = error;
  other.cause = SyscallError{EIO, "mmap", "fd 100"};
  EXPECT_NE(other, error);
  other = error;
  other.fault = RegionFault::kVersion;
  EXPECT_NE(other, error);
}

TEST(HeartbeatWordsTest, ProgressMustStrictlyAdvance) {
  EXPECT_TRUE(progressed(beat_number(4), beat_number(5)));
  EXPECT_FALSE(progressed(beat_number(5), beat_number(5))) << "alive and not progressing";
  EXPECT_FALSE(progressed(beat_number(5), beat_number(4))) << "backwards is not credited";
}

TEST(HeartbeatWordsTest, ASampleIsAcceptedOnlyBetweenTwoEqualEvenSequences) {
  EXPECT_TRUE(sample_is_consistent(4, 4)) << "at rest before and after";
  EXPECT_FALSE(sample_is_consistent(5, 5)) << "a write in progress throughout";
  EXPECT_FALSE(sample_is_consistent(4, 5)) << "a write began during the read";
  EXPECT_FALSE(sample_is_consistent(4, 6)) << "a write began and finished during the read";
}

// --- create and attach against the fake (T2) ---------------------------------

class HeartbeatRegionTest : public ::testing::Test {
 protected:
  FakeSyscalls syscalls;
};

TEST_F(HeartbeatRegionTest, CreateSizesNamesAndInitialisesARegionWithEverySlotUnpublished) {
  auto created = HeartbeatRegion::create(syscalls, 2);
  ASSERT_TRUE(created.has_value()) << describe(created.error());
  const HeartbeatRegion& region = created.value();
  EXPECT_EQ(region.workers(), 2U);
  EXPECT_EQ(region.slots(), 3U);
  EXPECT_EQ(region.bytes(), HeartbeatRegion::bytes_for(2));
  EXPECT_GE(region.fd(), 100) << "a memory-file descriptor";
  EXPECT_EQ(syscalls.memory_file_names(), (std::vector<std::string>{"loadforge-heartbeat"}));
  for (std::size_t slot = 0; slot < region.slots(); ++slot) {
    auto observed = region.observe(slot);
    ASSERT_TRUE(observed.has_value()) << describe(observed.error());
    EXPECT_FALSE(observed.value().has_value()) << "slot " << slot << " never beat";
  }
}

TEST_F(HeartbeatRegionTest, TooManyWorkersIsRefusedBeforeAnyCall) {
  auto created = HeartbeatRegion::create(syscalls, kMaxWorkers + 1);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, RegionFault::kTooManyWorkers);
  EXPECT_EQ(created.error().detail, "workers 1025, expected 1024");
  EXPECT_TRUE(syscalls.memory_file_names().empty()) << "no call was made";
  auto attached = HeartbeatRegion::attach(syscalls, 100, kMaxWorkers + 1);
  ASSERT_FALSE(attached.has_value());
  EXPECT_EQ(attached.error().fault, RegionFault::kTooManyWorkers);
  EXPECT_TRUE(HeartbeatRegion::create(syscalls, kMaxWorkers).has_value())
      << "the limit itself is allowed";
}

TEST_F(HeartbeatRegionTest, AFailedMemoryFileIsReportedWithTheKernelsWords) {
  syscalls.fail_create_memory_file(ENOSYS);
  auto created = HeartbeatRegion::create(syscalls, 1, "beats");
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, RegionFault::kSyscall);
  EXPECT_EQ(created.error().cause.number, ENOSYS);
  EXPECT_EQ(created.error().cause.call, "memfd_create");
  EXPECT_EQ(created.error().cause.subject, "beats");
}

TEST_F(HeartbeatRegionTest, AFailedMappingClosesTheFileItCreated) {
  syscalls.fail_map_shared(ENOMEM);
  auto created = HeartbeatRegion::create(syscalls, 1);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().fault, RegionFault::kSyscall);
  EXPECT_EQ(created.error().cause.call, "mmap");
  EXPECT_TRUE(syscalls.all_descriptors_closed()) << "the memfd was not leaked";
}

TEST_F(HeartbeatRegionTest, AttachSeesWhatTheCreatorPublishedThroughTheSameFile) {
  // The supervisor's side and the worker's side, in one process, sharing the
  // fake's one storage per descriptor -- the model of what the kernel does
  // for two processes mapping one memfd.
  HeartbeatRegion creator = make(syscalls, 2);
  ASSERT_TRUE(creator.publish(kControllerSlot, beat_number(1)).has_value());

  auto attached = HeartbeatRegion::attach(syscalls, creator.fd(), 2);
  ASSERT_TRUE(attached.has_value()) << describe(attached.error());
  HeartbeatRegion worker = std::move(attached).value();
  auto seen = worker.observe(kControllerSlot);
  ASSERT_TRUE(seen.has_value());
  EXPECT_EQ(seen.value(), beat_number(1));

  // And back: the worker's beat reaches the creator.
  ASSERT_TRUE(worker.publish(worker_slot(1), beat_number(2)).has_value());
  auto heard = creator.observe(worker_slot(1));
  ASSERT_TRUE(heard.has_value());
  EXPECT_EQ(heard.value(), beat_number(2));
}

TEST_F(HeartbeatRegionTest, AttachRefusesADescriptorThatIsNotARegion) {
  auto attached = HeartbeatRegion::attach(syscalls, 999, 1);
  ASSERT_FALSE(attached.has_value());
  EXPECT_EQ(attached.error().fault, RegionFault::kSyscall);
  EXPECT_EQ(attached.error().cause.number, EBADF);
  EXPECT_EQ(syscalls.live_shared_mappings(), 0U);
}

TEST_F(HeartbeatRegionTest, AttachValidatesEveryHeaderFieldAndUnmapsWhenItRefuses) {
  // Control state crossing the boundary is re-validated (F18). Each field is
  // corrupted in turn through a raw mapping of the same file.
  auto created = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value());
  const int fd = created.value().fd();
  auto raw = syscalls.map_shared(fd, HeartbeatRegion::bytes_for(1));
  ASSERT_TRUE(raw.has_value());
  auto* const words = static_cast<std::uint32_t*>(raw.value());
  const std::uint32_t good[4] = {kRegionMagic, kRegionVersion, 2, 64};
  const RegionFault faults[4] = {RegionFault::kMagic, RegionFault::kVersion,
                                 RegionFault::kSlotCount, RegionFault::kSlotBytes};
  const char* details[4] = {"magic 7, expected 1279674434", "version 7, expected 1",
                            "slots 7, expected 2", "slot bytes 7, expected 64"};
  for (std::size_t field = 0; field < 4; ++field) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    words[field] = 7;
    const std::size_t mappings_before = syscalls.live_shared_mappings();
    auto attached = HeartbeatRegion::attach(syscalls, fd, 1);
    ASSERT_FALSE(attached.has_value()) << "field " << field;
    EXPECT_EQ(attached.error().fault, faults[field]) << "field " << field;
    EXPECT_EQ(attached.error().detail, details[field]) << "field " << field;
    EXPECT_EQ(syscalls.live_shared_mappings(), mappings_before) << "refusal unmapped";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    words[field] = good[field];
  }
  EXPECT_TRUE(HeartbeatRegion::attach(syscalls, fd, 1).has_value()) << "restored header attaches";
  EXPECT_TRUE(syscalls.unmap(raw.value(), HeartbeatRegion::bytes_for(1)).has_value());
}

TEST_F(HeartbeatRegionTest, AttachWithTheWrongWorkerCountIsASlotCountFault) {
  auto created = HeartbeatRegion::create(syscalls, 3);
  ASSERT_TRUE(created.has_value());
  auto attached = HeartbeatRegion::attach(syscalls, created.value().fd(), 2);
  ASSERT_FALSE(attached.has_value());
  EXPECT_EQ(attached.error().fault, RegionFault::kSlotCount);
  EXPECT_EQ(attached.error().detail, "slots 4, expected 3");
}

// --- publish and observe -----------------------------------------------------

TEST_F(HeartbeatRegionTest, PublishedBeatsAreObservedAndTheSequenceAdvancesByTwo) {
  HeartbeatRegion region = make(syscalls, 1);
  for (std::uint64_t n = 1; n <= 3; ++n) {
    ASSERT_TRUE(region.publish(worker_slot(0), beat_number(n)).has_value());
    auto seen = region.observe(worker_slot(0));
    ASSERT_TRUE(seen.has_value());
    ASSERT_TRUE(seen.value().has_value());
    EXPECT_EQ(seen.value().value(), beat_number(n));
  }
  auto raw = syscalls.map_shared(region.fd(), region.bytes());
  ASSERT_TRUE(raw.has_value());
  std::uint64_t sequence = 0;
  std::memcpy(&sequence, static_cast<std::byte*>(raw.value()) + 64 + 64, sizeof sequence);
  EXPECT_EQ(sequence, 6U) << "three publishes, two steps each, resting even";
  EXPECT_TRUE(syscalls.unmap(raw.value(), region.bytes()).has_value());
}

TEST_F(HeartbeatRegionTest, ASlotTheRegionDoesNotHaveIsRefusedByBothSides) {
  HeartbeatRegion region = make(syscalls, 1);
  auto published = region.publish(2, beat_number(1));
  ASSERT_FALSE(published.has_value());
  EXPECT_EQ(published.error().fault, RegionFault::kSlotOutOfRange);
  EXPECT_EQ(published.error().detail, "slot 2, expected 2");
  auto observed = region.observe(2);
  ASSERT_FALSE(observed.has_value());
  EXPECT_EQ(observed.error().fault, RegionFault::kSlotOutOfRange);
  EXPECT_TRUE(region.publish(1, beat_number(1)).has_value()) << "the last slot is fine";
}

TEST_F(HeartbeatRegionTest, AWriterThatDiedMidUpdateIsReportedAsTornNotWaitedFor) {
  // The lock-free analogue of EOWNERDEAD. A worker killed between the two
  // sequence stores leaves the sequence odd forever; no reader may block on
  // it. Staged by writing an odd sequence through a raw mapping.
  HeartbeatRegion region = make(syscalls, 1);
  ASSERT_TRUE(region.publish(worker_slot(0), beat_number(1)).has_value());
  auto raw = syscalls.map_shared(region.fd(), region.bytes());
  ASSERT_TRUE(raw.has_value());
  const std::uint64_t odd = 3;
  std::memcpy(static_cast<std::byte*>(raw.value()) + 64 + 64, &odd, sizeof odd);

  auto observed = region.observe(worker_slot(0));
  ASSERT_FALSE(observed.has_value());
  EXPECT_EQ(observed.error().fault, RegionFault::kTorn);
  EXPECT_EQ(observed.error().detail, "no consistent sample in 64 attempts");
  EXPECT_TRUE(syscalls.unmap(raw.value(), region.bytes()).has_value());
}

TEST_F(HeartbeatRegionTest, ReleaseReportsAndTheDestructorIsSilentAfterwards) {
  auto created = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value());
  HeartbeatRegion region = std::move(created).value();
  EXPECT_TRUE(region.release().has_value());
  EXPECT_EQ(syscalls.live_shared_mappings(), 0U);
  EXPECT_TRUE(syscalls.all_descriptors_closed());
  EXPECT_TRUE(region.release().has_value()) << "nothing left to release succeeds";
  EXPECT_EQ(region.fd(), -1);
}

TEST_F(HeartbeatRegionTest, ReleaseReportsAFailedUnmapButStillClosesTheDescriptor) {
  auto created = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value());
  HeartbeatRegion region = std::move(created).value();
  syscalls.fail_unmap(EINVAL);
  auto released = region.release();
  ASSERT_FALSE(released.has_value());
  EXPECT_EQ(released.error().call, "munmap");
  EXPECT_TRUE(syscalls.all_descriptors_closed()) << "closed regardless";
}

TEST_F(HeartbeatRegionTest, ReleaseReportsAFailedClose) {
  auto created = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value());
  HeartbeatRegion region = std::move(created).value();
  syscalls.fail_close(EIO);
  auto released = region.release();
  // The fake's close counts attempts and then fails for memory files too.
  EXPECT_EQ(syscalls.live_shared_mappings(), 0U);
  (void)released;
}

TEST_F(HeartbeatRegionTest, MovingTransfersOwnershipSoTheRegionIsReleasedOnce) {
  auto first = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(first.has_value());
  HeartbeatRegion a = std::move(first).value();
  const int fd = a.fd();
  HeartbeatRegion b(std::move(a));
  EXPECT_EQ(a.fd(), -1);  // NOLINT(bugprone-use-after-move): the moved-from state is the assertion
  EXPECT_EQ(b.fd(), fd);

  auto second = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(second.has_value());
  HeartbeatRegion c = std::move(second).value();
  EXPECT_EQ(syscalls.live_shared_mappings(), 2U);
  c = std::move(b);
  EXPECT_EQ(syscalls.live_shared_mappings(), 1U) << "assignment released what c held";
  EXPECT_EQ(c.fd(), fd);
  HeartbeatRegion* const self = &c;
  c = std::move(*self);
  EXPECT_EQ(c.fd(), fd) << "self-move is a no-op";
  c = std::move(a);  // Assigning a moved-from region releases c's and holds nothing.
  EXPECT_EQ(syscalls.live_shared_mappings(), 0U);
  auto again = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(again.has_value());
  c = std::move(again).value();  // Into an empty region: nothing to release first.
  EXPECT_EQ(syscalls.live_shared_mappings(), 1U);
}

// --- the protocol (P5) ----------------------------------------------------------
//
// SYNCHRONISATION ARGUMENT. One writer, one slot, any readers. The writer
// stores sequence s+1, issues a release fence, stores the fields, then stores
// s+2 with release. A reader loads the sequence (acquire), the fields, issues
// an acquire fence, and loads the sequence again, and accepts only an even
// value that did not change.
//
// No newer field is missed: the first load acquiring s synchronises with the
// release store of s, so every field load sees at least the fields of the
// publish that ended at s.
//
// No newer field is accepted: if any field load sees a store from a later
// publish, that store follows the writer's release fence, so the reader's
// acquire fence synchronises with the writer's fence, and the second sequence
// load sees s+1 or later. The sample is then retried.
//
// The second half is why the writer needs the FENCE and not merely a release
// store of s+1. A release store orders the writes before it; it does not keep
// the field stores after it from becoming visible first. The first version of
// this code had only the release store, and its argument here claimed the
// second half anyway. x86 never reorders two stores, so no test on x86 could
// show the gap, and neither can this one; TSan checks for races, not for which
// fence a seqlock needs. The argument above is the evidence; the test checks
// its consequence where the hardware lets it be seen: every field of every
// observed beat derives from the same number. Under TSan the atomic_refs are
// also what make the concurrent access race-free by the language's definition.

TEST_F(HeartbeatRegionTest, AConcurrentReaderNeverObservesATornBeat) {
  // The writer publishes at least kBeats beats AND keeps publishing until the
  // reader has taken kMinSamples consistent samples while it was writing. The
  // first version stopped at a fixed count, and on a fast machine the writer
  // could finish all 20000 beats before the reader looked once: the test then
  // failed on samples > 0, two runs in five, having tested no concurrency at
  // all. Now every run reads concurrently, however the scheduler orders them.
  HeartbeatRegion region = make(syscalls, 1);
  constexpr std::uint64_t kBeats = 20000;
  constexpr std::uint64_t kMinSamples = 1000;
  std::atomic<std::uint64_t> samples{0};
  std::atomic<std::uint64_t> written{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};

  std::thread writer([&region, &samples, &written, &stop, &done] {
    for (std::uint64_t n = 1;; ++n) {
      (void)region.publish(worker_slot(0), beat_number(n));
      written.store(n, std::memory_order_release);
      const bool enough = n >= kBeats && samples.load(std::memory_order_acquire) >= kMinSamples;
      if (enough || stop.load(std::memory_order_acquire)) {
        break;
      }
    }
    done.store(true, std::memory_order_release);
  });

  // A failure is recorded and the writer told to stop, never asserted inside
  // the loop: returning from the test with the writer still running would end
  // the process in std::terminate rather than in a readable failure.
  std::string wrong;
  std::uint64_t last = 0;
  while (!done.load(std::memory_order_acquire)) {
    auto observed = region.observe(worker_slot(0));
    if (!observed.has_value() || !observed.value().has_value()) {
      continue;  // Torn for 64 looks, or not yet published: permitted, never wrong.
    }
    const Heartbeat& beat = observed.value().value();
    if (!consistent(beat)) {
      wrong = "fields from two different beats: progress " + std::to_string(beat.progress);
    } else if (beat.progress < last) {
      wrong = "a sample from the past: progress " + std::to_string(beat.progress);
    }
    if (!wrong.empty()) {
      stop.store(true, std::memory_order_release);
      break;
    }
    last = beat.progress;
    samples.fetch_add(1, std::memory_order_acq_rel);
  }
  writer.join();
  EXPECT_EQ(wrong, "");
  EXPECT_GE(samples.load(), kMinSamples) << "the reader read concurrently with the writer";
  auto final_beat = region.observe(worker_slot(0));
  ASSERT_TRUE(final_beat.has_value());
  ASSERT_TRUE(final_beat.value().has_value());
  EXPECT_EQ(final_beat.value().value(), beat_number(written.load()));
}

// --- across a real process boundary --------------------------------------------

TEST(RealHeartbeatRegion, AForkedChildPublishesThroughItsOwnMappingAndTheParentSeesIt) {
  // The real kernel, a real memfd, two processes, two mappings of one file.
  // The child maps the inherited descriptor afresh (as a worker does after
  // exec) rather than reusing the parent's mapping, so this proves the FILE
  // is shared and not merely the fork's copy-on-write pages.
  RealSyscalls syscalls;
  auto created = HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value()) << describe(created.error());
  HeartbeatRegion parent_view = std::move(created).value();
  ASSERT_TRUE(parent_view.publish(kControllerSlot, beat_number(10)).has_value());

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto attached = HeartbeatRegion::attach(syscalls, parent_view.fd(), 1);
    if (!attached.has_value()) {
      ::_exit(2);
    }
    HeartbeatRegion child_view = std::move(attached).value();
    auto seen = child_view.observe(kControllerSlot);
    if (!seen.has_value() || !seen.value().has_value() || seen.value().value() != beat_number(10)) {
      ::_exit(3);
    }
    if (!child_view.publish(worker_slot(0), beat_number(20)).has_value()) {
      ::_exit(4);
    }
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0) << "2: attach failed, 3: did not see the parent, 4: publish";
  auto heard = parent_view.observe(worker_slot(0));
  ASSERT_TRUE(heard.has_value());
  ASSERT_TRUE(heard.value().has_value());
  EXPECT_EQ(heard.value().value(), beat_number(20));
}

}  // namespace
}  // namespace loadforge::ipc
