// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crash journal as a run uses it: what becomes of the previous run's, what
// is said about it, `loadforge journal`, and the readings an entry carries.

#include "main/journal_rig.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "controller/journal/file_ops.hpp"
#include "controller/journal/file_storage.hpp"
#include "telemetry/timeline/bounded_timeline.hpp"
#include "unit/main/memory_files.hpp"

namespace loadforge::cli {
namespace {

using cli_testing::MemoryFiles;

journal::Entry an_entry(std::int64_t at_ns, const std::string& phase) {
  journal::Entry entry;
  entry.elapsed_ns = at_ns;
  entry.phase = phase;
  entry.workload = "fft";
  entry.workers_alive = 4;
  return entry;
}

/// A journal of two entries, left as a run that was killed leaves it, or closed.
void leave_a_journal(MemoryFiles& files, const std::string& path, bool close) {
  auto storage = journal::FileStorage::open(files, path);
  ASSERT_TRUE(storage.has_value());
  auto writer = journal::open_writer(*storage.value());
  ASSERT_TRUE(writer.has_value());
  ASSERT_TRUE(writer.value()->append(an_entry(1'000'000'000, "r0 mixed")).has_value());
  ASSERT_TRUE(writer.value()->append(an_entry(6'000'000'000, "r1 move")).has_value());
  ASSERT_TRUE(writer.value()->sync().has_value());
  if (close) {
    ASSERT_TRUE(writer.value()->close().has_value());
  }
}

// --- in words ----------------------------------------------------------------------------

TEST(JournalWords, AnEntryNamesWhatItCarriesAndNothingElse) {
  journal::Entry bare = an_entry(61'500'000'000, "r3 move");
  bare.workers_stale = 1;
  EXPECT_EQ(render_entry(bare), "t=61.5s phase \"r3 move\" workload fft workers 4 alive 1 stale");
  journal::Entry full = bare;
  full.last_verified_unit = 1234;
  full.temperature_c = 81.25;
  full.package_power_w = 95.5;
  full.frequency_hz = 3.6e9;
  full.ecc_corrected = 3;
  full.ecc_uncorrected = 1;
  EXPECT_EQ(render_entry(full),
            "t=61.5s phase \"r3 move\" workload fft units 1234 81.2C 95.5W 3600.0MHz ecc corrected "
            "3 uncorrected 1 workers 4 alive 1 stale");
}

TEST(JournalWords, AReplayNamesHowTheJournalEndedAndItsLastEntryOrEveryEntry) {
  journal::Replay replay;
  replay.ending = journal::Ending::kUnterminated;
  EXPECT_EQ(render_replay(replay, "run.lfj", false),
            "journal run.lfj: ended abnormally between records; 0 entries");
  replay.entries = {an_entry(1'000'000'000, "r0 mixed")};
  EXPECT_EQ(render_replay(replay, "run.lfj", false),
            "journal run.lfj: ended abnormally between records; 1 entry; the last: t=1.0s phase "
            "\"r0 mixed\" workload fft workers 4 alive 0 stale");
  replay.entries.push_back(an_entry(2'000'000'000, "r1 move"));
  replay.ending = journal::Ending::kTerminated;
  EXPECT_EQ(render_replay(replay, "run.lfj", true),
            "journal run.lfj: ended cleanly; 2 entries\n"
            "  t=1.0s phase \"r0 mixed\" workload fft workers 4 alive 0 stale\n"
            "  t=2.0s phase \"r1 move\" workload fft workers 4 alive 0 stale\n");
}

// --- what becomes of the previous journal ---------------------------------------------------

TEST(RunJournal, WithNothingThereANewJournalIsStartedAndNothingIsSaid) {
  MemoryFiles files;
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_TRUE(opened.value().previous.empty());
  EXPECT_TRUE(files.files.contains("run.lfj"));
  ASSERT_TRUE(opened.value().writer->append(an_entry(1, "x")).has_value());
  EXPECT_FALSE(files.files["run.lfj"].empty());
  EXPECT_TRUE(files.renamed.empty());
  EXPECT_TRUE(files.removed.empty());
}

TEST(RunJournal, AnEmptyFileIsNoJournalAndIsUsed) {
  MemoryFiles files;
  files.files["run.lfj"];
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_TRUE(opened.value().previous.empty());
  EXPECT_TRUE(files.removed.empty());
}

TEST(RunJournal, AJournalThatEndedCleanlyIsSaidSoAndRemovedNotKept) {
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", true);
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_EQ(opened.value().previous,
            "previous journal run.lfj: ended cleanly; 2 entries; the last: t=6.0s phase \"r1 "
            "move\" workload fft workers 4 alive 0 stale");
  EXPECT_EQ(files.removed, (std::vector<std::string>{"run.lfj"}));
  EXPECT_TRUE(files.renamed.empty());
  EXPECT_TRUE(files.files["run.lfj"].empty()) << "the new journal is a new file";
}

TEST(RunJournal, AJournalThatDidNotEndCleanlyIsKeptBesideTheNewOneAndNeverOverwritten) {
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", false);
  auto first_opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(first_opened.has_value()) << first_opened.error();
  RunJournal first = std::move(first_opened).value();
  EXPECT_EQ(first.previous,
            "previous journal run.lfj: ended abnormally between records; 2 entries; the last: "
            "t=6.0s phase \"r1 move\" workload fft workers 4 alive 0 stale; kept as run.lfj.1");
  ASSERT_EQ(files.renamed.size(), 1U);
  EXPECT_EQ(files.renamed[0], (std::pair<std::string, std::string>{"run.lfj", "run.lfj.1"}));
  EXPECT_FALSE(files.files["run.lfj.1"].empty());
  EXPECT_TRUE(files.files["run.lfj"].empty());
  // A second abnormal ending: the first is not overwritten.
  first.writer.reset();
  first.storage.reset();
  leave_a_journal(files, "run.lfj.tmp", false);
  files.files["run.lfj"] = files.files["run.lfj.tmp"];
  files.files.erase("run.lfj.tmp");
  auto second = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_NE(second.value().previous.find("kept as run.lfj.2"), std::string::npos)
      << second.value().previous;
  EXPECT_TRUE(files.files.contains("run.lfj.1"));
  EXPECT_TRUE(files.files.contains("run.lfj.2"));
}

TEST(RunJournal, ATornJournalIsKeptToo) {
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", false);
  files.files["run.lfj"].resize(files.files["run.lfj"].size() - 5);  // The machine died mid-record.
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_NE(opened.value().previous.find("ended abnormally inside a record"), std::string::npos)
      << opened.value().previous;
  EXPECT_TRUE(files.files.contains("run.lfj.1"));
}

TEST(RunJournal, EveryStepThatCanFailSaysWhichAndWhy) {
  const auto refusal = [](MemoryFiles& files) {
    auto opened = open_run_journal(files, files, "run.lfj");
    EXPECT_FALSE(opened.has_value());
    return opened.has_value() ? std::string{} : opened.error();
  };
  {
    MemoryFiles files;
    files.fail_open_from(1, EACCES);
// --- review C6: a file that is not a journal is not moved; a journal in use is not shared -------

TEST(RunJournal, AFileThatIsNotAJournalIsRefusedAndLeftWhereItIs) {
  MemoryFiles files;
  const std::string text = "[run]\nmode = \"stress\"\nduration = \"10s\"\n";
  for (const char each : text) {
    files.files["config.toml"].push_back(static_cast<std::byte>(each));
  }
  const std::vector<std::byte> before = files.files["config.toml"];
  auto opened = open_run_journal(files, files, "config.toml");
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(),
            "config.toml exists and is not a journal (its first record does not verify); it is "
            "left as it is. Give --journal a path of its own, or move that file away");
  EXPECT_TRUE(files.renamed.empty());
  EXPECT_TRUE(files.removed.empty());
  EXPECT_EQ(files.files["config.toml"], before);
  EXPECT_FALSE(files.files.contains("config.toml.1"));
}

TEST(RunJournal, AJournalDamagedAfterItsFirstEntryIsStillEvidenceAndIsKept) {
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", false);
  std::vector<std::byte>& bytes = files.files["run.lfj"];
  bytes[bytes.size() - 3] = static_cast<std::byte>(~static_cast<unsigned>(bytes[bytes.size() - 3]));
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_NE(opened.value().previous.find("corrupt; 1 entry"), std::string::npos)
      << opened.value().previous;
  EXPECT_TRUE(files.files.contains("run.lfj.1"));
}

TEST(RunJournal, AJournalThatIsTornInsideItsFirstRecordIsKept) {
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", false);
  files.files["run.lfj"].resize(12);  // A header and a few bytes of the first payload.
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_NE(opened.value().previous.find("ended abnormally inside a record; 0 entries"),
            std::string::npos)
      << opened.value().previous;
  EXPECT_TRUE(files.files.contains("run.lfj.1"));
}

TEST(RunJournal, ASecondRunOnTheSamePathIsRefusedAndTouchesNothing) {
  MemoryFiles files;
  auto first = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_TRUE(first.value().writer->append(an_entry(1, "x")).has_value());
  ASSERT_TRUE(first.value().writer->sync().has_value());
  const std::vector<std::byte> written = files.files["run.lfj"];
  auto second = open_run_journal(files, files, "run.lfj");
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(),
            "the journal run.lfj is in use by another run; give this run a journal of its own, "
            "or wait for that one");
  EXPECT_TRUE(files.renamed.empty()) << "the first run's journal is not moved away";
  EXPECT_TRUE(files.removed.empty());
  EXPECT_EQ(files.files["run.lfj"], written);
  // When the first run is over, the path is free again.
  first = RunJournal{};
  auto third = open_run_journal(files, files, "run.lfj");
  EXPECT_TRUE(third.has_value()) << (third.has_value() ? "" : third.error());
}

TEST(RunJournal, ThePreviousJournalsLockIsHeldUntilTheNewOneIsLocked) {
  // Moving the old file away and making the new one is one step as far as another run is
  // concerned: a run that opened the old name early finds it still held.
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", false);
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_TRUE(opened.has_value()) << opened.error();
  EXPECT_EQ(files.lock_calls(), 2) << "the old file's, then the new file's";
  auto intruder = open_run_journal(files, files, "run.lfj");
  ASSERT_FALSE(intruder.has_value());
  EXPECT_NE(intruder.error().find("in use by another run"), std::string::npos) << intruder.error();
}

TEST(RunJournal, ALockThatCannotBeTakenIsRefusedWithTheKernelsReason) {
  for (const int error : {EBADF, EINVAL, ENOLCK, EINTR}) {
    MemoryFiles files;
    files.fail_lock(error);
    auto opened = open_run_journal(files, files, "run.lfj");
    ASSERT_FALSE(opened.has_value()) << error;
    EXPECT_NE(opened.error().find("cannot lock the journal run.lfj: flock("), std::string::npos)
        << opened.error();
    EXPECT_NE(opened.error().find("(errno " + std::to_string(error) + ")"), std::string::npos)
        << opened.error();
  }
}

TEST(RunJournal, ALockOnTheNewJournalThatCannotBeTakenIsRefusedToo) {
  MemoryFiles files;
  leave_a_journal(files, "run.lfj", false);
  files.fail_lock(ENOLCK, 2);
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_FALSE(opened.has_value());
  EXPECT_NE(opened.error().find("cannot lock the journal run.lfj: "), std::string::npos)
      << opened.error();
  EXPECT_TRUE(files.files.contains("run.lfj.1")) << "the evidence was kept before the new file";
}

    EXPECT_NE(refusal(files).find("cannot open the journal run.lfj: "), std::string::npos);
  }
  {
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", false);
    files.fail_read_from(1, EIO);  // The replay of what is there.
    EXPECT_NE(refusal(files).find("cannot read the journal run.lfj: "), std::string::npos);
  }
  {
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", true);
    files.remove_fails = true;
    EXPECT_EQ(refusal(files), "cannot remove run.lfj: scripted");
  }
  {
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", false);
    files.rename_fails = true;
    EXPECT_EQ(refusal(files), "cannot rename run.lfj to run.lfj.1: scripted");
  }
  {
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", false);
    files.always_exists = true;
    EXPECT_EQ(refusal(files),
              "there are already 1000 journals kept beside run.lfj; look at them and remove some");
  }
  {
    // The new file cannot be opened after the old was moved aside: the fourth open (the file's,
    // its directory's, the replay's, then the new file).
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", false);
    files.fail_open_from(4, EACCES);
    const std::string said = refusal(files);
    EXPECT_NE(said.find("cannot open the journal run.lfj: "), std::string::npos) << said;
  }
}

TEST(RunJournal, AStorageThatCannotBeReadTwiceIsNotAJournalToWrite) {
  // The replay reads once, and the writer reads once more to be sure it starts on nothing.
  MemoryFiles files;
  files.fail_read_from(2, EIO);
  auto opened = open_run_journal(files, files, "run.lfj");
  ASSERT_FALSE(opened.has_value());
  EXPECT_NE(opened.error().find("cannot start the journal run.lfj: "), std::string::npos)
      << opened.error();
}

TEST(RunJournal, TheRealFilesystemDoesWhatTheDoubleDoes) {
  const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                          ("loadforge-journal-rig-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory);
  const std::string path = (directory / "run.lfj").string();
  journal::RealFileOps ops;
  RealJournalFiles files;
  {
    auto first = open_run_journal(ops, files, path);
    ASSERT_TRUE(first.has_value()) << first.error();
    ASSERT_TRUE(first.value().writer->append(an_entry(1, "r0")).has_value());
    ASSERT_TRUE(first.value().writer->sync().has_value());
    // Not closed: a run killed here.
  }
  auto second = open_run_journal(ops, files, path);
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_NE(second.value().previous.find("ended abnormally between records; 1 entry"),
            std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(path + ".1"));
  EXPECT_TRUE(files.exists(path + ".1"));
  EXPECT_FALSE(files.exists(path + ".2"));
  auto shown = read_journal(ops, files, path + ".1");
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_EQ(shown.value().entries.size(), 1U);
  EXPECT_EQ(shown.value().ending, journal::Ending::kUnterminated);
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST(RealJournalFilesTest, EachFailureOfTheFilesystemIsSaidInTheKernelsWords) {
  const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                          ("loadforge-journal-files-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory / "full");
  std::ofstream(directory / "full" / "inside") << "x";
  RealJournalFiles files;
  const auto moved = files.rename((directory / "absent").string(), (directory / "to").string());
  ASSERT_FALSE(moved.has_value());
  EXPECT_NE(moved.error().find("cannot rename "), std::string::npos);
  EXPECT_NE(moved.error().find("No such file or directory"), std::string::npos) << moved.error();
  const auto removed = files.remove((directory / "full").string());
  ASSERT_FALSE(removed.has_value()) << "a directory with something in it";
  EXPECT_NE(removed.error().find("cannot remove "), std::string::npos);
  EXPECT_TRUE(files.remove((directory / "absent").string()).has_value())
      << "nothing there is nothing to remove";
  EXPECT_TRUE(files.exists((directory / "full").string()));
  EXPECT_FALSE(files.exists((directory / "absent").string()));
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

// --- loadforge journal ------------------------------------------------------------------------

TEST(ReadJournal, AJournalIsReplayedAndAMissingOneIsNotMade) {
  MemoryFiles files;
  auto missing = read_journal(files, files, "run.lfj");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error(), "there is no journal at run.lfj");
  EXPECT_FALSE(files.files.contains("run.lfj")) << "reading makes no file";

  leave_a_journal(files, "run.lfj", true);
  auto shown = read_journal(files, files, "run.lfj");
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_EQ(shown.value().entries.size(), 2U);
  EXPECT_EQ(shown.value().ending, journal::Ending::kTerminated);
}

TEST(ReadJournal, EachFailureToReadOneIsSaid) {
  {
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", true);
    files.fail_open_from(1, EACCES);
    auto shown = read_journal(files, files, "run.lfj");
    ASSERT_FALSE(shown.has_value());
    EXPECT_NE(shown.error().find("cannot open the journal run.lfj: "), std::string::npos);
  }
  {
    MemoryFiles files;
    leave_a_journal(files, "run.lfj", true);
    files.fail_read_from(1, EIO);
    auto shown = read_journal(files, files, "run.lfj");
    ASSERT_FALSE(shown.has_value());
    EXPECT_NE(shown.error().find("cannot read the journal run.lfj: "), std::string::npos);
  }
}

// --- what an entry carries of the machine -----------------------------------------------------

telemetry::Metric metric(const char* name, telemetry::Kind kind, std::uint32_t index,
                         std::optional<std::uint64_t> modulus = std::nullopt) {
  telemetry::Metric each;
  each.name = name;
  each.kind = kind;
  each.scope_index = index;
  each.modulus = modulus;
  each.unit =
      kind == telemetry::Kind::kGauge ? telemetry::Unit::kCelsius : telemetry::Unit::kMicrojoules;
  return each;
}

telemetry::Point gauge(std::int64_t at, double value) {
  return telemetry::Point{at, telemetry::Gauge{value}};
}
telemetry::Point count(std::int64_t at, std::uint64_t raw) {
  return telemetry::Point{at, telemetry::Counter{raw}};
}

TEST(TimelineNotesTest, TheHottestPackageThePowerOfAllTheMeanFrequencyAndTheEccTotals) {
  telemetry::BoundedTimeline timeline(1000);
  const auto add = [&timeline](const telemetry::Metric& each,
                               const std::vector<telemetry::Point>& points) {
    const auto id = timeline.add_series(each);
    EXPECT_TRUE(id.has_value());
    for (const telemetry::Point& point : points) {
      EXPECT_TRUE(timeline.append(id.value(), point).has_value());
    }
  };
  add(metric("thermal.package", telemetry::Kind::kGauge, 0), {gauge(1, 50.0), gauge(2, 70.5)});
  add(metric("thermal.package", telemetry::Kind::kGauge, 1), {gauge(1, 90.0), gauge(2, 64.0)});
  add(metric("thermal.core", telemetry::Kind::kGauge, 0), {gauge(2, 120.0)});
  // 4,000,000 microjoules in 1 s is 4 W; 6,000,000 in 2 s is 3 W: 7 W.
  add(metric("power.package", telemetry::Kind::kCounter, 0),
      {count(1'000'000'000, 1'000'000), count(2'000'000'000, 5'000'000)});
  add(metric("power.package", telemetry::Kind::kCounter, 1),
      {count(1'000'000'000, 0), count(3'000'000'000, 6'000'000)});
  add(metric("frequency.cpu", telemetry::Kind::kGauge, 0), {gauge(1, 3.0e9)});
  add(metric("frequency.cpu", telemetry::Kind::kGauge, 1), {gauge(1, 2.0e9)});
  add(metric("edac.corrected", telemetry::Kind::kCounter, 0), {count(1, 2)});
  add(metric("edac.corrected", telemetry::Kind::kCounter, 1), {count(1, 5)});
  add(metric("edac.uncorrected", telemetry::Kind::kCounter, 0), {count(1, 1)});
  TimelineNotes notes(timeline);
  journal::Entry entry = an_entry(1, "x");
  notes.fill(entry);
  EXPECT_EQ(entry.temperature_c, 70.5) << "the hottest of the latest reading of each package";
  ASSERT_TRUE(entry.package_power_w.has_value());
  EXPECT_NEAR(*entry.package_power_w, 7.0, 1e-9);
  EXPECT_NEAR(entry.frequency_hz.value(), 2.5e9, 1.0);
  EXPECT_EQ(entry.ecc_corrected, 7U);
  EXPECT_EQ(entry.ecc_uncorrected, 1U);
}

TEST(TimelineNotesTest, WhatIsNotThereOrCannotBeUsedLeavesTheFieldAbsent) {
  telemetry::BoundedTimeline timeline(1000);
  // A series with no points; a latest reading that is an absence; a counter that went back
  // (a reset: the advance across it is unknown); one reading only; and series that are none of
  // the entry's concern.
  const auto add = [&timeline](const telemetry::Metric& each,
                               const std::vector<telemetry::Point>& points) {
    const auto id = timeline.add_series(each);
    EXPECT_TRUE(id.has_value());
    for (const telemetry::Point& point : points) {
      EXPECT_TRUE(timeline.append(id.value(), point).has_value());
    }
  };
  add(metric("thermal.package", telemetry::Kind::kGauge, 0), {});
  add(metric("thermal.package", telemetry::Kind::kGauge, 1),
      {telemetry::Point{1, telemetry::Unavailable{}}});
  add(metric("power.package", telemetry::Kind::kCounter, 0),
      {count(1'000'000'000, 900), count(2'000'000'000, 100)});
  add(metric("power.package", telemetry::Kind::kCounter, 1), {count(1'000'000'000, 5)});
  add(metric("power.package", telemetry::Kind::kCounter, 2),
      {telemetry::Point{1, telemetry::Unavailable{}}, count(2, 3)});
  add(metric("power.package", telemetry::Kind::kCounter, 3),
      {count(1, 3), telemetry::Point{2, telemetry::Unavailable{}}});
  add(metric("frequency.cpu", telemetry::Kind::kGauge, 0), {});
  add(metric("edac.corrected", telemetry::Kind::kCounter, 0), {});
  add(metric("edac.uncorrected", telemetry::Kind::kCounter, 0),
      {telemetry::Point{1, telemetry::Unavailable{}}});
  add(metric("memory.available", telemetry::Kind::kGauge, 0), {gauge(1, 1.0)});
  TimelineNotes notes(timeline);
  journal::Entry entry = an_entry(1, "x");
  notes.fill(entry);
  EXPECT_FALSE(entry.temperature_c.has_value());
  EXPECT_FALSE(entry.package_power_w.has_value());
  EXPECT_FALSE(entry.frequency_hz.has_value());
  EXPECT_FALSE(entry.ecc_corrected.has_value());
  EXPECT_FALSE(entry.ecc_uncorrected.has_value());
}

/// A view of one series, whatever its points are: a timeline refuses two readings of one
/// instant, and the notes must still not divide by the nothing between them.
class OneSeries final : public telemetry::TimelineView {
 public:
  OneSeries(telemetry::Metric each, std::vector<telemetry::Point> points)
      : metric_(std::move(each)), points_(std::move(points)) {}
  [[nodiscard]] std::size_t series_count() const noexcept override { return 1; }
  [[nodiscard]] const telemetry::Metric& metric(telemetry::SeriesId /*series*/) const override {
    return metric_;
  }
  [[nodiscard]] std::span<const telemetry::Point> points(
      telemetry::SeriesId /*series*/) const override {
    return points_;
  }
  [[nodiscard]] std::span<const telemetry::PhaseMark> phases() const override { return {}; }

 private:
  telemetry::Metric metric_;
  std::vector<telemetry::Point> points_;
};

TEST(TimelineNotesTest, TwoReadingsOfOneInstantDrewNoPowerThatCanBeSaid) {
  const OneSeries view(metric("power.package", telemetry::Kind::kCounter, 0),
                       {count(1'000'000'000, 10), count(1'000'000'000, 40)});
  TimelineNotes notes(view);
  journal::Entry entry = an_entry(1, "x");
  notes.fill(entry);
  EXPECT_FALSE(entry.package_power_w.has_value());
}

TEST(TimelineNotesTest, AWrappingCounterCountsAcrossItsModulusAndTwoReadingsOfOneInstantAreNone) {
  telemetry::BoundedTimeline timeline(1000);
  const auto id =
      timeline.add_series(metric("power.package", telemetry::Kind::kCounter, 0, 1'000'000));
  ASSERT_TRUE(id.has_value());
  ASSERT_TRUE(timeline.append(id.value(), count(1'000'000'000, 900'000)).has_value());
  ASSERT_TRUE(timeline.append(id.value(), count(2'000'000'000, 100'000)).has_value());
  TimelineNotes notes(timeline);
  journal::Entry entry = an_entry(1, "x");
  notes.fill(entry);
  ASSERT_TRUE(entry.package_power_w.has_value());
  EXPECT_NEAR(*entry.package_power_w, 0.2, 1e-9) << "200,000 microjoules in a second";
}

}  // namespace
}  // namespace loadforge::cli
