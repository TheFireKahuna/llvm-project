//===- va_region_desc.h - va_tracker Layer 1 leaf descriptor ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-region descriptor (\c RegionDesc) — the leaf payload pointed to by every
/// interval-skiplist node's \c value atomic in the va_tracker. One cache line
/// carrying per-desc state (view protection, section offset, shape, flags,
/// NUMA mask) plus an 8-byte \c BackingRef into the shared \c DescBacking
/// partition that owns the kernel-resident handles and placeholder identity
/// for the source mapping.
///
/// Multiple descs that share a kernel reservation — fragments produced by
/// partial unmap, MAP_FIXED carve, mprotect-with-shape-change — all carry the
/// SAME encoded \c BackingRef. The kernel placeholder is split zero times and
/// the kernel handles are duplicated zero times: one allocation, one free,
/// no aliasing.
///
/// Body lifetime is governed by Crystalline-W via
/// \c g_va_tracker_skiplist_domain — the owning skiplist node retires this
/// descriptor through the same domain it uses for its own retirement, so
/// there is no separate per-desc domain. Kernel-state teardown is owned by
/// the synchronous mutator path (the va_tracker Transaction commit), not by
/// the FreeFn; see Nikolaev & Ravindran, "Crystalline: Fast and Memory
/// Efficient Wait-Free Reclamation," PLDI 2024, §1, which establishes that
/// Crystalline-W offers no synchronous grace primitive.
///
/// The reader contract is field-reader, not VA-reader. Consumers under a
/// Crystalline pin observe consistent \c RegionDesc fields throughout their
/// read window, decide what to do, and dereference the underlying VA only
/// through a separate kernel call — NT rejects such a call with
/// \c STATUS_INVALID_HANDLE or \c STATUS_NOT_MAPPED_VIEW if the mutator has
/// already torn the VA down. Pinned readers never dereference user content
/// directly.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_REGION_DESC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_REGION_DESC_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

/// Syscall-recipe taxonomy picked at region creation time.
///
/// The numeric values are wire-stable across the va_tracker and the higher
/// \c memory::RegionShape consumer. Any new shape must take a fresh number
/// at the tail of the enum and be added to both surfaces until the consumer
/// alias is retired.
enum class RegionShape : uint16_t {
  /// Sentinel for default-constructed slots; never produced by a successful
  /// acquire path.
  NONE = 0,

  // 1 reserved (was ANON_ONESHOT; retired).

  /// Anonymous private/shared mapping backed by an NT placeholder reservation
  /// committed via \c NtAllocateVirtualMemoryEx. The canonical
  /// \c mmap(MAP_ANONYMOUS) shape.
  ANON_PLACEHOLDER = 2,

  /// File-backed mapping with a single contiguous section view spanning the
  /// region. Produced by \c mmap(fd, ...) when no partial-unmap or carve has
  /// fragmented the range.
  FILE_VIEW_MONO = 3,

  /// File-backed mapping split into multiple section views, typically after
  /// MAP_FIXED carve or partial unmap of a previously MONO region. View
  /// boundaries live on the owning skiplist nodes; this desc covers a single
  /// fragment.
  FILE_VIEW_CHUNKED = 4,

  /// File-backed mapping carried as a placeholder reservation only — the
  /// section view is materialised lazily on first fault. Used for
  /// MAP_NORESERVE and large precommitted file ranges.
  FILE_VIEW_RESERVE = 5,

  // 6 reserved (was MIXED; never produced — do not reuse without an owner
  // story).

  /// Foreign VA seen by the resolver but not owned by the libc — third-party
  /// mappings introduced by direct \c NtMapViewOfSection calls outside the
  /// tracker. Carried so SIGSEGV classification can distinguish "unknown VA"
  /// from "known foreign VA."
  FOREIGN_SENTINEL = 7,

  /// Anonymous mapping backed by a libc-created section (for shared anonymous
  /// fork survival and \c shm_open). Distinguished from \c ANON_PLACEHOLDER
  /// because the kernel handle is a section handle, not a raw reservation.
  ANON_RESERVE_SECTION = 8,

  /// Libc-internal arena VA. Tracked so SIGSEGV classification can refuse to
  /// invoke user handlers on memory the runtime owns.
  LIBC_INTERNAL = 9,

  /// Loaded PE image regions (the EXE plus mapped DLLs). Read-only in the
  /// tracker; teardown is owned by the PE loader.
  IMAGE_REGION = 10,

  /// Kernel-mapped regions (KUSER_SHARED_DATA, PEB, TEBs). Read-only in the
  /// tracker; the kernel owns lifetime.
  KERNEL_REGION = 11,
};

/// Static per-region attribute flags, set at acquire time and immutable after
/// the desc is published. Mutating ops (\c protect(), \c set_numa_interleave()
/// etc.) clone the descriptor and Swap a fresh skiplist node rather than
/// rewriting flags in place.
namespace region_flag {

inline constexpr uint16_t COW = 0x1;              ///< Copy-on-write.
inline constexpr uint16_t SHARED = 0x2;           ///< MAP_SHARED visibility.
inline constexpr uint16_t HUGE_PAGES = 0x4;       ///< Backed by large pages.
inline constexpr uint16_t NORESERVE = 0x8;        ///< MAP_NORESERVE (lazy).
inline constexpr uint16_t NUMA_INTERLEAVE = 0x10; ///< NUMA interleave active.
inline constexpr uint16_t COMMITTED = 0x20;       ///< Fully committed.

/// Guard pages installed via MADV_GUARD_INSTALL. The fault handler raises
/// SIGSEGV (or re-arms PAGE_NOACCESS) on first touch; MADV_GUARD_REMOVE
/// clears the bit and restores baseline protection. Hardware-permanent
/// guards use PAGE_NOACCESS — PAGE_GUARD is one-shot self-clearing and
/// would expose every guarded page after a single trip through the fault
/// handler.
inline constexpr uint16_t PROT_GUARD = 0x40;

/// MADV_DONTFORK: the region is omitted from the child's tracker by
/// `va_tracker_fork_reinit`. The kernel's CoW inheritance of the parent
/// reservation is released by the drift-detection sweep.
inline constexpr uint16_t DONTFORK = 0x80;

/// MADV_WIPEONFORK: child-side desc is materialised, then the region is
/// zero-filled (commit_replace recycle) under the LOCKED envelope before
/// child code runs.
inline constexpr uint16_t WIPEONFORK = 0x100;

/// MAP_32BIT: the reservation is constrained to the low 2 GiB
/// (`nt_pal::reserve_placeholder_32bit`). Carried so a future mremap-grow
/// or mremap-move preserves the constraint when re-acquiring.
inline constexpr uint16_t LOW_32BIT = 0x200;

/// MADV_DONTDUMP: exclude the region from WER crash dumps. The PEB WER
/// gather-list write is the durable side effect; this bit records the
/// flag for paired MADV_DODUMP, which clears it before re-including.
inline constexpr uint16_t DUMP_EXCLUDE = 0x400;

} // namespace region_flag

/// Per-region leaf descriptor for the va_tracker's interval skiplist.
///
/// One cache line, owned by the skiplist node whose \c value atomic publishes
/// it. The class inherits the 24-byte \c CrystallineNode header so the
/// descriptor rides the SMR substrate's batch-retire protocol without a
/// per-allocation overhead.
///
/// The class carries several invariants worth noting:
///   * Body lifetime is governed by Crystalline-W via
///     \c g_va_tracker_skiplist_domain. Pinned readers observe consistent
///     fields throughout their read window; the FreeFn
///     (\c region_desc_release) is metadata-only — it validates canaries,
///     zeroes the slot, and returns it to the partition pool. No \c nt_pal
///     calls; no \c NtClose.
///   * Kernel-resident state (placeholder identity, section handle, file
///     handle) lives on the shared \c DescBacking referenced via
///     \c backing_ref. Multiple fragment descs that share a single kernel
///     reservation carry the SAME encoded \c BackingRef value.
///   * Kernel-state teardown is run synchronously by the Transaction whose
///     commit retires this desc, never from the FreeFn. Crystalline-W is
///     asynchronous by construction (Nikolaev & Ravindran, PLDI 2024 §1) and
///     offers no synchronous grace primitive that could be used to safely
///     issue kernel calls from a FreeFn.
///   * Range bounds (low / high VA) live on the owning skiplist node's
///     \c lo / \c hi fields; the partition tree is itself the range index.
///     There is no descriptor-side replication of the range.
///   * Partial unmap splits the interval: each fragment becomes its own
///     skiplist node pointing at its own \c RegionDesc, with every fragment
///     carrying the same \c BackingRef into the shared kernel state.
///   * Published descs are immutable. \c shape and \c flags are atomic only
///     to give pinned readers a consistent view across the brief window
///     between Swap-CAS success and the dying desc's \c INVALIDATED stamp;
///     the race is benign — readers see consistent OLD-desc fields under
///     their pin's grace.
///
/// The reader contract is field-reader, not VA-reader: dereference user
/// content only via a separate kernel call so that NT rejection
/// (\c STATUS_INVALID_HANDLE / \c STATUS_NOT_MAPPED_VIEW) makes synchronous
/// teardown by the mutator invisible to grace-deferred readers.
///
/// Memory layout (one cache line, validated by \c static_assert):
/// \code
///   offset
///   [ 0..19]  CrystallineNode body (next/slot/birth_era union, refs/
///             batch_next union, batch_link u32 at 16..19)
///   [20..23]  view_prot          — tail-pad-reuse slot, NT-DWORD wide
///   [24..31]  backing_ref        — encoded (slot_idx | generation)
///   [32..39]  section_offset     — LARGE_INTEGER, per-desc
///   [40..41]  shape              — atomic uint16
///   [42..43]  flags              — atomic uint16
///   [44..47]  numa_interleave_mask
///   [48..55]  node_canary        — partition_secret-derived
///   [56..63]  reserved           — forward-compat headroom
/// \endcode
///
/// \see desc_backing.h for the \c BackingRef encoding and dereference contract.
struct alignas(64) RegionDesc
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // Intrusive Crystalline-W runtime fields (next/slot/birth_era union,
  // refs/batch_next union, batch_link). Emitted directly via
  // LIBC_CRYSTALLINE_NODE_FIELDS so RegionDesc remains standard-layout —
  // keeps `offsetof` unconditionally supported per [support.types.layout]/1.
  LIBC_CRYSTALLINE_NODE_FIELDS(RegionDesc);

  /// Base NT page protection for the section view at this range — the value
  /// last passed as the \c Protect argument to \c NtMapViewOfSectionEx for
  /// this descriptor. Set at Transaction commit time from
  /// \c AcquireMeta::view_prot and never mutated post-publish; mprotect
  /// changes route through clone-and-Swap, which produces a fresh desc with
  /// the new view_prot.
  ///
  /// Full 32-bit DWORD width matches NT's \c Protect argument exactly. Lives
  /// at offset 20 as a tail-pad-reuse slot under the Itanium / MSVC ABI's
  /// non-standard-layout tail-padding-reuse rule, packed by natural
  /// alignment immediately after \c batch_link at offset 16.
  uint32_t view_prot{0};

  /// Encoded \c (slot_idx | generation) reference into the \c DescBacking
  /// partition. Zero is the null sentinel. The referenced backing owns this
  /// desc's section handle, file handle, and placeholder identity; multiple
  /// descs that share an underlying kernel reservation all carry the same
  /// \c BackingRef value.
  ///
  /// Dereferencing kernel-state fields is a two-step under a Crystalline pin
  /// on \c g_va_tracker_backing_domain:
  ///
  /// \code{.cpp}
  ///   auto *b = deref_backing(desc->backing_ref);
  ///   HANDLE sec = b->section_handle.load(MemoryOrder::ACQUIRE);
  /// \endcode
  ///
  /// The pin on \c g_va_tracker_skiplist_domain that keeps this desc alive
  /// does \b not extend to the backing — the caller must hold the backing
  /// pin for the duration of the dereference.
  uint64_t backing_ref{0};

  /// Section offset for this view, as an NT \c LARGE_INTEGER. Partial unmaps
  /// that split a region produce two \c RegionDescs whose \c section_offset
  /// differs by the bytes the head fragment consumed. Per-desc because the
  /// offset shift is unique per fragment — it cannot share with the backing.
  LARGE_INTEGER section_offset{};

  /// Region shape tag (\c RegionShape numeric value). Atomic because the
  /// rare promotion paths (e.g., MONO → CHUNKED on first split) write through
  /// a published descriptor; readers observe ACQUIRE-fenced.
  cpp::Atomic<uint16_t> shape{static_cast<uint16_t>(RegionShape::NONE)};

  /// Flag bitset of \c region_flag::* constants. Written at acquire time via
  /// the Transaction commit path and never mutated post-publish in place.
  /// Atomic for the C++ memory model — concurrent readers may pin the desc
  /// and read flags during the brief window between Swap-CAS success and the
  /// dying desc's \c INVALIDATED stamp; the race is benign because the
  /// reader sees consistent OLD-desc fields under its pin.
  cpp::Atomic<uint16_t> flags{};

  /// NUMA interleave mask for \c set_numa_interleave() / \c mbind() /
  /// \c set_mempolicy(). One bit per NUMA node (LSB = node 0); zero means
  /// kernel default policy.
  ///
  /// Full 32-bit width covers 32 NUMA nodes, well above every production
  /// Windows host (highest-NUMA SKUs cap out at 12–16 nodes). POSIX
  /// \c mbind can express more in principle; the libc-internal representation
  /// truncates beyond bit 31 at acquire time.
  uint32_t numa_interleave_mask{0};

  /// Per-slot canary derived from
  /// \c partition_secret^class_id^chunk_id^slot_idx at allocation time;
  /// validated in \c region_desc_release before any chunk-descriptor
  /// dereference. Closes a heap-spray attack surface where an attacker
  /// writing into a freed-but-not-reused slot crafts a bogus
  /// \c chunk_id / \c slot_idx to redirect the FreeFn into a victim chunk's
  /// bitmap. \c partition_secret is \c ProcessPrng-derived and lives in
  /// sealed PCB Zone 0 — not observable to user code.
  uint64_t node_canary{0};

  /// Forward-compat headroom. Reserved for future hardening (quarantine
  /// epoch, GWP-ASan tag, telemetry counter) without disturbing the
  /// slot-size invariant.
  uint64_t reserved{0};

  // ---- Helpers ---------------------------------------------------------

  /// Returns the current shape under an ACQUIRE fence so the caller can pair
  /// it with subsequent reads of fields published before the shape was
  /// stamped (e.g., \c section_offset on a MONO → CHUNKED promotion).
  [[nodiscard]] LIBC_INLINE RegionShape current_shape() const {
    uint16_t v;
    __atomic_load(&shape.val, &v, __ATOMIC_ACQUIRE);
    return static_cast<RegionShape>(v);
  }

  /// Returns the flags bitset with an ACQUIRE fence.
  [[nodiscard]] LIBC_INLINE uint16_t flags_load() {
    return flags.load(cpp::MemoryOrder::ACQUIRE);
  }

  /// Returns true if every bit in \p bit is set in the current flags.
  [[nodiscard]] LIBC_INLINE bool has_flag(uint16_t bit) {
    return (flags_load() & bit) != 0;
  }

  /// Returns true if this region is backed by a file-derived section view in
  /// any of its three file shapes (MONO, CHUNKED, RESERVE).
  [[nodiscard]] LIBC_INLINE bool is_file_backed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::FILE_VIEW_MONO ||
           s == RegionShape::FILE_VIEW_CHUNKED ||
           s == RegionShape::FILE_VIEW_RESERVE;
  }

  /// Returns true if this region is backed by an NT section object —
  /// includes the three file shapes plus the anonymous-section shape.
  [[nodiscard]] LIBC_INLINE bool is_section_backed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::FILE_VIEW_MONO ||
           s == RegionShape::FILE_VIEW_CHUNKED ||
           s == RegionShape::FILE_VIEW_RESERVE ||
           s == RegionShape::ANON_RESERVE_SECTION;
  }

  /// Returns true if this region is anonymous — either a placeholder commit
  /// or a libc-owned anonymous section.
  [[nodiscard]] LIBC_INLINE bool is_anonymous() const {
    const RegionShape s = current_shape();
    return s == RegionShape::ANON_PLACEHOLDER ||
           s == RegionShape::ANON_RESERVE_SECTION;
  }

  /// Returns true if this region is a foreign (libc-unowned) mapping.
  [[nodiscard]] LIBC_INLINE bool is_foreign() const {
    return current_shape() == RegionShape::FOREIGN_SENTINEL;
  }

  /// Returns true if the COW flag is set.
  [[nodiscard]] LIBC_INLINE bool is_cow() {
    return has_flag(region_flag::COW);
  }
};

// ---- Layout invariants ----------------------------------------------------

static_assert(sizeof(RegionDesc) == 64,
              "va_tracker::RegionDesc must be exactly one cache line "
              "(slot size in PartitionClass::RegionDesc partition)");
static_assert(alignof(RegionDesc) == 64,
              "va_tracker::RegionDesc must be cache-line aligned");

static_assert(offsetof(RegionDesc, view_prot) == 20,
              "RegionDesc::view_prot must reuse the CrystallineNode "
              "tail-pad slot at offset 20 — the Itanium/MSVC ABI's "
              "tail-padding-reuse rule for non-standard-layout bases");
static_assert(offsetof(RegionDesc, backing_ref) == 24,
              "RegionDesc::backing_ref layout pin");
static_assert(offsetof(RegionDesc, section_offset) == 32,
              "RegionDesc::section_offset layout pin");
static_assert(offsetof(RegionDesc, shape) == 40,
              "RegionDesc::shape layout pin");
static_assert(offsetof(RegionDesc, flags) == 42,
              "RegionDesc::flags layout pin");
static_assert(offsetof(RegionDesc, numa_interleave_mask) == 44,
              "RegionDesc::numa_interleave_mask layout pin");
static_assert(offsetof(RegionDesc, node_canary) == 48,
              "RegionDesc::node_canary layout pin");
static_assert(offsetof(RegionDesc, reserved) == 56,
              "RegionDesc::reserved layout pin");

// The Crystalline-W FreeFn explicitly clears the slot via __builtin_memset
// before returning it to the bitmap, so the type can hold no C++ object
// state that would need a destructor.
static_assert(__is_trivially_destructible(RegionDesc),
              "va_tracker::RegionDesc must be trivially destructible "
              "for partition slot recycling");

struct VaChunkDesc;

/// Visitor signature for the fork-reinit chunk walk. Invoked once per live
/// \c VaChunkDesc reachable from the \c RegionDesc class's chunk table.
using RegionDescForkChunkVisitor = void (*)(VaChunkDesc *cd, void *ctx);

/// Fork-reinit phase for the \c RegionDesc subsystem.
///
/// Walks the per-class chunk table, refreshes per-chunk and per-slot canaries
/// against the rotated \c partition_secret, and invokes \p visit on every
/// reachable \c VaChunkDesc (used by the master reinit hook to build its
/// leaked-descriptor reclaim bitmap).
///
/// \param visit Per-chunk visitor; pass \c nullptr to skip the visit step.
/// \param ctx Opaque context forwarded to \p visit.
void region_desc_fork_reinit_phase(RegionDescForkChunkVisitor visit,
                                   void *ctx);

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_REGION_DESC_H
