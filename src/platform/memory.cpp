// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/memory.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::platform {
namespace {

/// MAP_HUGETLB with the page size named: bits 26..31 carry log2 of the size,
/// and 2 MiB is 2^21. Spelled out rather than taken from <linux/mman.h>,
/// which cannot be included alongside <sys/mman.h> without redefinitions.
constexpr int kExplicitHugePageFlags = MAP_HUGETLB | (21 << MAP_HUGE_SHIFT);
static_assert((std::size_t{1} << 21) == kExplicitHugePageBytes,
              "the flag's log2 must name kExplicitHugePageBytes");

}  // namespace

std::string_view describe(HugePages huge_pages) noexcept {
  switch (huge_pages) {
    case HugePages::kNone:
      return "ordinary pages";
    case HugePages::kTransparent:
      return "transparent huge pages";
    case HugePages::kExplicit:
      return "explicit huge pages";
  }
  return "unrecognised huge-page strategy";
}

std::string_view describe(Stage stage) noexcept {
  switch (stage) {
    case Stage::kMap:
      return "mapping";
    case Stage::kBind:
      return "binding to the NUMA node";
    case Stage::kAdvise:
      return "advising transparent huge pages";
    case Stage::kPopulate:
      return "populating";
    case Stage::kLock:
      return "locking";
  }
  return "unrecognised stage";
}

std::string_view describe(Refusal refusal) noexcept {
  switch (refusal) {
    case Refusal::kBadRequest:
      return "the request is not one the kernel accepts";
    case Refusal::kExhausted:
      return "the memory, huge pages or lock allowance are exhausted";
    case Refusal::kNotPermitted:
      return "not permitted";
    case Refusal::kUnsupported:
      return "this kernel does not support it";
    case Refusal::kUnexpected:
      return "the kernel refused for a reason this code does not expect";
  }
  return "unrecognised refusal";
}

std::string describe(const MappingRequest& request) {
  std::string text = std::to_string(request.bytes) + " bytes, ";
  text += describe(request.huge_pages);
  if (request.populate) {
    text += ", populated";
  }
  if (request.lock) {
    text += ", locked";
  }
  if (request.node.has_value()) {
    text += ", on node " + std::to_string(request.node.value());
  }
  return text;
}

Refusal classify_refusal(const SyscallError& error) noexcept {
  switch (error.number) {
    case EINVAL:
      return Refusal::kBadRequest;
    case ENOMEM:
    case EAGAIN:
      // EAGAIN is mlock's word for "some of the range could not be locked"
      // and mmap's for a locked file; both are a shortage, not a bad request.
      return Refusal::kExhausted;
    case EPERM:
    case EACCES:
      return Refusal::kNotPermitted;
    case ENOSYS:
    case EOPNOTSUPP:
      return Refusal::kUnsupported;
    default:
      return Refusal::kUnexpected;
  }
}

std::optional<std::size_t> round_up_to_huge_pages(std::size_t bytes) noexcept {
  constexpr std::size_t kLargestRoundable =
      std::numeric_limits<std::size_t>::max() - (kExplicitHugePageBytes - 1);
  if (bytes > kLargestRoundable) {
    return std::nullopt;
  }
  const std::size_t pages = (bytes + kExplicitHugePageBytes - 1) / kExplicitHugePageBytes;
  return pages * kExplicitHugePageBytes;
}

core::Result<NodeMask, std::string_view> node_mask_of(std::uint32_t node) {
  if (node >= kNodeMaskBits) {
    return std::string_view{"a node id past what any Linux kernel can number"};
  }
  NodeMask mask;
  mask.nodes.set(node);
  return mask;
}

std::string describe(const MappingRefused& refused) {
  return "cannot map " + describe(refused.request) + ": " + std::string{describe(refused.stage)} +
         " failed, " + std::string{describe(refused.kind)} + " (" + describe(refused.cause) + ")";
}

// --- Mapping -------------------------------------------------------------------

Mapping::~Mapping() {
  if (syscalls_ != nullptr) {
    // Nowhere to report to; release() is the path for a caller who must know.
    (void)syscalls_->unmap(address_, bytes_);
  }
}

Mapping::Mapping(Mapping&& other) noexcept
    : syscalls_(std::exchange(other.syscalls_, nullptr)),
      address_(std::exchange(other.address_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0)) {}

Mapping& Mapping::operator=(Mapping&& other) noexcept {
  if (this != &other) {
    if (syscalls_ != nullptr) {
      (void)syscalls_->unmap(address_, bytes_);
    }
    syscalls_ = std::exchange(other.syscalls_, nullptr);
    address_ = std::exchange(other.address_, nullptr);
    bytes_ = std::exchange(other.bytes_, 0);
  }
  return *this;
}

core::Result<core::Ok, SyscallError> Mapping::release() {
  Syscalls* const syscalls = std::exchange(syscalls_, nullptr);
  if (syscalls == nullptr) {
    return core::Ok{};
  }
  void* const address = std::exchange(address_, nullptr);
  const std::size_t bytes = std::exchange(bytes_, 0);
  return syscalls->unmap(address, bytes);
}

// --- Memory --------------------------------------------------------------------

namespace {

/// Built from named locals (journal §1.13): the cause arrives by value and is
/// moved, the request is copied, and the aggregate takes both without an
/// allocating temporary in the expression.
MappingRefused refuse(Stage stage, const MappingRequest& request, SyscallError cause) {
  const Refusal kind = classify_refusal(cause);
  return MappingRefused{stage, kind, request, std::move(cause)};
}

/// A refusal for a request no kernel could accept, made before any call. The
/// cause names the check, not a syscall, so the message does not claim a
/// call that was never made (journal §4.4).
MappingRefused refuse_unasked(Stage stage, const MappingRequest& request, std::string_view check,
                              std::string detail) {
  SyscallError cause{EINVAL, check, std::move(detail)};
  return refuse(stage, request, std::move(cause));
}

}  // namespace

core::Result<Mapping, MappingRefused> Memory::map(const MappingRequest& request) {
  std::size_t bytes = request.bytes;
  int flags = 0;
  if (request.huge_pages == HugePages::kExplicit) {
    const std::optional<std::size_t> rounded = round_up_to_huge_pages(request.bytes);
    if (!rounded.has_value()) {
      return refuse_unasked(Stage::kMap, request, "huge-page rounding",
                            std::to_string(request.bytes) + " bytes rounds past the largest size");
    }
    bytes = rounded.value();
    flags = kExplicitHugePageFlags;
  }

  auto mapped = syscalls_->map_anonymous(bytes, flags);
  if (!mapped.has_value()) {
    return refuse(Stage::kMap, request, mapped.error());
  }
  // From here on, every failure unmaps: the Mapping does it on the way out.
  Mapping mapping(*syscalls_, mapped.value(), bytes);

  if (request.node.has_value()) {
    const auto nodes = node_mask_of(request.node.value());
    if (!nodes.has_value()) {
      return refuse_unasked(
          Stage::kBind, request, "node mask",
          "node " + std::to_string(request.node.value()) + " is " + std::string{nodes.error()});
    }
    auto bound = syscalls_->bind_memory(mapping.address(), bytes, nodes.value());
    if (!bound.has_value()) {
      return refuse(Stage::kBind, request, bound.error());
    }
  }

  if (request.huge_pages == HugePages::kTransparent) {
    auto advised = syscalls_->advise_memory(mapping.address(), bytes, MADV_HUGEPAGE);
    if (!advised.has_value()) {
      return refuse(Stage::kAdvise, request, advised.error());
    }
  }

  if (request.populate) {
    // MADV_POPULATE_WRITE rather than MAP_POPULATE: the flag's failure is
    // silent -- mmap succeeds and the pages are simply not there -- and a
    // memory test that believed its pages were resident when they were not
    // would measure the page-fault handler. The madvise reports.
    auto populated = syscalls_->advise_memory(mapping.address(), bytes, MADV_POPULATE_WRITE);
    if (!populated.has_value()) {
      return refuse(Stage::kPopulate, request, populated.error());
    }
  }

  if (request.lock) {
    auto locked = syscalls_->lock_memory(mapping.address(), bytes);
    if (!locked.has_value()) {
      return refuse(Stage::kLock, request, locked.error());
    }
  }

  return mapping;
}

}  // namespace loadforge::platform
