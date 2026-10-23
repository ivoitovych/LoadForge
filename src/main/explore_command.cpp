// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/explore_command.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/parse_number.hpp"
#include "explore/estimate.hpp"
#include "main/cli.hpp"
#include "main/outcome.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "telemetry/capability/capability.hpp"
#include "topology/cpu_list.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {

namespace {

constexpr int kOk = 0;
constexpr int kRefused = 1;
constexpr int kUsageError = 2;
constexpr int kTrialFailed = 3;

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// The largest elimination factor: a factor above the number of candidates keeps one,
/// so a thousand is far beyond any search, and the arithmetic stays far from wrapping.
constexpr std::uint64_t kMostElimination = 1024;
/// Thirty days, as the trial runner's bound on a load.
constexpr std::int64_t kLongestMilliseconds = std::int64_t{30} * 24 * 3600 * 1000;

std::string refused(std::string_view key, const std::string& what) {
  std::string text{key};
  text += ": ";
  text += what;
  return text;
}

/// An option as it was written: its key and the value after it.
struct Written {
  std::string_view key;
  std::string_view value;
};

/// What parsing builds: the arguments, and the settle phase's parts, which may
/// come in any order and are put together at the end.
struct Parsing {
  ExploreArgs args;
  bool objective_given = false;
  std::optional<std::string> settle_metric;
  std::optional<double> settle_slope;
  std::optional<std::int64_t> settle_window_ns;
  std::optional<std::int64_t> settle_max_ns;
};

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t from = 0;
  for (;;) {
    const std::size_t at = text.find(separator, from);
    if (at == std::string_view::npos) {
      parts.push_back(text.substr(from));
      return parts;
    }
    parts.push_back(text.substr(from, at - from));
    from = at + 1;
  }
}

core::Result<std::uint64_t, std::string> whole_number(const Written& option) {
  auto split_text = core::split_number_and_suffix(option.value);
  if (!split_text.has_value() || !split_text.value().suffix.empty()) {
    std::string what = "expected a whole number, got '";
    what += option.value;
    what += "'";
    return refused(option.key, what);
  }
  return split_text.value().value;
}

core::Result<std::uint64_t, std::string> at_least_one(const Written& option) {
  auto count = whole_number(option);
  if (!count.has_value()) {
    return count;
  }
  if (count.value() == 0) {
    return refused(option.key, "must be at least 1");
  }
  return count;
}

/// A length of time above zero that a trial can have, in nanoseconds.
core::Result<std::int64_t, std::string> positive_ns(const Written& option) {
  auto duration = core::parse_duration(option.value);
  if (!duration.has_value()) {
    return refused(option.key, std::string{core::describe(duration.error())});
  }
  if (duration.value().count() <= 0 || duration.value().count() > kLongestMilliseconds) {
    return refused(option.key, "must be above zero and at most 30 days");
  }
  return duration.value().count() * kNanosecondsPerMillisecond;
}

// --- the objective ---------------------------------------------------------------

struct StatisticName {
  std::string_view name;
  std::variant<explore::GaugeStatistic, explore::CounterStatistic> statistic;
};

constexpr std::array<StatisticName, 8> kStatistics{{
    {"mean", explore::GaugeStatistic::kMean},
    {"max", explore::GaugeStatistic::kMax},
    {"min", explore::GaugeStatistic::kMin},
    {"last", explore::GaugeStatistic::kLast},
    {"stddev", explore::GaugeStatistic::kStddev},
    {"slope", explore::GaugeStatistic::kSlope},
    {"rate", explore::CounterStatistic::kRate},
    {"delta", explore::CounterStatistic::kDelta},
}};

std::optional<std::string> take_objective(const Written& option, Parsing& out) {
  const std::vector<std::string_view> parts = split(option.value, ':');
  if (parts.size() != 3) {
    return refused(option.key, "expected METRIC[@INDEX]:STATISTIC:higher|lower");
  }
  explore::Objective objective;
  const std::vector<std::string_view> named = split(parts[0], '@');
  if (named.size() > 2 || named[0].empty()) {
    return refused(option.key, "the metric is NAME or NAME@INDEX");
  }
  objective.metric = std::string{named[0]};
  if (named.size() == 2) {
    auto index = whole_number(Written{option.key, named[1]});
    if (!index.has_value()) {
      return index.error();
    }
    if (index.value() > std::numeric_limits<std::uint32_t>::max()) {
      return refused(option.key, "the scope index is too large");
    }
    objective.scope_index = static_cast<std::uint32_t>(index.value());
  }
  const auto* const statistic = std::ranges::find(kStatistics, parts[1], &StatisticName::name);
  if (statistic == kStatistics.end()) {
    return refused(option.key, "'" + std::string{parts[1]} +
                                   "' is none of mean, max, min, last, stddev, slope, rate, delta");
  }
  objective.statistic = statistic->statistic;
  if (parts[2] == "higher") {
    objective.direction = explore::Direction::kHigherIsWorse;
  } else if (parts[2] == "lower") {
    objective.direction = explore::Direction::kLowerIsWorse;
  } else {
    return refused(option.key, "'" + std::string{parts[2]} + "' is neither higher nor lower");
  }
  out.args.objective = std::move(objective);
  out.objective_given = true;
  return std::nullopt;
}

// --- candidates ------------------------------------------------------------------

core::Result<explore::Candidate, std::string> candidate_of(const Written& option) {
  const std::vector<std::string_view> parts = split(option.value, ':');
  if (parts.size() < 3 || parts.size() > 4) {
    return refused(option.key, "expected ID:WORKLOAD[+WORKLOAD...]:THREADS[:CPUS]");
  }
  explore::Candidate candidate;
  if (parts[0].empty()) {
    return refused(option.key, "a candidate needs an id");
  }
  candidate.id = std::string{parts[0]};
  for (const std::string_view workload : split(parts[1], '+')) {
    if (workload.empty()) {
      return refused(option.key, "an empty workload name");
    }
    candidate.workloads.emplace_back(workload);
  }
  auto threads = at_least_one(Written{option.key, parts[2]});
  if (!threads.has_value()) {
    return threads.error();
  }
  if (threads.value() > std::numeric_limits<std::uint32_t>::max()) {
    return refused(option.key, "too many threads");
  }
  candidate.threads = static_cast<std::uint32_t>(threads.value());
  if (parts.size() == 4) {
    auto cpus = topology::CpuList::parse(parts[3]);
    if (!cpus.has_value() || cpus.value().empty()) {
      return refused(option.key,
                     "'" + std::string{parts[3]} + "' is not a list of CPUs like 0-3,6");
    }
    candidate.cpus = cpus.value().ids();
  }
  return candidate;
}

std::optional<std::string> take_candidate(const Written& option, Parsing& out) {
  auto candidate = candidate_of(option);
  if (!candidate.has_value()) {
    return candidate.error();
  }
  out.args.candidates.push_back(std::move(candidate).value());
  return std::nullopt;
}

std::optional<std::string> take_reference(const Written& option, Parsing& out) {
  auto candidate = candidate_of(option);
  if (!candidate.has_value()) {
    return candidate.error();
  }
  out.args.reference = std::move(candidate).value();
  return std::nullopt;
}

// --- the rest --------------------------------------------------------------------

std::optional<std::string> take_duration(const Written& option, Parsing& out) {
  auto length = positive_ns(option);
  if (!length.has_value()) {
    return length.error();
  }
  out.args.duration_ns = length.value();
  return std::nullopt;
}

std::optional<std::string> take_settle(const Written& option, Parsing& out) {
  out.settle_metric = std::string{option.value};
  return std::nullopt;
}

std::optional<std::string> take_settle_slope(const Written& option, Parsing& out) {
  double slope = 0.0;
  const char* const end = option.value.data() + option.value.size();
  const auto parsed = std::from_chars(option.value.data(), end, slope);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(slope) || slope < 0.0) {
    return refused(option.key,
                   "expected a number of 0 or more, got '" + std::string{option.value} + "'");
  }
  out.settle_slope = slope;
  return std::nullopt;
}

std::optional<std::string> take_settle_window(const Written& option, Parsing& out) {
  auto length = positive_ns(option);
  if (!length.has_value()) {
    return length.error();
  }
  out.settle_window_ns = length.value();
  return std::nullopt;
}

std::optional<std::string> take_settle_max(const Written& option, Parsing& out) {
  auto length = positive_ns(option);
  if (!length.has_value()) {
    return length.error();
  }
  out.settle_max_ns = length.value();
  return std::nullopt;
}

std::optional<std::string> take_replicates(const Written& option, Parsing& out) {
  auto count = at_least_one(option);
  if (!count.has_value()) {
    return count.error();
  }
  out.args.base_replicates = static_cast<std::size_t>(count.value());
  return std::nullopt;
}

std::optional<std::string> take_final_replicates(const Written& option, Parsing& out) {
  auto count = at_least_one(option);
  if (!count.has_value()) {
    return count.error();
  }
  out.args.final_replicates = static_cast<std::size_t>(count.value());
  return std::nullopt;
}

std::optional<std::string> take_elimination(const Written& option, Parsing& out) {
  auto count = at_least_one(option);
  if (!count.has_value()) {
    return count.error();
  }
  if (count.value() > kMostElimination) {
    return refused(option.key, "must be at most " + std::to_string(kMostElimination));
  }
  out.args.elimination_factor = static_cast<std::size_t>(count.value());
  return std::nullopt;
}

std::optional<std::string> take_interleave(const Written& option, Parsing& out) {
  auto count = at_least_one(option);
  if (!count.has_value()) {
    return count.error();
  }
  out.args.interleave_every = static_cast<std::size_t>(count.value());
  return std::nullopt;
}

std::optional<std::string> take_reference_replicates(const Written& option, Parsing& out) {
  auto count = at_least_one(option);
  if (!count.has_value()) {
    return count.error();
  }
  out.args.reference_replicates = static_cast<std::size_t>(count.value());
  return std::nullopt;
}

std::optional<std::string> take_seed(const Written& option, Parsing& out) {
  auto seed = whole_number(option);
  if (!seed.has_value()) {
    return seed.error();
  }
  out.args.seed = seed.value();
  return std::nullopt;
}

std::optional<std::string> take_footprint(const Written& option, Parsing& out) {
  auto size = core::parse_byte_size(option.value);
  if (!size.has_value()) {
    return refused(option.key, std::string{core::describe(size.error())});
  }
  if (!size.value().is_absolute()) {
    return refused(option.key, "a trial needs an absolute size, not a share of memory");
  }
  out.args.footprint_bytes = size.value().byte_count();
  return std::nullopt;
}

std::optional<std::string> take_isa(const Written& option, Parsing& out) {
  out.args.isa = simd::parse_isa_path(option.value);
  if (!out.args.isa.has_value()) {
    return refused(option.key,
                   "'" + std::string{option.value} + "' is none of scalar, avx2, avx512, neon");
  }
  return std::nullopt;
}

std::optional<std::string> take_lock(const Written& option, Parsing& out) {
  for (const verification::LockPolicy policy :
       {verification::LockPolicy::kRequire, verification::LockPolicy::kTry,
        verification::LockPolicy::kNone}) {
    if (verification::describe(policy) == option.value) {
      out.args.lock = policy;
      return std::nullopt;
    }
  }
  return refused(option.key, "'" + std::string{option.value} + "' is none of require, try, none");
}

std::optional<std::string> take_sample_interval(const Written& option, Parsing& out) {
  auto length = positive_ns(option);
  if (!length.has_value()) {
    return length.error();
  }
  out.args.sample_interval_ns = length.value();
  return std::nullopt;
}

/// The range `limits.max_temperature_c` has in a configuration (config.hpp).
std::optional<std::string> take_max_temperature(const Written& option, Parsing& out) {
  auto celsius = whole_number(option);
  if (!celsius.has_value()) {
    return celsius.error();
  }
  if (celsius.value() < 1 ||
      celsius.value() > static_cast<std::uint64_t>(config::kMaxTemperatureLimitC)) {
    std::string range = "must be between 1 and ";
    range += std::to_string(config::kMaxTemperatureLimitC);
    range += " degrees Celsius";
    return refused(option.key, range);
  }
  out.args.max_temperature_c = static_cast<std::int64_t>(celsius.value());
  return std::nullopt;
}

struct Option {
  std::string_view key;
  std::optional<std::string> (*take)(const Written&, Parsing&);
  bool repeatable;
};

constexpr std::array<Option, 19> kOptions{{
    {"--objective", &take_objective, false},
    {"--candidate", &take_candidate, true},
    {"--reference", &take_reference, false},
    {"--duration", &take_duration, false},
    {"--settle", &take_settle, false},
    {"--settle-slope", &take_settle_slope, false},
    {"--settle-window", &take_settle_window, false},
    {"--settle-max", &take_settle_max, false},
    {"--replicates", &take_replicates, false},
    {"--final-replicates", &take_final_replicates, false},
    {"--elimination", &take_elimination, false},
    {"--interleave", &take_interleave, false},
    {"--reference-replicates", &take_reference_replicates, false},
    {"--seed", &take_seed, false},
    {"--footprint", &take_footprint, false},
    {"--isa", &take_isa, false},
    {"--lock", &take_lock, false},
    {"--sample-interval", &take_sample_interval, false},
    {"--max-temperature", &take_max_temperature, false},
}};

constexpr std::string_view kVerbose = "--verbose";

/// The settle phase the parts come to, or the refusal.
core::Result<std::optional<explore::Settle>, std::string> settle_of(const Parsing& parsing) {
  const bool extras = parsing.settle_slope.has_value() || parsing.settle_window_ns.has_value() ||
                      parsing.settle_max_ns.has_value();
  if (!parsing.settle_metric.has_value()) {
    if (extras) {
      return std::string{"--settle-slope, --settle-window and --settle-max need --settle METRIC"};
    }
    return std::optional<explore::Settle>{};
  }
  explore::Settle settle;
  settle.metric = *parsing.settle_metric;
  settle.max_slope_per_second = parsing.settle_slope.value_or(settle.max_slope_per_second);
  settle.window_ns = parsing.settle_window_ns.value_or(settle.window_ns);
  settle.max_ns = parsing.settle_max_ns.value_or(settle.max_ns);
  if (settle.window_ns > settle.max_ns) {
    // The slope is judged only once a full window has been read, and the phase ends at
    // its maximum: a window longer than the maximum can never settle.
    return "--settle-window (" + std::to_string(settle.window_ns / kNanosecondsPerMillisecond) +
           " ms) is longer than --settle-max (" +
           std::to_string(settle.max_ns / kNanosecondsPerMillisecond) +
           " ms): the settle phase could never settle";
  }
  return std::optional<explore::Settle>{std::move(settle)};
}

/// Whether the statistic is not defined by fewer than two readings of the window: a
/// spread, a slope, a rate (a span) and a delta (a pair).
bool needs_two_readings(
    const std::variant<explore::GaugeStatistic, explore::CounterStatistic>& statistic) {
  if (const auto* gauge = std::get_if<explore::GaugeStatistic>(&statistic)) {
    return *gauge == explore::GaugeStatistic::kStddev || *gauge == explore::GaugeStatistic::kSlope;
  }
  return true;
}

/// The refusal of a load too short for the objective, or nothing: a trial of a
/// duration that cannot hold two readings at the sample interval ends the search
/// with "no reading for the objective" after a whole trial, exit 6, for what is a
/// mistake in the options. Two sample intervals always hold two rounds.
std::optional<std::string> short_load_refusal(const ExploreArgs& args) {
  if (!needs_two_readings(args.objective.statistic) ||
      args.duration_ns >= 2 * args.sample_interval_ns) {
    return std::nullopt;
  }
  const std::string statistic = std::visit(
      [](auto each) { return std::string{explore::describe(each)}; }, args.objective.statistic);
  return refused("--duration",
                 "must be at least twice --sample-interval (" +
                     std::to_string(args.sample_interval_ns / kNanosecondsPerMillisecond) +
                     " ms): " + statistic +
                     " needs two readings in the window, and the duration is " +
                     std::to_string(args.duration_ns / kNanosecondsPerMillisecond) + " ms");
}

}  // namespace

core::Result<ExploreArgs, std::string> parse_explore_args(std::span<const std::string_view> args) {
  Parsing parsing;
  std::vector<std::string_view> seen;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string_view key = args[index];
    if (key == kVerbose) {
      if (parsing.args.verbose) {
        return refused(key, "given twice");
      }
      parsing.args.verbose = true;
      continue;
    }
    const auto* const option = std::ranges::find(kOptions, key, &Option::key);
    if (option == kOptions.end()) {
      return refused(key, "not an option of explore");
    }
    if (!option->repeatable && std::ranges::find(seen, key) != seen.end()) {
      return refused(key, "given twice");
    }
    if (index + 1 >= args.size()) {
      return refused(key, "missing its value");
    }
    seen.push_back(key);
    ++index;
    if (auto refusal = option->take(Written{key, args[index]}, parsing); refusal.has_value()) {
      return *refusal;
    }
  }
  if (!parsing.objective_given) {
    return std::string{"--objective is required"};
  }
  if (parsing.args.candidates.empty()) {
    return std::string{"at least one --candidate is required"};
  }
  auto settle = settle_of(parsing);
  if (!settle.has_value()) {
    return settle.error();
  }
  parsing.args.settle = std::move(settle).value();
  if (auto refusal = short_load_refusal(parsing.args); refusal.has_value()) {
    return *refusal;
  }
  return std::move(parsing.args);
}

explore::SearchConfig search_config_of(const ExploreArgs& args) {
  explore::SearchConfig config;
  config.objective = args.objective;
  config.candidates = args.candidates;
  if (args.reference.has_value()) {
    config.reference = *args.reference;
  } else {
    // One worker for each of the first candidate's workloads, anywhere: the
    // plainest load that stands for the machine, and not itself a candidate.
    config.reference.id = "reference";
    config.reference.workloads = args.candidates.front().workloads;
    config.reference.threads = static_cast<std::uint32_t>(config.reference.workloads.size());
  }
  const auto apply = [&args](explore::Candidate& each) {
    each.duration_ns = args.duration_ns;
    each.settle = args.settle;
  };
  std::ranges::for_each(config.candidates, apply);
  apply(config.reference);
  config.base_replicates = args.base_replicates;
  config.elimination_factor = args.elimination_factor;
  config.final_replicates = args.final_replicates;
  config.interleave_every = args.interleave_every;
  config.reference_replicates = args.reference_replicates;
  return config;
}

namespace {

std::string number(double value) {
  std::ostringstream text;
  text << std::setprecision(6) << value;
  return text.str();
}

std::string estimate_text(const explore::Estimate& estimate) {
  std::string text = "mean " + number(estimate.mean);
  if (estimate.half_width.has_value()) {
    text += " +/- " + number(*estimate.half_width) +
            " (95% CI, n=" + std::to_string(estimate.replicates) + ", sd " +
            number(estimate.stddev.value_or(0.0)) + ")";
  } else {
    text += " (n=" + std::to_string(estimate.replicates) + ", no spread from fewer than two)";
  }
  return text;
}

/// The 1-based replicates (or readings) that `which` of their note is true of.
std::string noted_list(const std::vector<explore::TrialNote>& notes,
                       bool explore::TrialNote::*which) {
  std::string text;
  for (std::size_t i = 0; i < notes.size(); ++i) {
    if (notes[i].*which) {
      text += " " + std::to_string(i + 1);
    }
  }
  return text;
}

std::string statistic_text(const explore::BoundObjective& objective) {
  return std::visit([](auto statistic) { return std::string{explore::describe(statistic)}; },
                    objective.statistic);
}

}  // namespace

std::string render_report(const explore::SearchReport& report) {
  std::ostringstream text;
  const telemetry::Metric& metric = report.objective.metric;
  text << "explore: objective " << statistic_text(report.objective) << " of " << metric.name << " ("
       << telemetry::describe(metric.scope) << " " << metric.scope_index << ", "
       << telemetry::describe(metric.unit) << "), " << explore::describe(report.objective.direction)
       << "\n"
       << "explore: " << report.trials << " trial(s) in " << report.rounds << " round(s)\n";
  if (report.reference_stop.has_value()) {
    text << "worst case: none; the search ended early because a reference trial's verdict was "
         << results::describe(*report.reference_stop)
         << ". What was measured before it follows; no candidate was ranked\n";
  } else if (report.worst_case.has_value()) {
    text << "worst case: " << explore::format(*report.worst_case) << "\n";
  } else {
    text << "worst case: none; every candidate left the search with findings or an "
            "unfinished trial\n";
  }
  text << "ranking: " << explore::describe(report.ranking);
  if (!report.rival.empty()) {
    text << " (" << report.rival << ")";
  }
  text << "\n";
  if (!report.uncompared.empty()) {
    text << "  not compared by an interval (eliminated with fewer than two replicates):";
    for (const std::string& id : report.uncompared) {
      text << " " << id;
    }
    text << "\n";
  }
  if (report.unsettled_trials > 0) {
    text << "settle: " << report.unsettled_trials << " of " << report.settle_trials
         << " trial(s) did not settle before the settle maximum and started from an "
            "uncontrolled state"
         << (report.unsettled_trials == report.settle_trials
                 ? "; no trial of this search started from a controlled state"
                 : "")
         << "\n";
  }
  if (report.corrected_error_trials > 0) {
    text << "corrected hardware errors: " << report.corrected_error_trials
         << " scored trial(s) had the hardware report an error it corrected; the scores stand, "
            "the exit code is 5 as for stress\n";
  }
  for (const explore::CandidateReport& candidate : report.candidates) {
    text << "candidate " << candidate.id << ": " << explore::describe(candidate.fate)
         << ", ranked in " << candidate.rounds << " round(s), " << candidate.samples.size()
         << " replicate(s)\n";
    if (!candidate.samples.empty()) {
      // The survivor's estimates are from the replicates after the ones that chose it.
      std::string basis;
      if (candidate.selection_replicates > 0) {
        basis = " of the " +
                std::to_string(candidate.samples.size() - candidate.selection_replicates) +
                " replicate(s) run after it was chosen";
      }
      text << "  corrected for drift" << basis << ": " << estimate_text(candidate.corrected)
           << "\n";
      text << "  raw" << basis << ": " << estimate_text(candidate.raw) << "\n";
      text << "  replicates:";
      for (std::size_t i = 0; i < candidate.samples.size(); ++i) {
        text << (i == candidate.selection_replicates && i > 0 ? " |" : "") << " "
             << number(candidate.samples[i]);
      }
      text << "\n";
      if (candidate.selection_replicates > 0) {
        text << "  (the " << candidate.selection_replicates
             << " replicate(s) before the bar chose this candidate and are not in its estimates)\n";
      }
      if (const std::string flagged =
              noted_list(candidate.notes, &explore::TrialNote::corrected_errors);
          !flagged.empty()) {
        text << "  corrected hardware errors in replicate(s):" << flagged << "\n";
      }
      if (const std::string flagged = noted_list(candidate.notes, &explore::TrialNote::unsettled);
          !flagged.empty()) {
        text << "  replicate(s) that did not settle:" << flagged << "\n";
      }
    }
  }
  text << "reference: anchor " << number(report.anchor) << ", drift " << number(report.drift)
       << " (the last reading less the anchor), " << estimate_text(report.reference) << "\n"
       << "  readings (midpoint of the load window in ns, value):";
  for (const explore::ReferencePoint& point : report.references) {
    text << " (" << point.at_ns << ", " << number(point.value) << ")";
  }
  text << "\n";
  if (const std::string flagged =
          noted_list(report.reference_notes, &explore::TrialNote::corrected_errors);
      !flagged.empty()) {
    text << "  corrected hardware errors in reference reading(s):" << flagged << "\n";
  }
  if (const std::string flagged =
          noted_list(report.reference_notes, &explore::TrialNote::unsettled);
      !flagged.empty()) {
    text << "  reference reading(s) that did not settle:" << flagged << "\n";
  }
  return text.str();
}

int exit_code_for(const explore::SearchReport& report) noexcept {
  const auto fate_is = [&report](explore::Fate fate) {
    return std::ranges::any_of(report.candidates, [fate](const explore::CandidateReport& each) {
      return each.fate == fate;
    });
  };
  const bool reference_found = report.reference_stop == results::Verdict::kFindings;
  if (fate_is(explore::Fate::kFindings) || reference_found || report.corrected_error_trials > 0) {
    return kExitFault;
  }
  if (fate_is(explore::Fate::kIncomplete) || report.reference_stop.has_value()) {
    return kExitNotVerified;
  }
  return kOk;
}

int exit_code_for(const explore::ExploreError& error) noexcept {
  switch (error.fault) {
    case explore::ExploreFault::kUnknownMetric:
    case explore::ExploreFault::kMetricUnavailable:
    case explore::ExploreFault::kStatistic:
    case explore::ExploreFault::kConfiguration:
    case explore::ExploreFault::kDutyCycle:
      return kRefused;
    case explore::ExploreFault::kTrial:
    case explore::ExploreFault::kReference:
      return kTrialFailed;
    case explore::ExploreFault::kNoReading:
      return kExitNotVerified;
  }
  return kRefused;
}

// The two streams are the command's standard output and error, in the order every command
// of this build takes them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int run_explore(ControllerTrialRunner& runner, explore::SearchConfig config, std::ostream& out,
                std::ostream& err) {
  auto searched = explore::explore(runner, std::move(config));
  if (const std::optional<int> child = runner.worker_exit_code(); child.has_value()) {
    return *child;  // This process is a worker that could not exec.
  }
  if (!searched.has_value()) {
    const explore::ExploreError& error = searched.error();
    err << "loadforge: explore: " << explore::describe(error.fault) << ": " << error.detail << "\n";
    return exit_code_for(error);
  }
  out << render_report(searched.value());
  return exit_code_for(searched.value());
}

int explore_command(std::span<const std::string_view> args, const std::string& self_link,
                    std::string_view sysfs_root, const TrialOptions& base, std::ostream& out,
                    std::ostream& err) {
  auto parsed = parse_explore_args(args);
  if (!parsed.has_value()) {
    err << "loadforge: explore: " << parsed.error() << "\n";
    return kUsageError;
  }
  const ExploreArgs& arguments = parsed.value();
  platform::RealSyscalls syscalls;
  const std::optional<Launch> launch = prepare_launch(syscalls, self_link, sysfs_root, err);
  if (!launch.has_value()) {
    return kRefused;
  }
  const workloads::BuiltinRegistry registry;
  TrialOptions options = base;
  options.executable = launch->executable;
  options.isa = arguments.isa;
  options.seed = arguments.seed;
  options.footprint_bytes = arguments.footprint_bytes;
  options.lock = arguments.lock;
  options.sample_interval_ns = arguments.sample_interval_ns;
  options.max_temperature_c = arguments.max_temperature_c;
  options.log = arguments.verbose ? &out : nullptr;
  auto made =
      ControllerTrialRunner::create(syscalls, launch->machine, registry, std::move(options));
  if (!made.has_value()) {
    err << "loadforge: explore: " << made.error().detail << "\n";
    return kRefused;
  }
  const std::unique_ptr<ControllerTrialRunner> runner = std::move(made).value();
  out << telemetry::render_capabilities(runner->capabilities());
  // Flushed before the first fork, so a child that fails to exec and exits
  // normally cannot replay buffered output.
  out.flush();
  err.flush();
  return run_explore(*runner, search_config_of(arguments), out, err);
}

}  // namespace loadforge::cli
