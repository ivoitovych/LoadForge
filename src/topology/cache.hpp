// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_CACHE_HPP
#define LOADFORGE_TOPOLOGY_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/cpu_topology.hpp"

namespace loadforge::topology {

/// What a cache holds. The three values `cache/indexN/type` ever contains.
enum class CacheType : std::uint8_t { kData, kInstruction, kUnified };

/// The word for a CacheType, and the exact text sysfs writes for it.
[[nodiscard]] std::string_view describe(CacheType type) noexcept;

/// What sort of cache: its level and what it holds.
///
/// Not an identity on its own -- every CPU has an L1 Data cache -- but half of
/// one. The other half is WHICH CPUs share it; see Cache.
struct CacheKind {
  std::uint32_t level = 0;
  CacheType type = CacheType::kUnified;

  [[nodiscard]] friend bool operator==(const CacheKind&, const CacheKind&) = default;
};

/// One cache, as the kernel describes it.
///
/// WHAT IDENTIFIES A CACHE, AND WHY IT IS NOT `id`
/// ----------------------------------------------
/// A cache is identified by its kind and the set of CPUs that share it. Two
/// entries with the same level, type and `shared_cpu_list` are two views of one
/// piece of silicon, read through two CPUs; that is the definition of a shared
/// cache, and it is what deduplication keys on.
///
/// sysfs also offers `cache/indexN/id`, and the first version of this module
/// keyed on that instead. It was wrong on every arm64 machine: the kernel only
/// publishes `id` when the architecture supplies one (x86 derives it from
/// CPUID; arm64 generally does not), and a reader that required it refused
/// hardware that was describing itself perfectly well. Found by CI's arm64
/// runner -- the first x86 machine it ran on could not have shown it.
///
/// So `id` is OPTIONAL, and so are `size_bytes` and `line_bytes`: the kernel's
/// cacheinfo hides each of these attributes when it does not know the value,
/// rather than writing zero. Absent means "not known", and every one of these
/// is a legitimate reading of a real machine rather than a failure to read it.
/// `level`, `type` and `shared_cpu_list` are the ones always present for a
/// real cache, and the ones this module requires.
struct Cache {
  CacheKind kind;
  /// Every CPU this cache serves -- including the one it was read through, and
  /// the same set whichever of them it is read through. With `kind`, this IS
  /// the cache's identity.
  CpuList shared_with;

  std::optional<std::uint64_t> size_bytes;
  std::optional<std::uint32_t> line_bytes;
  /// The kernel's own id, when it publishes one. Corroboration, not identity:
  /// every CPU sharing a cache must report the same id for it (checked), but
  /// nothing here depends on the id being there.
  std::optional<std::uint32_t> id;

  [[nodiscard]] friend bool operator==(const Cache&, const Cache&) = default;
};

/// The machine's caches, deduplicated and cross-checked.
///
/// WHY THE SAME CACHE IS READ SEVERAL TIMES
/// ----------------------------------------
/// sysfs publishes a cache once per CPU that can see it: a shared L3 appears in
/// four CPUs' `cache/` directories on a four-CPU machine, describing ONE piece
/// of silicon. Reading all four first, rather than taking the first and moving
/// on, is what makes them CHECKABLE against each other; deduplication happens
/// only after the checks have passed.
///
/// WHAT IS CHECKED, AND WHAT DELIBERATELY IS NOT
/// --------------------------------------------
/// Checked, because each is certain of any machine:
///
///   * a cache's shared set contains the CPU it was read through;
///   * every CPU in that set is one the CPU topology found online;
///   * every CPU a cache claims to serve publishes an IDENTICAL record for it
///     -- same size, line size and id where known, same shared set. Sharing a
///     cache is an equivalence relation, exactly as sharing a core is;
///   * sharing only ever widens with level: the CPUs sharing a CPU's L1 are a
///     subset of those sharing its L2, and so on. That is the defining
///     structural property of a cache hierarchy.
///
/// NOT checked, and the omissions are deliberate rather than oversights:
///
///   * that every CPU reports the same set of levels. False on a heterogeneous
///     machine -- an arm64 big.LITTLE package can genuinely give its big and
///     little cores different cache depths -- and a check that refuses a real
///     machine is worse than no check at all.
///   * that sizes grow with level. Nearly always true and not guaranteed:
///     L1 instruction and L1 data caches differ in size at the SAME level
///     (32K and 48K here), and nothing forbids an unusual arrangement further
///     up. The nesting check above already catches the corruption this would.
///   * that `id`, where present, agrees with the shared set -- that two caches
///     of one kind with different sharers carry different ids. Probably true of
///     every kernel, and "probably" is the wrong standard for a check whose
///     failure mode is refusing a real machine.
class CacheHierarchy {
 public:
  /// Highest `cache/indexN` examined before giving up.
  ///
  /// The scan walks index0, index1, ... and stops at the first one that is
  /// absent, because sysfs numbers these contiguously from zero. The bound
  /// exists so that a tree which is NOT contiguous -- a directory of thousands
  /// of stray entries, a path that resolves somewhere unexpected -- cannot turn
  /// discovery into an unbounded walk. Real machines use four or five.
  static constexpr std::uint32_t kMaxCacheIndex = 16;

  /// Read and cross-check the machine's caches.
  ///
  /// Takes the CPU topology rather than re-reading `online`, so that the two
  /// halves of discovery cannot disagree about which CPUs exist -- and so that
  /// a cache naming a CPU the topology never saw is a contradiction this can
  /// actually detect.
  [[nodiscard]] static core::Result<CacheHierarchy, DiscoveryFailure> discover(
      platform::FileSystem& filesystem, const CpuTopology& cpus,
      std::string_view sysfs_root = kDefaultSysfsRoot);

  /// Distinct caches, ordered by level, then by the lowest CPU sharing them,
  /// then by type -- so a CPU's L1 Data and L1 Instruction sit together.
  [[nodiscard]] const std::vector<Cache>& caches() const noexcept { return caches_; }

  [[nodiscard]] std::size_t count() const noexcept { return caches_.size(); }

  /// How many distinct caches exist at a level. Zero if the level is absent,
  /// which is a real answer: not every machine has an L3.
  [[nodiscard]] std::size_t count_at_level(std::uint32_t level) const noexcept;

  /// The deepest level any cache reports, or zero when there are none.
  [[nodiscard]] std::uint32_t deepest_level() const noexcept;

  /// Total bytes across every distinct cache at a level, counted once each
  /// however many CPUs share them -- or nothing, if the size of any cache at
  /// that level is unknown. A partial sum would be a plausible wrong number,
  /// which is the one thing this module does not produce. A level with no
  /// caches totals zero, and that is known.
  [[nodiscard]] std::optional<std::uint64_t> total_bytes_at_level(
      std::uint32_t level) const noexcept;

 private:
  explicit CacheHierarchy(std::vector<Cache> caches) : caches_(std::move(caches)) {}

  std::vector<Cache> caches_;
};

/// Parses the size format sysfs writes for a cache, in bytes.
///
/// Exposed because it is where a judgement lives, and a format decision that
/// can only be reached through a directory walk is one nobody will keep correct.
///
/// This is NOT core::parse_byte_size, and the difference is not an oversight.
/// That function parses a value a USER wrote in a configuration file: it accepts
/// `KiB`/`MiB`/`GiB` and, deliberately, a percentage like `70%`. sysfs writes
/// neither -- it writes a bare `K`, which that parser rejects as an unknown unit
/// (verified: "48K" returns "unit suffix is not recognised"). Pointing the
/// config parser at a cache size would therefore fail on every real machine, and
/// teaching it `K` would additionally make `70%` a parseable cache size, which
/// is not a thing.
///
/// `K` means 1024, as everywhere in the kernel. A bare number is bytes, and `B`
/// is accepted because the kernel's own formatter switches to it below 1 KiB;
/// every level on every machine probed so far reports `K`.
[[nodiscard]] core::Result<std::uint64_t, std::string_view> parse_cache_size(std::string_view text);

}  // namespace loadforge::topology

#endif
