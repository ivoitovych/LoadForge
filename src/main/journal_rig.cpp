// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/journal_rig.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "controller/journal/file_storage.hpp"
#include "core/result.hpp"
#include "telemetry/sample.hpp"

namespace loadforge::cli {

bool RealJournalFiles::exists(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
}

core::Result<core::Ok, std::string> RealJournalFiles::rename(const std::string& from,
                                                             const std::string& to) {
  std::error_code failed;
  std::filesystem::rename(from, to, failed);
  if (failed) {
    return "cannot rename " + from + " to " + to + ": " + failed.message();
  }
  return core::Ok{};
}

core::Result<core::Ok, std::string> RealJournalFiles::remove(const std::string& path) {
  std::error_code failed;
  std::filesystem::remove(path, failed);
  if (failed) {
    return "cannot remove " + path + ": " + failed.message();
  }
  return core::Ok{};
}

namespace {

constexpr double kNanosecondsPerSecond = 1e9;

/// A number to one decimal place.
std::string one_place(double value) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(1) << value;
  return text.str();
}

}  // namespace

std::string render_entry(const journal::Entry& entry) {
  std::string text = "t=" + one_place(static_cast<double>(entry.elapsed_ns) / kNanosecondsPerSecond) +
                     "s phase \"" + entry.phase + "\" workload " + entry.workload;
  if (entry.last_verified_unit.has_value()) {
    text += " units " + std::to_string(*entry.last_verified_unit);
  }
  if (entry.temperature_c.has_value()) {
    text += " " + one_place(*entry.temperature_c) + "C";
  }
  if (entry.package_power_w.has_value()) {
    text += " " + one_place(*entry.package_power_w) + "W";
  }
  if (entry.frequency_hz.has_value()) {
    text += " " + one_place(*entry.frequency_hz / 1e6) + "MHz";
  }
  if (entry.ecc_corrected.has_value()) {
    text += " ecc corrected " + std::to_string(*entry.ecc_corrected);
  }
  if (entry.ecc_uncorrected.has_value()) {
    text += " uncorrected " + std::to_string(*entry.ecc_uncorrected);
  }
  text += " workers " + std::to_string(entry.workers_alive) + " alive " +
          std::to_string(entry.workers_stale) + " stale";
  return text;
}

std::string render_replay(const journal::Replay& replay, std::string_view path, bool all) {
  std::string text = "journal ";
  text += path;
  text += ": ";
  text += journal::describe(replay.ending);
  text += "; ";
  text += std::to_string(replay.entries.size());
  text += " entr";
  text += replay.entries.size() == 1 ? "y" : "ies";
  if (all) {
    text += "\n";
    for (const journal::Entry& entry : replay.entries) {
      text += "  " + render_entry(entry) + "\n";
    }
    return text;
  }
  if (!replay.entries.empty()) {
    text += "; the last: " + render_entry(replay.entries.back());
  }
  return text;
}

core::Result<RunJournal, std::string> open_run_journal(journal::FileOps& ops, JournalFiles& files,
                                                       const std::string& path) {
  auto first = journal::FileStorage::open(ops, path);
  if (!first.has_value()) {
    return "cannot open the journal " + path + ": " + first.error().detail;
  }
  auto replayed = journal::replay(*first.value());
  if (!replayed.has_value()) {
    return "cannot read the journal " + path + ": " + replayed.error().detail;
  }
  RunJournal opened;
  std::unique_ptr<journal::FileStorage> storage = std::move(first).value();
  if (replayed.value().ending != journal::Ending::kEmpty) {
    opened.previous = "previous " + render_replay(replayed.value(), path, false);
    storage.reset();  // Closed before the file is moved or removed.
    if (replayed.value().ending == journal::Ending::kTerminated) {
      if (auto removed = files.remove(path); !removed.has_value()) {
        return removed.error();
      }
    } else {
      int number = 1;
      while (number <= kMostKeptJournals && files.exists(path + "." + std::to_string(number))) {
        ++number;
      }
      if (number > kMostKeptJournals) {
        return "there are already " + std::to_string(kMostKeptJournals) + " journals kept beside " +
               path + "; look at them and remove some";
      }
      const std::string kept = path + "." + std::to_string(number);
      if (auto moved = files.rename(path, kept); !moved.has_value()) {
        return moved.error();
      }
      opened.previous += "; kept as " + kept;
    }
    auto fresh = journal::FileStorage::open(ops, path);
    if (!fresh.has_value()) {
      return "cannot open the journal " + path + ": " + fresh.error().detail;
    }
    storage = std::move(fresh).value();
  }
  opened.storage = std::move(storage);
  auto writer = journal::open_writer(*opened.storage);
  if (!writer.has_value()) {
    return "cannot start the journal " + path + ": " + writer.error().detail;
  }
  opened.writer = std::move(writer).value();
  return opened;
}

core::Result<journal::Replay, std::string> read_journal(journal::FileOps& ops, JournalFiles& files,
                                                        const std::string& path) {
  if (!files.exists(path)) {
    return "there is no journal at " + path;
  }
  auto storage = journal::FileStorage::open(ops, path);
  if (!storage.has_value()) {
    return "cannot open the journal " + path + ": " + storage.error().detail;
  }
  auto replayed = journal::replay(*storage.value());
  if (!replayed.has_value()) {
    return "cannot read the journal " + path + ": " + replayed.error().detail;
  }
  return std::move(replayed).value();
}

namespace {

/// What the latest reading of a series is, if it is a value.
template <typename Kind>
const Kind* latest_of(const telemetry::TimelineView& timeline, telemetry::SeriesId series) {
  const auto points = timeline.points(series);
  return points.empty() ? nullptr : std::get_if<Kind>(&points.back().reading);
}

/// The watts a package drew between its last two readings, if both are counts
/// and the counter's advance across them is known.
std::optional<double> watts_between(const telemetry::TimelineView& timeline,
                                    telemetry::SeriesId series) {
  const auto points = timeline.points(series);
  if (points.size() < 2) {
    return std::nullopt;
  }
  const telemetry::Point& before = points[points.size() - 2];
  const telemetry::Point& after = points.back();
  const auto* from = std::get_if<telemetry::Counter>(&before.reading);
  const auto* to = std::get_if<telemetry::Counter>(&after.reading);
  if (from == nullptr || to == nullptr) {
    return std::nullopt;
  }
  const auto advance =
      telemetry::counter_advance(from->raw, to->raw, timeline.metric(series).modulus);
  const std::int64_t span_ns = after.timestamp_ns - before.timestamp_ns;
  if (!advance.has_value() || span_ns <= 0) {
    return std::nullopt;
  }
  // Microjoules over nanoseconds: 10^-6 J / 10^-9 s = 10^3 W.
  return static_cast<double>(*advance) / static_cast<double>(span_ns) * 1e3;
}

void add(std::optional<std::uint64_t>& total, std::uint64_t more) {
  total = total.value_or(0) + more;
}

}  // namespace

void TimelineNotes::fill(journal::Entry& entry) {
  const telemetry::TimelineView& timeline = *timeline_;
  double frequency_sum = 0.0;
  std::size_t frequencies = 0;
  for (telemetry::SeriesId series = 0; series < timeline.series_count(); ++series) {
    const std::string& name = timeline.metric(series).name;
    if (name == "thermal.package") {
      if (const auto* gauge = latest_of<telemetry::Gauge>(timeline, series)) {
        entry.temperature_c = std::max(entry.temperature_c.value_or(gauge->value), gauge->value);
      }
    } else if (name == "power.package") {
      if (const auto watts = watts_between(timeline, series)) {
        entry.package_power_w = entry.package_power_w.value_or(0.0) + *watts;
      }
    } else if (name == "frequency.cpu") {
      if (const auto* gauge = latest_of<telemetry::Gauge>(timeline, series)) {
        frequency_sum += gauge->value;
        ++frequencies;
      }
    } else if (name == "edac.corrected") {
      if (const auto* count = latest_of<telemetry::Counter>(timeline, series)) {
        add(entry.ecc_corrected, count->raw);
      }
    } else if (name == "edac.uncorrected") {
      if (const auto* count = latest_of<telemetry::Counter>(timeline, series)) {
        add(entry.ecc_uncorrected, count->raw);
      }
    }
  }
  if (frequencies > 0) {
    entry.frequency_hz = frequency_sum / static_cast<double>(frequencies);
  }
}

}  // namespace loadforge::cli
