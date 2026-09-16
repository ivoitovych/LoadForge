// SPDX-License-Identifier: GPL-3.0-or-later
#include "topology/source.hpp"

#include <cerrno>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "topology/cpu_list.hpp"

namespace loadforge::topology {

std::string_view describe(Availability availability) noexcept {
  // No default arm, deliberately. -Wswitch (on, and -Werror) then makes a new
  // enumerator without a word for it a build failure rather than a message
  // reading "unknown", which is what a default arm would quietly produce
  // forever. The same reason ExitStatus has no default arm.
  switch (availability) {
    case Availability::kPresent:
      return "present";
    case Availability::kAbsent:
      return "not offered by this kernel";
    case Availability::kVanished:
      return "gone since it was last read";
    case Availability::kDenied:
      return "not permitted";
    case Availability::kMalformed:
      return "not in the expected format";
    case Availability::kUnreadable:
      return "unreadable";
  }
  // Reached only for a value outside the enumeration, which is possible because
  // an enum's value range is wider than its enumerators. Saying so beats
  // returning an empty string that a report would render as a blank.
  return "unrecognised availability";
}

std::string describe(const Unavailable& unavailable) {
  return unavailable.path + ": " + std::string{describe(unavailable.kind)} + " (" +
         unavailable.detail + ")";
}

Availability classify(const platform::SyscallError& error, bool already_seen) {
  switch (error.number) {
    case ENOENT:
    case ENODEV:
    case ENXIO:
      // The three ways sysfs says "there is nothing here": the attribute was
      // never created (ENOENT), or the device behind it has gone (ENODEV,
      // ENXIO -- a driver returns these from its own show() once the device is
      // detached, which happens between our open and our read).
      //
      // `already_seen` is the whole reason this function takes a second
      // argument. The errno is identical in both cases; only the history
      // separates a source this kernel never offered from one that was here a
      // second ago.
      return already_seen ? Availability::kVanished : Availability::kAbsent;

    case EACCES:
    case EPERM:
      // Note this is NOT promoted to kVanished when already_seen. Permission
      // does not evaporate because a device did, and telling a user their
      // machine changed when what they actually need is a group membership
      // sends them looking in the wrong place.
      return Availability::kDenied;

    default:
      // Everything else, and the list matters less than the fact that it is
      // NOT silently folded into "absent":
      //
      //   EIO      the device is failing -- which for a reliability tool is a
      //            finding, not a missing feature.
      //   EISDIR   the path is a directory. sysfs opens one happily and fails
      //            the read, so this arrives here rather than at open.
      //   ENOTDIR  a component of the path is a file. Verified on this kernel.
      //   EINTR    past FileSystem's retry bound: a signal storm, not a gap.
      //   EFBIG    FileSystem's own refusal of an implausibly large value.
      //
      // EISDIR and ENOTDIR both mean OUR PATH IS WRONG, not that the machine
      // lacks something. Reporting them as kAbsent would turn a bug in our own
      // path construction into a confident, permanent statement about the
      // user's hardware -- and nothing downstream would ever question it.
      return Availability::kUnreadable;
  }
}

core::Result<std::string, Unavailable> Source::text() {
  auto contents = filesystem_->read_first_line(path_);
  if (!contents.has_value()) {
    const platform::SyscallError& error = contents.error();
    // Built in two steps, and the order is load-bearing for a reason that is
    // not obvious: `describe` allocates, and so does copying `path_`. Written
    // as one aggregate initialiser -- `Unavailable{kind, path_, describe(e)}` --
    // the compiler must emit a landing pad that destroys the already-copied
    // path if describe throws, and that destructor carries a branch (the small
    // string check) which no test can ever take. gcovr's
    // --exclude-throw-branches does not remove it: the edge inside the cleanup
    // block is not itself labelled a throw edge. Hoisting the second allocation
    // out means the only throwing construction left happens when nothing is
    // half-built, so no cleanup path exists to be uncoverable. See journal §1.13.
    std::string detail = describe(error);
    const Availability kind = classify(error, seen_);
    return Unavailable{kind, path_, std::move(detail)};
  }
  // Set only on success, and only here: `seen_` means "this source has yielded
  // a value", which is exactly the claim kVanished rests on. Setting it on
  // entry, or on a failed read, would make the first ENOENT after a denied read
  // report as kVanished -- a machine change that never happened.
  seen_ = true;
  return contents.value();
}

core::Result<CpuList, Unavailable> Source::cpu_list() {
  auto contents = text();
  if (!contents.has_value()) {
    return contents.error();
  }
  auto parsed = CpuList::parse(contents.value());
  if (!parsed.has_value()) {
    std::string detail = describe(parsed.error());  // Hoisted; see text() above.
    return Unavailable{Availability::kMalformed, path_, std::move(detail)};
  }
  return parsed.value();
}

}  // namespace loadforge::topology
