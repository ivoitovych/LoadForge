// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/config.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <toml.hpp>
#include <utility>
#include <vector>

#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/parse_error.hpp"
#include "core/result.hpp"

namespace loadforge::config {

std::string_view describe(Mode mode) noexcept {
  switch (mode) {
    case Mode::kBenchmark:
      return "benchmark";
    case Mode::kStress:
      return "stress";
    case Mode::kExplore:
      return "explore";
  }
  return "unrecognised mode";
}

std::optional<Mode> parse_mode(std::string_view text) noexcept {
  if (text == "benchmark") {
    return Mode::kBenchmark;
  }
  if (text == "stress") {
    return Mode::kStress;
  }
  if (text == "explore") {
    return Mode::kExplore;
  }
  return std::nullopt;
}

std::string describe(const ConfigError& error) {
  std::string text = error.origin;
  if (error.line != 0) {
    text += ':';
    text += std::to_string(error.line);
  }
  if (!text.empty()) {
    text += ": ";
  }
  if (!error.key.empty()) {
    text += error.key;
    text += ": ";
  }
  text += error.message;
  return text;
}

namespace {

using Keys = std::initializer_list<std::string_view>;

/// A type's name for a message: "expected a string, found integer".
std::string type_name(const toml::node& node) {
  std::ostringstream out;
  out << node.type();
  return out.str();
}

std::uint32_t line_of(const toml::node& node) noexcept { return node.source().begin.line; }

/// An integer only if the node IS an integer. toml++'s value<int64_t>() also
/// answers for a boolean (true is 1) and for a float with an integral value,
/// which is exactly the coercion a schema exists to refuse: `threads = true`
/// is not one thread. Found by the test that said so, not by reading the
/// docs.
std::optional<std::int64_t> integer_of(const toml::node& node) noexcept {
  if (!node.is_integer()) {
    return std::nullopt;
  }
  return node.value<std::int64_t>();
}

std::optional<bool> boolean_of(const toml::node& node) noexcept {
  if (!node.is_boolean()) {
    return std::nullopt;
  }
  return node.value<bool>();
}

/// Joins a dotted key path: "" + "run" is "run", "run" + "mode" is "run.mode".
/// Two string_views in a row: the check is right that they could be swapped,
/// and the order (outer, inner) is the one every call site reads naturally.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string join(std::string_view prefix, std::string_view key) {
  if (prefix.empty()) {
    return std::string{key};
  }
  std::string text{prefix};
  text += '.';
  text += key;
  return text;
}

/// Everything the validation walk needs to name a fault: where the document
/// came from. Each method returns the value or the first fault it met; the
/// walk stops at the first fault because a document with two is fixed one at
/// a time anyway, and the second message is often a consequence of the first.
class Validator {
 public:
  /// A view: the origin string outlives the walk in parse(), and a view has
  /// no destructor for the unwind path to branch in.
  explicit Validator(std::string_view origin) : origin_(origin) {}

  [[nodiscard]] core::Result<Config, ConfigError> document(const toml::table& root) {
    if (auto unknown = check_keys(root, "", {"schema_version", "run", "limits", "workload"});
        unknown.has_value()) {
      return unknown.value();
    }
    if (auto version = schema_version(root); version.has_value()) {
      return version.value();
    }

    Config config;
    auto run = run_table(root);
    if (!run.has_value()) {
      return run.error();
    }
    config.run = run.value();

    auto limits = limits_table(root);
    if (!limits.has_value()) {
      return limits.error();
    }
    config.limits = limits.value();

    auto workloads = workload_array(root);
    if (!workloads.has_value()) {
      return workloads.error();
    }
    config.workloads = workloads.value();
    return config;
  }

 private:
  // --- faults ------------------------------------------------------------------
  // Built from named locals throughout (journal §1.13): the message arrives
  // complete, and the aggregate takes copies and moves only.

  [[nodiscard]] ConfigError fault(std::string key, std::uint32_t line, std::string message) const {
    std::string origin{origin_};
    return ConfigError{std::move(origin), std::move(key), line, std::move(message)};
  }

  [[nodiscard]] ConfigError fault_at(std::string key, const toml::node& node,
                                     std::string message) const {
    return fault(std::move(key), line_of(node), std::move(message));
  }

  // --- structure ---------------------------------------------------------------

  /// The first key the schema does not know, named, with what it does know.
  /// An unknown key is never ignored: it may be the misspelling of the one
  /// that bounds the run.
  [[nodiscard]] std::optional<ConfigError> check_keys(const toml::table& table,
                                                      std::string_view prefix, Keys allowed) const {
    for (const auto& [key, node] : table) {
      if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
        std::string message = "unknown key; this build knows: ";
        for (const std::string_view name : allowed) {
          if (name != *allowed.begin()) {
            message += ", ";
          }
          message += name;
        }
        return fault_at(join(prefix, key.str()), node, std::move(message));
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ConfigError> schema_version(const toml::table& root) const {
    const toml::node* node = root.get("schema_version");
    if (node == nullptr) {
      std::string message =
          "schema_version is required; this build reads schema " + std::to_string(kSchemaVersion);
      return fault("schema_version", 0, std::move(message));
    }
    const std::optional<std::int64_t> version = integer_of(*node);
    if (!version.has_value()) {
      std::string message = "expected an integer, found " + type_name(*node);
      return fault_at("schema_version", *node, std::move(message));
    }
    if (version.value() != kSchemaVersion) {
      std::string message = "schema " + std::to_string(version.value()) +
                            " is not one this build reads (it reads " +
                            std::to_string(kSchemaVersion) + ")";
      return fault_at("schema_version", *node, std::move(message));
    }
    return std::nullopt;
  }

  /// A table that must be present.
  [[nodiscard]] core::Result<const toml::table*, ConfigError> required_table(
      const toml::table& parent, std::string_view key) const {
    const toml::node* node = parent.get(key);
    if (node == nullptr) {
      return fault(std::string{key}, 0, "required table is missing");
    }
    const toml::table* table = node->as_table();
    if (table == nullptr) {
      std::string message = "expected a table, found " + type_name(*node);
      return fault_at(std::string{key}, *node, std::move(message));
    }
    return table;
  }

  // --- values ------------------------------------------------------------------

  [[nodiscard]] core::Result<std::string_view, ConfigError> required_string(
      const toml::table& table, std::string_view prefix, std::string_view key) const {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
      return fault(join(prefix, key), 0, "required value is missing");
    }
    const std::optional<std::string_view> text = node->value<std::string_view>();
    if (!text.has_value()) {
      std::string message = "expected a string, found " + type_name(*node);
      return fault_at(join(prefix, key), *node, std::move(message));
    }
    return text.value();
  }

  /// An optional byte size. Zero is refused: no footprint is not a footprint,
  /// and the way to ask for none is to leave the key out.
  [[nodiscard]] core::Result<std::optional<core::ByteSize>, ConfigError> optional_byte_size(
      const toml::table& table, std::string_view prefix, std::string_view key) const {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
      return std::optional<core::ByteSize>{};
    }
    const std::optional<std::string_view> text = node->value<std::string_view>();
    if (!text.has_value()) {
      std::string message =
          R"(expected a string such as "8GiB" or "70%", found )" + type_name(*node);
      return fault_at(join(prefix, key), *node, std::move(message));
    }
    const auto size = core::parse_byte_size(text.value());
    if (!size.has_value()) {
      std::string message{core::describe(size.error())};
      return fault_at(join(prefix, key), *node, std::move(message));
    }
    if (size.value().is_absolute() && size.value().byte_count() == 0) {
      return fault_at(join(prefix, key), *node, "memory must be more than zero bytes");
    }
    return std::optional<core::ByteSize>{size.value()};
  }

  // --- sections ----------------------------------------------------------------

  [[nodiscard]] core::Result<Run, ConfigError> run_table(const toml::table& root) const {
    auto table = required_table(root, "run");
    if (!table.has_value()) {
      return table.error();
    }
    const toml::table& run = *table.value();
    if (auto unknown = check_keys(run, "run", {"mode", "duration"}); unknown.has_value()) {
      return unknown.value();
    }

    Run parsed;
    auto mode_text = required_string(run, "run", "mode");
    if (!mode_text.has_value()) {
      return mode_text.error();
    }
    const std::optional<Mode> mode = parse_mode(mode_text.value());
    if (!mode.has_value()) {
      return fault_at("run.mode", *run.get("mode"),
                      "mode must be one of benchmark, stress, explore");
    }
    parsed.mode = mode.value();

    auto duration_text = required_string(run, "run", "duration");
    if (!duration_text.has_value()) {
      return duration_text.error();
    }
    const auto duration = core::parse_duration(duration_text.value());
    if (!duration.has_value()) {
      std::string message{core::describe(duration.error())};
      return fault_at("run.duration", *run.get("duration"), std::move(message));
    }
    if (duration.value() <= core::Duration{0}) {
      return fault_at("run.duration", *run.get("duration"), "duration must be longer than zero");
    }
    parsed.duration = duration.value();
    return parsed;
  }

  [[nodiscard]] core::Result<Limits, ConfigError> limits_table(const toml::table& root) const {
    Limits parsed;
    const toml::node* node = root.get("limits");
    if (node == nullptr) {
      return parsed;  // Every limit is optional, and so is the table.
    }
    const toml::table* limits = node->as_table();
    if (limits == nullptr) {
      std::string message = "expected a table, found " + type_name(*node);
      return fault_at("limits", *node, std::move(message));
    }
    if (auto unknown = check_keys(*limits, "limits", {"max_temperature_c", "memory"});
        unknown.has_value()) {
      return unknown.value();
    }

    if (const toml::node* temperature = limits->get("max_temperature_c"); temperature != nullptr) {
      const std::optional<std::int64_t> celsius = integer_of(*temperature);
      if (!celsius.has_value()) {
        std::string message = "expected an integer, found " + type_name(*temperature);
        return fault_at("limits.max_temperature_c", *temperature, std::move(message));
      }
      if (celsius.value() < 1 || celsius.value() > kMaxTemperatureLimitC) {
        std::string message =
            "must be between 1 and " + std::to_string(kMaxTemperatureLimitC) + " degrees Celsius";
        return fault_at("limits.max_temperature_c", *temperature, std::move(message));
      }
      parsed.max_temperature_c = celsius;
    }

    auto memory = optional_byte_size(*limits, "limits", "memory");
    if (!memory.has_value()) {
      return memory.error();
    }
    parsed.memory = memory.value();
    return parsed;
  }

  [[nodiscard]] core::Result<Threads, ConfigError> threads_value(const toml::table& workload,
                                                                 std::string_view prefix) const {
    const toml::node* node = workload.get("threads");
    const std::string key = join(prefix, "threads");
    if (node == nullptr) {
      return fault(key, 0, "required value is missing");
    }
    if (const std::optional<std::string_view> text = node->value<std::string_view>();
        text.has_value()) {
      if (text.value() == "all") {
        return Threads::all();
      }
      return fault_at(key, *node, "threads must be a positive integer or \"all\"");
    }
    const std::optional<std::int64_t> count = integer_of(*node);
    if (!count.has_value()) {
      std::string message =
          "threads must be a positive integer or \"all\", found " + type_name(*node);
      return fault_at(key, *node, std::move(message));
    }
    if (count.value() < 1 || count.value() > std::numeric_limits<std::uint32_t>::max()) {
      return fault_at(key, *node, "threads must be a positive integer or \"all\"");
    }
    return Threads::count(static_cast<std::uint32_t>(count.value()));
  }

  [[nodiscard]] core::Result<Workload, ConfigError> workload_table(const toml::table& table,
                                                                   std::string_view prefix) const {
    if (auto unknown = check_keys(table, prefix, {"name", "threads", "enabled", "memory"});
        unknown.has_value()) {
      return unknown.value();
    }
    Workload parsed;

    auto name = required_string(table, prefix, "name");
    if (!name.has_value()) {
      return name.error();
    }
    if (name.value().empty()) {
      return fault_at(join(prefix, "name"), *table.get("name"), "name must not be empty");
    }
    parsed.name = std::string{name.value()};

    auto threads = threads_value(table, prefix);
    if (!threads.has_value()) {
      return threads.error();
    }
    parsed.threads = threads.value();

    if (const toml::node* enabled = table.get("enabled"); enabled != nullptr) {
      const std::optional<bool> flag = boolean_of(*enabled);
      if (!flag.has_value()) {
        std::string message = "expected true or false, found " + type_name(*enabled);
        return fault_at(join(prefix, "enabled"), *enabled, std::move(message));
      }
      parsed.enabled = flag.value();
    }

    auto memory = optional_byte_size(table, prefix, "memory");
    if (!memory.has_value()) {
      return memory.error();
    }
    parsed.memory = memory.value();
    return parsed;
  }

  [[nodiscard]] core::Result<std::vector<Workload>, ConfigError> workload_array(
      const toml::table& root) const {
    const toml::node* node = root.get("workload");
    if (node == nullptr) {
      return fault("workload", 0, "at least one [[workload]] is required");
    }
    const toml::array* array = node->as_array();
    if (array == nullptr) {
      std::string message = "expected an array of tables ([[workload]]), found " + type_name(*node);
      return fault_at("workload", *node, std::move(message));
    }
    if (array->empty()) {
      return fault_at("workload", *node, "at least one [[workload]] is required");
    }

    std::vector<Workload> parsed;
    parsed.reserve(array->size());
    std::size_t index = 0;
    for (const toml::node& element : *array) {
      const std::string prefix = "workload[" + std::to_string(index) + "]";
      const toml::table* table = element.as_table();
      if (table == nullptr) {
        std::string message = "expected a table, found " + type_name(element);
        return fault_at(prefix, element, std::move(message));
      }
      auto workload = workload_table(*table, prefix);
      if (!workload.has_value()) {
        return workload.error();
      }
      parsed.push_back(workload.value());
      ++index;
    }
    return parsed;
  }

  std::string_view origin_;
};

}  // namespace

/// A fault that concerns the whole document rather than one key. Parameters
/// by value and moved into the aggregate (journal §1.13): building the
/// ConfigError from allocating expressions in place leaves a cleanup edge in
/// the initialiser that no test can take.
ConfigError whole_document_fault(std::string origin, std::uint32_t line, std::string message) {
  return ConfigError{std::move(origin), std::string{}, line, std::move(message)};
}

core::Result<Config, ConfigError> parse(std::string_view text, std::string origin) {
  // The parser's result is scoped so that only the table outlives it: the
  // walk below can allocate, and every non-trivial local alive across it is
  // a destructor on the unwind path.
  toml::table root;
  {
    toml::parse_result result = toml::parse(text, origin);
    if (result.failed()) {
      // The document could not be read at all. The line is the parser's, and
      // the message its own words: it knows what it choked on and this code
      // does not.
      std::string message = "TOML syntax: " + std::string{result.error().description()};
      return whole_document_fault(std::move(origin), result.error().source().begin.line,
                                  std::move(message));
    }
    root = std::move(result).table();
  }
  Validator validator(origin);
  return validator.document(root);
}

namespace {

using FileHandle = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

}  // namespace

core::Result<Config, ConfigError> load(const std::filesystem::path& path) {
  // C stdio rather than an ifstream, on purpose: libstdc++ THROWS from the
  // read of a directory (EISDIR) whatever the stream's exception mask says,
  // and a catch clause is a branch no test can take. fread reports the same
  // fact through ferror, as a value.
  std::string origin = path.string();
  const FileHandle file(std::fopen(origin.c_str(), "rb"), &std::fclose);
  if (file == nullptr) {
    // No errno in the message: the path is what a reader can check, and
    // "cannot open" is distinct from "cannot read" (a directory), from an
    // empty document and from a malformed one (F3).
    return whole_document_fault(std::move(origin), 0, "cannot open the file");
  }
  std::string text;
  std::array<char, 4096> chunk{};
  for (;;) {
    const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), file.get());
    text.append(chunk.data(), got);
    if (got < chunk.size()) {
      break;  // End of file, or an error; ferror tells them apart.
    }
  }
  if (std::ferror(file.get()) != 0) {
    return whole_document_fault(std::move(origin), 0, "cannot read the file");
  }
  return parse(text, std::move(origin));
}

std::string to_toml(const Config& config) {
  toml::table run;
  run.insert("mode", std::string{describe(config.run.mode)});
  run.insert("duration", core::to_string(config.run.duration));

  toml::table limits;
  if (config.limits.max_temperature_c.has_value()) {
    limits.insert("max_temperature_c", config.limits.max_temperature_c.value());
  }
  if (config.limits.memory.has_value()) {
    limits.insert("memory", core::to_string(config.limits.memory.value()));
  }

  toml::array workloads;
  for (const Workload& workload : config.workloads) {
    toml::table table;
    table.insert("name", workload.name);
    if (workload.threads.is_all()) {
      table.insert("threads", "all");
    } else {
      table.insert("threads", static_cast<std::int64_t>(workload.threads.value()));
    }
    table.insert("enabled", workload.enabled);
    if (workload.memory.has_value()) {
      table.insert("memory", core::to_string(workload.memory.value()));
    }
    workloads.push_back(std::move(table));
  }

  toml::table root;
  root.insert("schema_version", kSchemaVersion);
  root.insert("run", std::move(run));
  root.insert("limits", std::move(limits));
  root.insert("workload", std::move(workloads));

  std::ostringstream out;
  out << root << '\n';
  return out.str();
}

std::uint64_t ceiling_of(const MemoryBudget& budget) noexcept {
  if (budget.cgroup_limit_bytes.has_value()) {
    return std::min(budget.available_bytes, budget.cgroup_limit_bytes.value());
  }
  return budget.available_bytes;
}

core::Result<std::uint64_t, ConfigError> resolve_memory(core::ByteSize size,
                                                        const MemoryBudget& budget,
                                                        std::string_view key) {
  const std::uint64_t ceiling = ceiling_of(budget);
  if (size.is_absolute()) {
    const std::uint64_t wanted = size.byte_count();
    if (wanted > ceiling) {
      std::string message = "requests " + std::to_string(wanted) +
                            " bytes but the machine can offer " + std::to_string(ceiling);
      std::string named_key{key};
      return ConfigError{std::string{}, std::move(named_key), 0, std::move(message)};
    }
    return wanted;
  }
  // floor(ceiling * percent / 100) without the product overflowing: split the
  // ceiling into whole hundreds and a remainder, each of which times a
  // percentage up to 100 fits comfortably.
  const std::uint64_t percent = size.percent_of_available();
  constexpr std::uint64_t kHundred = 100;
  return (ceiling / kHundred) * percent + ((ceiling % kHundred) * percent) / kHundred;
}

}  // namespace loadforge::config
