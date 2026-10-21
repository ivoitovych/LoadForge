// SPDX-License-Identifier: GPL-3.0-or-later
//
// T10: the journal across a long run. The controller appends an entry every
// five seconds and syncs it; a run of many hours is some thousands of entries.
// Two clocks exist here, and the test keeps them apart:
//
//   * the entries carry the run's clock, five seconds apart, so the journal
//     the test writes is the journal of a run of that length;
//   * the test's own pace is LOADFORGE_SOAK_INTERVAL_MS between appends,
//     zero by default so a run of the whole suite stays short, and 5000 in the
//     nightly soak that really takes the hours.
//
// LOADFORGE_SOAK_ENTRIES sets how many (default 4320: six hours at 5 s).
//
// What must not grow: the process's resident memory, its descriptors, and its
// heap beyond the entry in flight. What must grow by exactly what was
// appended: the file.

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "controller/journal/file_storage.hpp"
#include "controller/journal/format.hpp"
#include "controller/journal/journal.hpp"

namespace loadforge::journal {
namespace {

constexpr std::int64_t kIntervalNs = 5'000'000'000;

std::uint64_t env_number(const char* name, std::uint64_t fallback) {
  const char* text = std::getenv(name);
  return text == nullptr ? fallback : std::strtoull(text, nullptr, 10);
}

// AddressSanitizer parks freed blocks in a quarantine of up to 256 MiB, so
// resident memory climbs under it whatever the code does; there the leak
// check is LeakSanitizer's, at exit, and the resident-memory bound is not
// asserted. ThreadSanitizer likewise: every heap page the run touches also
// touches its shadow and metadata, which count in this process's resident set,
// so the 256 KiB slack is exceeded or not depending on where the allocator
// lands (failed once in CI, 2026-10-21, on #77; passed 5 of 5 locally). The
// plain and coverage builds still assert the bound, and the ASan build's leak
// check still covers what a growing resident set would mean.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kQuarantine = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kQuarantine = true;
#else
constexpr bool kQuarantine = false;
#endif
#else
constexpr bool kQuarantine = false;
#endif

std::uint64_t resident_bytes() {
  std::ifstream statm("/proc/self/statm");
  std::uint64_t size = 0;
  std::uint64_t resident = 0;
  statm >> size >> resident;
  return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
}

std::size_t open_descriptors() {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    static_cast<void>(entry);
    ++count;
  }
  return count;
}

Entry entry_at(std::uint64_t index) {
  Entry entry;
  entry.elapsed_ns = static_cast<std::int64_t>(index) * kIntervalNs;
  entry.phase = index % 2 == 0 ? "stress" : "cooldown";
  entry.workload = "mixed";
  entry.last_verified_unit = index * 1000;
  entry.temperature_c = 55.0 + static_cast<double>(index % 40);
  entry.package_power_w = 90.0;
  entry.frequency_hz = 3.6e9;
  entry.ecc_corrected = index / 100;
  entry.ecc_uncorrected = 0;
  entry.workers_alive = 8;
  return entry;
}

TEST(JournalSoak, AppendingForHoursGrowsTheFileAndNothingElse) {
  const std::uint64_t entries = env_number("LOADFORGE_SOAK_ENTRIES", 4320);
  const auto pace = std::chrono::milliseconds(env_number("LOADFORGE_SOAK_INTERVAL_MS", 0));
  std::string pattern = (std::filesystem::temp_directory_path() / "loadforge-soak-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const std::string file = (directory / "run.journal").string();

  RealFileOps ops;
  auto storage = FileStorage::open(ops, file);
  ASSERT_TRUE(storage.has_value());
  auto writer = open_writer(*storage.value());
  ASSERT_TRUE(writer.has_value());

  const std::uint64_t warm = entries / 10;
  std::uint64_t resident_warm = 0;
  std::size_t descriptors_warm = 0;
  std::uint64_t appended_bytes = 0;
  for (std::uint64_t index = 0; index < entries; ++index) {
    const Entry entry = entry_at(index);
    appended_bytes += format::kHeaderBytes + format::encode_entry(entry).size();
    ASSERT_TRUE(writer.value()->append(entry).has_value()) << index;
    ASSERT_TRUE(writer.value()->sync().has_value()) << index;
    if (index == warm) {
      // The descriptors are counted first: listing /proc/self/fd opens a
      // directory stream and allocates for it, and under ThreadSanitizer
      // that first use alone costs about 200 KiB of resident memory.
      // Counted after the resident sample, it landed in the growth the bound
      // is there to find and failed it by a few KiB; counted before, it is
      // part of the baseline, as it is of the measurement at the end.
      descriptors_warm = open_descriptors();
      resident_warm = resident_bytes();
    }
    if (pace.count() > 0) {
      std::this_thread::sleep_for(pace);
    }
  }
  ASSERT_TRUE(writer.value()->close().has_value());
  appended_bytes += format::kHeaderBytes + format::terminator_payload().size();

  EXPECT_EQ(std::filesystem::file_size(file), appended_bytes)
      << "the file grew by exactly what was appended";
  EXPECT_EQ(open_descriptors(), descriptors_warm) << "no descriptor per append";
  // 256 KiB of slack for the allocator and the page cache's accounting;
  // a leak of even one entry per append is 100 bytes x thousands, far above.
  constexpr std::uint64_t kSlack = std::uint64_t{256} * 1024;
  EXPECT_TRUE(kQuarantine || resident_bytes() <= resident_warm + kSlack)
      << "resident memory after " << entries << " appends against " << warm << " warm";

  const Replay replayed = replay(*storage.value()).value();
  ASSERT_EQ(replayed.entries.size(), entries);
  EXPECT_EQ(replayed.entries.front(), entry_at(0));
  EXPECT_EQ(replayed.entries.back(), entry_at(entries - 1));
  EXPECT_EQ(replayed.ending, Ending::kTerminated);
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace loadforge::journal
