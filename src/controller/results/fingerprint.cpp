// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/results/fingerprint.hpp"

#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <variant>

#include "core/parse_number.hpp"
#include "core/result.hpp"
#include "core/version.hpp"
#include "resources/memory_budget.hpp"

#ifndef LOADFORGE_BUILD_FLAGS
#error "LOADFORGE_BUILD_FLAGS is defined by src/controller/results/CMakeLists.txt"
#endif
#ifndef LOADFORGE_GIT_COMMIT
#error "LOADFORGE_GIT_COMMIT is defined by src/controller/results/CMakeLists.txt"
#endif

namespace loadforge::results {

namespace {

#if defined(__clang__)
constexpr std::string_view kCompiler = "clang " __clang_version__;
#elif defined(__GNUC__)
constexpr std::string_view kCompiler = "gcc " __VERSION__;
#else
constexpr std::string_view kCompiler = "unknown compiler";
#endif

constexpr std::int64_t kMillidegreesPerDegree = 1000;

using topology::Availability;
using topology::Unavailable;

using Value = std::variant<bool, double, std::string>;

// Why a parser refused a file's text.
struct Refused {
  std::string detail;
};

std::string unexpected(std::string_view expected, const std::string& found) {
  std::string detail{"expected "};
  detail += expected;
  detail += ", found \"";
  detail += found;
  detail += "\"";
  return detail;
}

core::Result<Value, Refused> parse_flag(const std::string& text) {
  if (text == "1") {
    return Value{true};
  }
  if (text == "0") {
    return Value{false};
  }
  return Refused{unexpected("0 or 1", text)};
}

// intel_pstate's no_turbo is the question asked the other way round.
core::Result<Value, Refused> parse_no_turbo(const std::string& text) {
  auto no_turbo = parse_flag(text);
  if (!no_turbo.has_value()) {
    return no_turbo;
  }
  return Value{!std::get<bool>(no_turbo.value())};
}

core::Result<Value, Refused> parse_mains(const std::string& text) {
  if (text == "1") {
    return Value{std::string{"ac"}};
  }
  if (text == "0") {
    return Value{std::string{"battery"}};
  }
  return Refused{unexpected("0 or 1", text)};
}

core::Result<Value, Refused> parse_word(const std::string& text) {
  if (text.empty()) {
    return Refused{"expected a value, found an empty file"};
  }
  return Value{text};
}

core::Result<Value, Refused> parse_selected(const std::string& text) {
  auto word = selected_word(text);
  if (!word.has_value()) {
    return Refused{unexpected("one [selected] word", text)};
  }
  return Value{std::move(*word)};
}

core::Result<Value, Refused> parse_millidegrees(const std::string& text) {
  std::int64_t value = 0;
  const char* const end = std::to_address(text.end());
  const auto parsed = std::from_chars(std::to_address(text.begin()), end, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    return Refused{unexpected("a whole number of millidegrees", text)};
  }
  return Value{static_cast<double>(value) / static_cast<double>(kMillidegreesPerDegree)};
}

// One file a field may come from, and how to read what it says.
struct Candidate {
  std::string_view path;
  core::Result<Value, Refused> (*parse)(const std::string&);
};

// The failure a field reports when no candidate answered: the first that is not
// a plain absence, else the first. `failures` is never empty: a field has at
// least one candidate and each one that did not answer left its failure.
const Unavailable& most_informative(const std::vector<Unavailable>& failures) {
  for (const Unavailable& failure : failures) {
    if (failure.kind != Availability::kAbsent) {
      return failure;
    }
  }
  return failures.front();
}

// Appends in place: a gap built as a temporary and handed to push_back leaves a
// cleanup path no test can take (journal §1.13).
void add_gap(std::vector<FingerprintGap>& gaps, std::string_view field, const Unavailable& why) {
  gaps.emplace_back();
  gaps.back().field = field;
  gaps.back().why = why;
}

std::optional<std::string> text_of(const std::optional<Value>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::get<std::string>(*value);
}

std::optional<bool> flag_of(const std::optional<Value>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::get<bool>(*value);
}

std::optional<double> real_of(const std::optional<Value>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::get<double>(*value);
}

class Reader {
 public:
  Reader(platform::FileSystem& filesystem, std::string_view root, std::vector<FingerprintGap>& gaps)
      : filesystem_(&filesystem), root_(root), gaps_(&gaps) {}

  // The first candidate that yields a value. When none does, one gap for the field.
  std::optional<Value> parsed(std::string_view field, std::initializer_list<Candidate> candidates) {
    std::vector<Unavailable> failures;
    for (const Candidate& candidate : candidates) {
      topology::Source source(*filesystem_, root_ + std::string(candidate.path));
      auto reading = parse_file(source, candidate.parse);
      if (reading.has_value()) {
        return std::move(reading).value();
      }
      failures.push_back(reading.error());
    }
    add_gap(*gaps_, field, most_informative(failures));
    return std::nullopt;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  std::optional<std::string> cpuinfo(std::string_view field, std::string_view key) {
    topology::Source source(*filesystem_, root_ + "/proc/cpuinfo");
    auto line = cpuinfo_line(source, key);
    if (line.has_value()) {
      return std::move(line).value();
    }
    add_gap(*gaps_, field, line.error());
    return std::nullopt;
  }

 private:
  static core::Result<Value, Unavailable> parse_file(
      topology::Source& source, core::Result<Value, Refused> (*parse)(const std::string&)) {
    auto text = source.text();
    if (!text.has_value()) {
      return text.error();
    }
    auto value = parse(text.value());
    if (!value.has_value()) {
      std::string detail = value.error().detail;
      return Unavailable{Availability::kMalformed, source.path(), std::move(detail)};
    }
    return std::move(value).value();
  }

  static core::Result<std::string, Unavailable> cpuinfo_line(topology::Source& source,
                                                             std::string_view key) {
    auto lines = source.lines();
    if (!lines.has_value()) {
      return lines.error();
    }
    auto value = cpuinfo_value(lines.value(), key);
    if (!value.has_value()) {
      std::string detail = "no \"" + std::string(key) + "\" line";
      return Unavailable{Availability::kAbsent, source.path(), std::move(detail)};
    }
    return std::move(*value);
  }

  platform::FileSystem* filesystem_;
  std::string root_;
  std::vector<FingerprintGap>* gaps_;
};

template <typename T>
void compare(std::vector<std::string_view>& out, std::string_view name, const T& lhs,
             const T& rhs) {
  if (!(lhs == rhs)) {
    out.push_back(name);
  }
}

}  // namespace

BuildInfo this_build() {
  BuildInfo build;
  build.loadforge_version = core::kVersion;
  build.git_commit = non_empty(LOADFORGE_GIT_COMMIT);
  build.compiler = kCompiler;
  build.flags = LOADFORGE_BUILD_FLAGS;
  return build;
}

std::optional<std::string> non_empty(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  return std::string(text);
}

std::optional<std::string> cpuinfo_value(const std::vector<std::string>& lines,
                                         std::string_view key) {
  for (const std::string& line : lines) {
    std::string_view rest{line};
    if (!rest.starts_with(key)) {
      continue;
    }
    rest.remove_prefix(key.size());
    const std::size_t colon = rest.find_first_not_of(" \t");
    if (colon == std::string_view::npos || rest[colon] != ':') {
      continue;
    }
    rest.remove_prefix(colon + 1);
    return std::string(core::trim(rest));
  }
  return std::nullopt;
}

std::optional<std::string> selected_word(std::string_view line) {
  const std::size_t open = line.find('[');
  const std::size_t close = line.find(']');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open ||
      line.find('[', open + 1) != std::string_view::npos ||
      line.find(']', close + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view word = line.substr(open + 1, close - open - 1);
  if (word.empty()) {
    return std::nullopt;
  }
  return std::string(word);
}

FingerprintReading read_fingerprint(platform::FileSystem& filesystem, std::string_view root,
                                    const FingerprintContext& context) {
  FingerprintReading reading;
  Fingerprint& fingerprint = reading.fingerprint;
  Reader reader(filesystem, root, reading.gaps);

  fingerprint.loadforge_version = context.build.loadforge_version;
  fingerprint.git_commit = context.build.git_commit;
  fingerprint.compiler = context.build.compiler;
  fingerprint.flags = context.build.flags;
  fingerprint.fp_environment = context.fp_environment;
  fingerprint.isa_path = context.isa_path;

  fingerprint.cpu_model = reader.cpuinfo("cpu_model", "model name");
  fingerprint.microcode = reader.cpuinfo("microcode", "microcode");
  fingerprint.kernel =
      text_of(reader.parsed("kernel", {{"/proc/sys/kernel/osrelease", parse_word}}));
  fingerprint.smt_enabled =
      flag_of(reader.parsed("smt_enabled", {{"/sys/devices/system/cpu/smt/active", parse_flag}}));
  fingerprint.power_source =
      text_of(reader.parsed("power_source", {{"/sys/class/power_supply/AC/online", parse_mains},
                                             {"/sys/class/power_supply/ACAD/online", parse_mains},
                                             {"/sys/class/power_supply/ADP0/online", parse_mains},
                                             {"/sys/class/power_supply/ADP1/online", parse_mains},
                                             {"/sys/class/power_supply/AC0/online", parse_mains}}));
  fingerprint.battery_state = text_of(
      reader.parsed("battery_state", {{"/sys/class/power_supply/BAT0/status", parse_word},
                                      {"/sys/class/power_supply/BAT1/status", parse_word}}));
  fingerprint.cpufreq_governor = text_of(reader.parsed(
      "cpufreq_governor", {{"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", parse_word}}));
  fingerprint.energy_performance_preference = text_of(reader.parsed(
      "energy_performance_preference",
      {{"/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", parse_word}}));
  fingerprint.platform_profile = text_of(
      reader.parsed("platform_profile", {{"/sys/firmware/acpi/platform_profile", parse_word}}));
  fingerprint.turbo_enabled = flag_of(reader.parsed(
      "turbo_enabled", {{"/sys/devices/system/cpu/intel_pstate/no_turbo", parse_no_turbo},
                        {"/sys/devices/system/cpu/cpufreq/boost", parse_flag}}));
  fingerprint.cooling_profile = text_of(
      reader.parsed("cooling_profile", {{"/sys/class/thermal/thermal_zone0/policy", parse_word}}));

  fingerprint.huge_page_policy = text_of(reader.parsed(
      "huge_page_policy", {{"/sys/kernel/mm/transparent_hugepage/enabled", parse_selected}}));
  fingerprint.temperature_at_start_c = real_of(reader.parsed(
      "temperature_at_start_c", {{"/sys/class/thermal/thermal_zone0/temp", parse_millidegrees}}));

  const auto budget = resources::read_memory_budget(filesystem, root);
  if (budget.has_value()) {
    fingerprint.cgroup_memory_limit_bytes = budget.value().cgroup_limit_bytes;
  } else {
    const topology::DiscoveryFailure& failure = budget.error();
    Unavailable why;
    why.kind = failure.availability;
    why.path = failure.subject;
    why.detail = failure.detail;
    add_gap(reading.gaps, "cgroup_memory_limit_bytes", why);
  }
  return reading;
}

std::vector<std::string_view> differing_fields(const Fingerprint& lhs, const Fingerprint& rhs) {
  std::vector<std::string_view> out;
  compare(out, "loadforge_version", lhs.loadforge_version, rhs.loadforge_version);
  compare(out, "git_commit", lhs.git_commit, rhs.git_commit);
  compare(out, "compiler", lhs.compiler, rhs.compiler);
  compare(out, "flags", lhs.flags, rhs.flags);
  compare(out, "cpu_model", lhs.cpu_model, rhs.cpu_model);
  compare(out, "microcode", lhs.microcode, rhs.microcode);
  compare(out, "kernel", lhs.kernel, rhs.kernel);
  compare(out, "smt_enabled", lhs.smt_enabled, rhs.smt_enabled);
  compare(out, "power_source", lhs.power_source, rhs.power_source);
  compare(out, "battery_state", lhs.battery_state, rhs.battery_state);
  compare(out, "cpufreq_governor", lhs.cpufreq_governor, rhs.cpufreq_governor);
  compare(out, "energy_performance_preference", lhs.energy_performance_preference,
          rhs.energy_performance_preference);
  compare(out, "platform_profile", lhs.platform_profile, rhs.platform_profile);
  compare(out, "turbo_enabled", lhs.turbo_enabled, rhs.turbo_enabled);
  compare(out, "cooling_profile", lhs.cooling_profile, rhs.cooling_profile);
  compare(out, "cgroup_memory_limit_bytes", lhs.cgroup_memory_limit_bytes,
          rhs.cgroup_memory_limit_bytes);
  compare(out, "huge_page_policy", lhs.huge_page_policy, rhs.huge_page_policy);
  compare(out, "temperature_at_start_c", lhs.temperature_at_start_c, rhs.temperature_at_start_c);
  compare(out, "fp_environment", lhs.fp_environment, rhs.fp_environment);
  compare(out, "isa_path", lhs.isa_path, rhs.isa_path);
  return out;
}

bool is_moment_field(std::string_view field) noexcept { return field == "temperature_at_start_c"; }

}  // namespace loadforge::results
