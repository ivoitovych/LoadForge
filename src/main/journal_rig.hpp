// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_JOURNAL_RIG_HPP
#define LOADFORGE_MAIN_JOURNAL_RIG_HPP

// The crash journal as a run uses it (docs/PLAN.md F16): `stress --journal
// FILE` replays what the previous run left at FILE, says how it ended, keeps
// it if it ended abnormally, and starts a new journal of this run there; and
// `loadforge journal FILE` reads one and prints it. The format, the writer and
// the replay are the journal stream's (controller/journal/); this is the
// housekeeping around them and the readings of the timeline an entry carries.
//
// WHAT HAPPENS TO THE PREVIOUS JOURNAL
// ------------------------------------
// A journal is written by one run and never continued (journal.hpp, kNotEmpty).
// So at the start of a run the file is replayed first. One that ended cleanly
// has nothing more to say than that it did, and is removed. One that did not --
// a run killed, a machine that lost power, a record torn by the death, a
// journal that does not verify -- is the evidence the journal exists for, and is
// kept: renamed to FILE.1, or FILE.2 if that is taken, and so on, never
// overwritten. The new journal is FILE.
//
// Two things are refused before anything is moved (review C6). A file that is
// not a journal -- its first record does not verify -- is left where it is: it
// is not evidence of a crash, and renaming a user's file to FILE.1 is not a
// thing a journal path may do. And the journal file is locked (flock, without
// waiting) for as long as the run holds it, so a second run on the same path is
// refused instead of moving the first one's journal away under it.

#include <memory>
#include <string>
#include <string_view>

#include "controller/journal/file_ops.hpp"
#include "controller/journal/journal.hpp"
#include "controller/orchestration.hpp"
#include "core/result.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::cli {

/// The file operations around a journal that FileOps does not have: whether a
/// name is taken, moving a file, removing one. A seam so that each failure is
/// reachable by a test that makes it.
class JournalFiles {
 public:
  JournalFiles() = default;
  JournalFiles(const JournalFiles&) = delete;
  JournalFiles(JournalFiles&&) = delete;
  JournalFiles& operator=(const JournalFiles&) = delete;
  JournalFiles& operator=(JournalFiles&&) = delete;
  virtual ~JournalFiles() = default;

  [[nodiscard]] virtual bool exists(const std::string& path) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, std::string> rename(const std::string& from,
                                                                   const std::string& to) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, std::string> remove(const std::string& path) = 0;
};

/// The filesystem.
class RealJournalFiles final : public JournalFiles {
 public:
  [[nodiscard]] bool exists(const std::string& path) override;
  [[nodiscard]] core::Result<core::Ok, std::string> rename(const std::string& from,
                                                           const std::string& to) override;
  [[nodiscard]] core::Result<core::Ok, std::string> remove(const std::string& path) override;
};

/// How many journals kept beside FILE (FILE.1 ... FILE.n) a run will count
/// past before it says there are too many: a directory full of them is a
/// problem to be looked at, not a name to be found.
inline constexpr int kMostKeptJournals = 1000;

/// One entry in words: its time, phase and workload, and every reading and
/// count it carries; what it does not carry is not mentioned.
[[nodiscard]] std::string render_entry(const journal::Entry& entry);

/// What a replay recovered, in words: how the journal ended and the last
/// entry; with `all`, every entry, one to a line.
[[nodiscard]] std::string render_replay(const journal::Replay& replay, std::string_view path,
                                        bool all);

/// A journal opened for a run: the storage the writer appends to, the writer,
/// and what the previous journal at the path said (empty if it held none).
struct RunJournal {
  std::unique_ptr<journal::Storage> storage;
  std::unique_ptr<journal::Writer> writer;
  std::string previous;
};

/// The journal at `path` for a run to write: see the header's account of what
/// becomes of one already there. The refusal says which step failed and why.
[[nodiscard]] core::Result<RunJournal, std::string> open_run_journal(journal::FileOps& ops,
                                                                     JournalFiles& files,
                                                                     const std::string& path);

/// `loadforge journal FILE`: the journal at `path`, replayed; a path with no
/// file is refused, and no file is made.
[[nodiscard]] core::Result<journal::Replay, std::string> read_journal(journal::FileOps& ops,
                                                                      JournalFiles& files,
                                                                      const std::string& path);

/// What a journal entry carries of the machine, read from the timeline the
/// sampler fills: the hottest package, the packages' power over their last two
/// readings, the mean frequency, and the machine's ECC totals; each absent
/// when no series of it has a reading.
class TimelineNotes final : public controller::JournalNotes {
 public:
  explicit TimelineNotes(const telemetry::TimelineView& timeline) noexcept : timeline_(&timeline) {}

  void fill(journal::Entry& entry) override;

 private:
  const telemetry::TimelineView* timeline_;
};

}  // namespace loadforge::cli

#endif
