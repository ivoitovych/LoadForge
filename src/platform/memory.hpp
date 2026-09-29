// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_MEMORY_HPP
#define LOADFORGE_PLATFORM_MEMORY_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {

/// The huge-page strategy for a mapping. An explicit recorded choice rather
/// than a default the kernel makes for us (docs/PLAN.md F7): the strategy
/// changes TLB behaviour and therefore the result, so a run record that did
/// not say which one was in effect would describe a measurement nobody can
/// repeat.
///
///   kNone         ordinary pages. What the kernel gives when asked nothing.
///   kTransparent  MADV_HUGEPAGE on the range: a request the kernel may honour
///                 in whole, in part, or not at all, silently. Verified by
///                 reading the mapping's smaps afterwards, which is telemetry's
///                 job at M3, not this module's.
///   kExplicit     MAP_HUGETLB from the reserved pool: either the whole
///                 mapping is huge pages or the map fails. Nothing silent.
enum class HugePages : std::uint8_t { kNone, kTransparent, kExplicit };

/// The size of the explicit huge page this module asks for, always. It is
/// named in the mmap flags rather than left to the kernel's default: the
/// default is 2 MiB on x86-64 and on arm64 with 4 KiB base pages, and 512 MiB
/// on arm64 with 64 KiB base pages, and a run record saying "huge pages" that
/// meant a different size on a different machine would not be a record.
inline constexpr std::size_t kExplicitHugePageBytes = std::size_t{2} << 20;

/// What a caller wants mapped.
struct MappingRequest {
  std::size_t bytes = 0;
  HugePages huge_pages = HugePages::kNone;
  bool populate = false;              ///< Fault every page in before returning.
  bool lock = false;                  ///< mlock the range; implies it is populated.
  std::optional<std::uint32_t> node;  ///< Bind every page to this NUMA node.

  [[nodiscard]] friend bool operator==(const MappingRequest&, const MappingRequest&) = default;
};

/// The step of a mapping request that failed. They run in this order, and a
/// failure at any step unmaps what the earlier steps built.
enum class Stage : std::uint8_t { kMap, kBind, kAdvise, kPopulate, kLock };

/// Why the kernel would not do what a step asked.
///
///   kBadRequest    EINVAL: a length of zero, an unaligned address, a node the
///                  machine does not have, a huge-page size it does not
///                  support. Also a request refused before any call was made,
///                  because no call could have succeeded; the cause then names
///                  the check rather than a syscall.
///   kExhausted     ENOMEM or EAGAIN: no memory, no free huge pages, the
///                  address space is full, or RLIMIT_MEMLOCK is reached. The
///                  remedy is a smaller request or a larger reservation.
///   kNotPermitted  EPERM or EACCES: RLIMIT_MEMLOCK is zero and the process
///                  lacks CAP_IPC_LOCK, or a policy forbids the mapping.
///   kUnsupported   ENOSYS or EOPNOTSUPP: this kernel was built without the
///                  feature -- NUMA, or a madvise it predates.
///   kUnexpected    anything else. EFAULT would mean a bug in this code.
enum class Refusal : std::uint8_t {
  kBadRequest,
  kExhausted,
  kNotPermitted,
  kUnsupported,
  kUnexpected
};

[[nodiscard]] std::string_view describe(HugePages huge_pages) noexcept;
[[nodiscard]] std::string_view describe(Stage stage) noexcept;
[[nodiscard]] std::string_view describe(Refusal refusal) noexcept;
[[nodiscard]] std::string describe(const MappingRequest& request);

/// Classifies a failed memory call. Pure and exposed, so the table of
/// errno-to-meaning is tested directly rather than through a fake.
[[nodiscard]] Refusal classify_refusal(const SyscallError& error) noexcept;

/// `bytes` rounded up to a whole number of explicit huge pages, or nothing
/// when that rounding would not fit in a size_t.
///
/// The kernel rounds a MAP_HUGETLB length up itself -- verified: a one-byte
/// request maps and consumes a whole page -- but munmap does NOT: unmapping
/// with the unrounded length is EINVAL and the mapping stays. So the rounded
/// length is what a Mapping must remember, and it is computed here, above the
/// seam, where the overflow arm can be tested.
[[nodiscard]] std::optional<std::size_t> round_up_to_huge_pages(std::size_t bytes) noexcept;

/// A mask naming exactly this node. Refuses an id past kNodeMaskBits, which
/// no Linux kernel can number.
[[nodiscard]] core::Result<NodeMask, std::string_view> node_mask_of(std::uint32_t node);

/// A refused request: which step, what kind, what was asked, and the words
/// of the kernel -- or of the check that refused it before the kernel was
/// asked, in which case `cause.call` names the check.
struct MappingRefused {
  Stage stage = Stage::kMap;
  Refusal kind = Refusal::kUnexpected;
  MappingRequest request;
  SyscallError cause;

  [[nodiscard]] friend bool operator==(const MappingRefused&, const MappingRefused&) = default;
};

/// Human-readable rendering, suitable for putting in front of a user.
[[nodiscard]] std::string describe(const MappingRefused& refused);

/// An anonymous mapping that unmaps itself. Move-only: two owners of one
/// range would unmap it twice, and the second would be an EINVAL at best and
/// somebody else's memory at worst.
///
/// The destructor's unmap is silent, because a destructor has nowhere to
/// report to. A caller that needs to know the unmap succeeded -- and for a
/// huge-page mapping a failed unmap is a page kept from the pool -- calls
/// release() and reads the answer.
class Mapping {
 public:
  Mapping(Syscalls& syscalls, void* address, std::size_t bytes) noexcept
      : syscalls_(&syscalls), address_(address), bytes_(bytes) {}
  ~Mapping();
  Mapping(Mapping&& other) noexcept;
  Mapping& operator=(Mapping&& other) noexcept;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  /// The start of the range. Null once released or moved from.
  [[nodiscard]] void* address() const noexcept { return address_; }

  /// The length the kernel holds, which for explicit huge pages is the
  /// request rounded up to whole pages. Zero once released or moved from.
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  /// Unmaps now and reports the answer. A second call, or a call on a
  /// moved-from Mapping, has nothing to unmap and says so by succeeding.
  [[nodiscard]] core::Result<core::Ok, SyscallError> release();

 private:
  Syscalls* syscalls_;
  void* address_;
  std::size_t bytes_;
};

/// Maps memory the way a workload asked for it, or says exactly which step
/// the kernel refused and why.
///
/// THE ORDER OF THE STEPS IS THE POINT
/// -----------------------------------
/// map, then bind, then advise, then populate, then lock. The NUMA policy and
/// the huge-page advice govern how pages are FAULTED, so both must be in place
/// before anything touches the range; populating and locking touch it. Lock
/// after populate rather than instead of it: mlock also faults pages in, but
/// a caller who asked for both gets both, and the record says so.
///
/// WHAT IS NOT CHECKED HERE, DELIBERATELY
/// --------------------------------------
/// A length of zero, a node the machine lacks, a huge-page pool that is empty:
/// every one is sent to the kernel and the answer classified, for the reason
/// Affinity gives (journal §2.12). The two exceptions are requests no kernel
/// could accept -- a huge-page rounding that overflows size_t, a node id past
/// what a node mask can hold -- and those are refused with a cause that says
/// no call was made.
class Memory {
 public:
  explicit Memory(Syscalls& syscalls) : syscalls_(&syscalls) {}

  [[nodiscard]] core::Result<Mapping, MappingRefused> map(const MappingRequest& request);

 private:
  Syscalls* syscalls_;
};

}  // namespace loadforge::platform

#endif
