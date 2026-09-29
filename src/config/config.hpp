// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONFIG_CONFIG_HPP
#define LOADFORGE_CONFIG_CONFIG_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/result.hpp"

namespace loadforge::config {

/// The configuration schema this build reads. A file must say which schema it
/// was written for, so that a file from a later LoadForge is refused rather
/// than half-read: a key this version does not know is not ignorable when the
/// key may be the one that bounds the run.
inline constexpr std::int64_t kSchemaVersion = 1;

/// The three operating modes (docs/PLAN.md §4.4). They differ in experimental
/// discipline, not just in duration, which is why the mode is a required field
/// and not inferred from the rest of the file.
enum class Mode : std::uint8_t { kBenchmark, kStress, kExplore };

[[nodiscard]] std::string_view describe(Mode mode) noexcept;
[[nodiscard]] std::optional<Mode> parse_mode(std::string_view text) noexcept;

/// How many threads a workload runs: every CPU the process may use, or a
/// count. The count is not bounded here against anything -- the topology is
/// the only honest ceiling, and it is not known until discovery runs, so the
/// scheduler compares the two. Zero is refused at parse time: a workload with
/// no threads is a request for nothing.
class Threads {
 public:
  static constexpr Threads all() noexcept { return Threads{0}; }
  static constexpr Threads count(std::uint32_t n) noexcept { return Threads{n}; }

  [[nodiscard]] constexpr bool is_all() const noexcept { return count_ == 0; }
  /// The count. Meaningless when is_all(); callers check first.
  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return count_; }

  [[nodiscard]] friend constexpr bool operator==(Threads, Threads) noexcept = default;

 private:
  constexpr explicit Threads(std::uint32_t n) noexcept : count_(n) {}
  std::uint32_t count_;
};

struct Run {
  Mode mode = Mode::kStress;
  core::Duration duration{0};

  [[nodiscard]] friend bool operator==(const Run&, const Run&) = default;
};

/// The highest temperature a limit may name. Silicon junction limits top out
/// around 110-125 C on every CPU this project will meet, so a figure past 150
/// cannot be a considered choice; it is 95 with a digit stuck to it, and a
/// run that trusted it would never stop. Zero and negatives are refused for
/// the opposite reason: they stop the run before it starts.
inline constexpr std::int64_t kMaxTemperatureLimitC = 150;

struct Limits {
  std::optional<std::int64_t> max_temperature_c;
  std::optional<core::ByteSize> memory;

  [[nodiscard]] friend bool operator==(const Limits&, const Limits&) = default;
};

struct Workload {
  std::string name;
  Threads threads = Threads::all();
  bool enabled = true;
  std::optional<core::ByteSize> memory;

  [[nodiscard]] friend bool operator==(const Workload&, const Workload&) = default;
};

struct Config {
  Run run;
  Limits limits;
  std::vector<Workload> workloads;

  [[nodiscard]] friend bool operator==(const Config&, const Config&) = default;
};

/// Why a configuration was refused: where it came from, which key, at which
/// line, and what is wrong with it. Every field is for the person who has to
/// fix the file; "invalid configuration" is not a message this project emits.
///
/// The suppression is the one SyscallError carries, for the same reason:
/// clang-analyzer cannot follow a value through std::variant and reports the
/// held alternative's `line` as garbage when a ConfigError is copied out of a
/// Result. Every field has a default member initialiser, so the condition
/// cannot occur for this type; the copies are asserted field by field in
/// config_test.cpp.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct ConfigError {
  std::string origin;      ///< The path, or "<string>" for text parsed directly.
  std::string key;         ///< Dotted, e.g. "run.duration"; empty for a whole-document fault.
  std::uint32_t line = 0;  ///< 1-based, or 0 when no line applies.
  std::string message;

  [[nodiscard]] friend bool operator==(const ConfigError&, const ConfigError&) = default;
};

/// "origin:line: key: message", omitting the parts that do not apply.
[[nodiscard]] std::string describe(const ConfigError& error);

/// Parses and validates a document. `origin` names it in errors.
///
/// WHAT IS REFUSED, AND WHY EACH IS A REFUSAL AND NOT A DEFAULT
/// ------------------------------------------------------------
/// An unknown key anywhere. A file with `duraton = "5h"` would otherwise run
/// for the default duration, and there is no default duration precisely so
/// that this cannot happen -- but `max_temprature_c` would be ignored and the
/// run would have no thermal limit, silently. A typo is refused by name.
///
/// A wrong type. `duration = 5` is not five of anything; the unit is part of
/// the value (docs/DESIGN-DRAFT.md §37).
///
/// A value outside its domain: a zero duration, zero threads, a temperature
/// limit past kMaxTemperatureLimitC, a memory share past 100%. Each domain
/// is stated in one place, and each boundary has a test.
///
/// A missing schema_version, or one this build does not read.
[[nodiscard]] core::Result<Config, ConfigError> parse(std::string_view text,
                                                      std::string origin = "<string>");

/// Reads and parses a file. Ordinary file I/O, on purpose: docs/PLAN.md §4.2
/// keeps the platform seam for hardware introspection, and a config file is
/// tested with real files in a temporary directory.
[[nodiscard]] core::Result<Config, ConfigError> load(const std::filesystem::path& path);

/// Renders a configuration as a document `parse` accepts, so that a run can
/// record exactly what it ran with and a reader can feed it back in.
[[nodiscard]] std::string to_toml(const Config& config);

/// What the machine can offer, for resolving a memory share (docs/PLAN.md F7).
///
/// The ceiling is MemAvailable and the cgroup limit, whichever is lower --
/// never MemTotal, which invites the OOM killer, and never host memory alone
/// inside a container, where the cgroup is the real wall. Reading these is
/// platform knowledge; this module only does the arithmetic.
struct MemoryBudget {
  std::uint64_t available_bytes = 0;
  std::optional<std::uint64_t> cgroup_limit_bytes;

  [[nodiscard]] friend bool operator==(const MemoryBudget&, const MemoryBudget&) = default;
};

/// The ceiling a budget implies: the lower of the two figures.
[[nodiscard]] std::uint64_t ceiling_of(const MemoryBudget& budget) noexcept;

/// A byte count for a configured footprint. A share is a share of the
/// ceiling, rounded down. An absolute size larger than the ceiling is refused
/// rather than clamped: the user asked for a specific footprint, and a run
/// that quietly used less would report results for a workload nobody
/// configured.
[[nodiscard]] core::Result<std::uint64_t, ConfigError> resolve_memory(core::ByteSize size,
                                                                      const MemoryBudget& budget,
                                                                      std::string_view key);

}  // namespace loadforge::config

#endif
