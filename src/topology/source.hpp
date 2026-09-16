// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_SOURCE_HPP
#define LOADFORGE_TOPOLOGY_SOURCE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "platform/syscall_error.hpp"
#include "topology/cpu_list.hpp"

namespace loadforge::topology {

/// What a source could tell us -- and when it could not, what KIND of could-not.
///
/// This enum is the capability model (F3) made into a type. Everything above it
/// exists because "I have no value for you" is not one fact but six, and
/// collapsing them is how a tool ends up reporting a machine it is not running
/// on. A discovery layer that returns an empty optional for all of these tells
/// its caller nothing it can act on, and tells the USER nothing at all.
///
/// The distinctions are not academic. Each one has a different remedy and a
/// different meaning for the run that follows:
///
///   kPresent     the value is here, and it parsed.
///   kAbsent      this kernel does not offer this source and never did. An
///                older kernel, a config option off, a counter this CPU lacks.
///                NOT an error: it is a fact about the machine, and a run that
///                proceeds without it is a legitimate run with a smaller
///                telemetry set. The report must say so rather than stay silent.
///   kVanished    it was here, we read it, and now it is gone. The machine
///                changed UNDER US -- a CPU offlined, a device unplugged, a
///                cgroup torn down. Reporting this as kAbsent would be the
///                single most misleading thing this module could do: it would
///                say "this machine never had that" about a machine that did,
///                and every number gathered before the change would silently
///                belong to a different machine than the summary claims.
///   kDenied      it is here and we are not allowed to read it. The remedy is
///                permissions -- a capability, a group, a sysctl -- and the user
///                can act on it the moment they are told which path.
///   kMalformed   it is here, it was read, and the text is not what this format
///                can be. Refused rather than guessed at; see CpuList.
///   kUnreadable  present, permitted, and the read failed anyway. EIO from a
///                dying device, a signal storm past the retry bound, a path that
///                is not a file at all. Nothing here is a normal condition.
///
/// kVanished is the reason Source carries state at all. No stateless classifier
/// can produce it: ENOENT is ENOENT, and only a reader that remembers having
/// succeeded on this path can tell "never offered" from "gone since".
enum class Availability : std::uint8_t {
  kPresent,
  kAbsent,
  kVanished,
  kDenied,
  kMalformed,
  kUnreadable,
};

/// The word for an Availability, for messages and for test failures that name
/// what they expected. Every enumerator has one; there is no default arm, so
/// adding a state without a word for it does not compile.
[[nodiscard]] std::string_view describe(Availability availability) noexcept;

/// Why a source yielded no value.
///
/// `detail` is already rendered -- the errno's description, or the parser's
/// reason -- because the thing that knew the cause is the thing that had the
/// evidence, and reconstructing it later means guessing. `path` is carried
/// separately from the detail so a caller can group, filter or retry by path
/// without parsing a sentence.
///
/// The suppression is the same one platform::SyscallError carries, for the same
/// provably-narrow reason. clang-analyzer claims `kind` can be garbage when this
/// struct is copied out of a Result; it cannot follow a value through
/// std::variant, so it treats the held alternative as unconstructed. Every field
/// here has a default member initialiser, so even a default-constructed
/// Unavailable has defined values and the condition the check describes cannot
/// occur for this type. The diagnostic is reported at the struct, which is why
/// the marker is here rather than at the copy in source.cpp; that copy is
/// asserted intact, field by field, by
/// SourceTest.ACpuListPropagatesAReadFailureUnchanged.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct Unavailable {
  Availability kind = Availability::kUnreadable;
  std::string path;
  std::string detail;

  [[nodiscard]] friend bool operator==(const Unavailable&, const Unavailable&) = default;
};

/// Human-readable rendering, suitable for putting in front of a user.
[[nodiscard]] std::string describe(const Unavailable& unavailable);

/// Classifies a failed read of a sysfs attribute into an availability state.
///
/// Exposed rather than kept private because it is pure, it is where the
/// judgement lives, and a table of errno-to-meaning that cannot be tested
/// directly is a table nobody will keep correct. `already_seen` is the memory
/// that separates kAbsent from kVanished.
[[nodiscard]] Availability classify(const platform::SyscallError& error, bool already_seen);

/// One sysfs attribute, remembered across readings.
///
/// WHY THIS IS A CLASS AND NOT A FUNCTION
/// --------------------------------------
/// Because of kVanished, and only because of it. A free function
/// `read_cpu_list(fs, path)` would be simpler in every other respect, and it
/// would be incapable of telling the two ENOENTs apart -- which is the one
/// distinction this module was written to make. The state is a single bool and
/// it earns its place.
///
/// Thread-safety: NOT safe to share. `seen_` is mutated by every successful
/// read. A Source belongs to whatever is sampling that attribute, and telemetry
/// sampling is per-thread by construction. Stated because FileSystem directly
/// below it IS shareable, and the difference is not guessable.
class Source {
 public:
  Source(platform::FileSystem& filesystem, std::string path)
      : filesystem_(&filesystem), path_(std::move(path)) {}

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  /// Whether this source has ever yielded a value. The memory behind kVanished.
  [[nodiscard]] bool has_been_read() const noexcept { return seen_; }

  /// The attribute's first line, trimmed -- or why there is none.
  ///
  /// An EMPTY string is a success, not an absence. `offline` is empty on a
  /// machine with every CPU up, and a reader that treated empty as "no value"
  /// would report a healthy machine as an unreadable one.
  [[nodiscard]] core::Result<std::string, Unavailable> text();

  /// The attribute parsed as a CPU list -- or why it could not be.
  ///
  /// Folds the parse failure into the same Availability ladder as the read
  /// failures, so a caller has ONE thing to handle. A caller forced to unpack a
  /// Result-of-a-Result writes the second check less carefully than the first,
  /// and the second is the malformed one.
  [[nodiscard]] core::Result<CpuList, Unavailable> cpu_list();

  /// The attribute parsed as a signed integer -- or why it could not be.
  ///
  /// Signed, and that is not laziness. `core_id` and `physical_package_id` are
  /// **-1** on a kernel that cannot determine them, which happens on some
  /// virtualised and arm64 machines. An unsigned parser would refuse that as
  /// "not a digit" and report a malformed file, which is a lie: the file is
  /// exactly what the kernel meant to write, and the truth is that the kernel
  /// does not know. Reading it faithfully lets the caller say so.
  ///
  /// Accepts an optional leading '-' and digits, nothing else -- no whitespace,
  /// no '+', no hex. The kernel writes none of those, and accepting them would
  /// turn a file that is not what we think into a plausible number.
  [[nodiscard]] core::Result<std::int64_t, Unavailable> integer();

 private:
  platform::FileSystem* filesystem_;
  std::string path_;
  bool seen_ = false;
};

}  // namespace loadforge::topology

#endif
