//===- va_region_desc.h - va_tracker Layer 1 leaf descriptor ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-region descriptor (RegionDesc) — the leaf payload pointed to by every
// interval-skiplist node's value atomic in the va_tracker. Body lifetime is
// governed by Crystalline-W via g_va_tracker_skiplist_domain; kernel-resident
// handles and placeholder identity live on a shared DescBacking referenced
// via an 8-byte encoded BackingRef. Fragment descs produced by partial unmap
// or MAP_FIXED carve carry the SAME BackingRef value — one kernel allocation,
// one free, zero aliasing.
//
// Kernel-state teardown is synchronous on the mutator path (Transaction
// commit), never the FreeFn — Crystalline-W is asynchronous by construction
// and offers no synchronous grace primitive (Nikolaev & Ravindran,
// "Crystalline: Fast and Memory Efficient Wait-Free Reclamation,"
// PLDI 2024, §1).
//
// The reader contract is field-reader, not VA-reader: pinned readers observe
// consistent RegionDesc fields, then dereference the underlying VA only via
// a separate kernel call so that NT rejection (STATUS_INVALID_HANDLE /
// STATUS_NOT_MAPPED_VIEW) makes synchronous mutator teardown invisible to
// grace-deferred readers.
//
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

// Syscall-recipe taxonomy picked at region creation time. Numeric values are
// wire-stable across the va_tracker and the higher memory::RegionShape
// consumer; new shapes append at the tail.
enum class RegionShape : uint16_t {
  NONE = 0, // Default-constructed sentinel; never produced by acquire.

  // 1 reserved (was ANON_ONESHOT; retired).

  // mmap(MAP_ANONYMOUS): NT placeholder committed via
  // NtAllocateVirtualMemoryEx.
  ANON_PLACEHOLDER = 2,

  // mmap(fd) producing a single contiguous section view.
  FILE_VIEW_MONO = 3,

  // File mapping split into multiple section views by MAP_FIXED carve or
  // partial unmap; each fragment is its own desc.
  FILE_VIEW_CHUNKED = 4,

  // File placeholder reservation; section view materialised lazily on first
  // fault. Used for MAP_NORESERVE and large precommitted file ranges.
  FILE_VIEW_RESERVE = 5,

  // 6 reserved (was MIXED; never produced — do not reuse without an owner
  // story).

  // Third-party mapping introduced by a direct NtMapViewOfSection outside
  // the tracker; carried so SIGSEGV classification can distinguish "unknown
  // VA" from "known foreign VA."
  FOREIGN_SENTINEL = 7,

  // Anonymous mapping backed by a libc-created section (shared anonymous
  // fork survival, shm_open). Distinguished from ANON_PLACEHOLDER because
  // the kernel handle is a section, not a raw reservation.
  ANON_RESERVE_SECTION = 8,

  // Libc-internal arena VA. SIGSEGV classification refuses to invoke user
  // handlers on memory the runtime owns.
  LIBC_INTERNAL = 9,

  // Loaded PE images (EXE + DLLs). Read-only in the tracker; the PE loader
  // owns teardown.
  IMAGE_REGION = 10,

  // Kernel-mapped regions (KUSER_SHARED_DATA, PEB, TEBs). Read-only in the
  // tracker; the kernel owns lifetime.
  KERNEL_REGION = 11,
};

// Static per-region attribute flags, set at acquire time and immutable after
// publish. Mutating ops clone the descriptor and Swap a fresh skiplist node
// rather than rewriting in place.
namespace region_flag {

inline constexpr uint16_t COW = 0x1;              // Copy-on-write.
inline constexpr uint16_t SHARED = 0x2;           // MAP_SHARED visibility.
inline constexpr uint16_t HUGE_PAGES = 0x4;       // Backed by large pages.
inline constexpr uint16_t NORESERVE = 0x8;        // MAP_NORESERVE (lazy).
inline constexpr uint16_t NUMA_INTERLEAVE = 0x10; // NUMA interleave active.
inline constexpr uint16_t COMMITTED = 0x20;       // Fully committed.

// MADV_GUARD_INSTALL: hardware-permanent guard via PAGE_NOACCESS, not
// PAGE_GUARD (PAGE_GUARD is one-shot self-clearing and would expose every
// guarded page after a single trip through the fault handler).
inline constexpr uint16_t PROT_GUARD = 0x40;

// MADV_DONTFORK: omitted from the child's tracker by va_tracker_fork_reinit;
// the kernel's CoW inheritance is released by drift-detection sweep.
inline constexpr uint16_t DONTFORK = 0x80;

// MADV_WIPEONFORK: child desc materialised then zero-filled under the
// LOCKED envelope before child code runs.
inline constexpr uint16_t WIPEONFORK = 0x100;

// MAP_32BIT: reservation constrained to low 2 GiB via
// nt_pal::reserve_placeholder_32bit. Carried so future mremap-grow or
// mremap-move preserves the constraint when re-acquiring.
inline constexpr uint16_t LOW_32BIT = 0x200;

// MADV_DONTDUMP: exclude from WER crash dumps. The PEB WER gather-list
// write is the durable effect; this bit records the flag for paired
// MADV_DODUMP to clear before re-including.
inline constexpr uint16_t DUMP_EXCLUDE = 0x400;

// LOCK_ONFAULT: arm lock-on-first-touch. The fault handler reads this
// bit on every demand-commit and guard-page violation that resolves to a
// tracked region; on hit it calls nt_pal::lock_range for the faulting
// page. Per-desc rather than a side table because the resolver fast path
// already loads the desc — no RW lock, no sorted-range search, no PAGE_GUARD
// out-of-band dispatch. POSIX forbids lock survival across fork;
// serialize_for_fork strips this bit before emitting to the child snapshot.
inline constexpr uint16_t LOCK_ONFAULT = 0x800;

// 0x1000 unused. Was PROT_DIVERGED; substrate no longer caches current
// protection on the desc (consumers query MBI; mprotect leaves desc
// untouched), so a divergence signal has no purpose.

} // namespace region_flag

// Per-region leaf descriptor for the va_tracker's interval skiplist. One
// cache line, owned by the skiplist node whose `value` atomic publishes it.
// Inherits the 24-byte CrystallineNode header so the desc rides the SMR
// substrate's batch-retire protocol with no per-allocation overhead.
//
// Invariants worth flagging:
//   * Range bounds live on the skiplist node's lo/hi; the partition tree is
//     the range index. No descriptor-side replication.
//   * Published descs are immutable. `shape` and `flags` are atomic only to
//     give pinned readers a consistent view across the brief window between
//     Swap-CAS success and the dying desc's INVALIDATED stamp — readers see
//     consistent OLD-desc fields under their pin's grace.
//   * Partial unmap splits the interval: each fragment becomes its own
//     skiplist node pointing at its own RegionDesc, all sharing one
//     BackingRef.
struct alignas(64) RegionDesc
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // Intrusive Crystalline-W runtime fields emitted via the macro so RegionDesc
  // stays standard-layout — keeps offsetof unconditionally supported per
  // C++23 [support.types.layout]/1.
  LIBC_CRYSTALLINE_NODE_FIELDS(RegionDesc);

  // Acquire-time protection intent (the value passed as
  // AcquireMeta::view_prot at the original acquire/replace commit).
  // Never updated post-publish. mprotect changes affect only kernel-side
  // per-page protection; the kernel is source of truth and consumers query
  // MBI for current protection. This field's role is reduced to (a) a
  // "what was asked for at acquire time" hint used by replace's
  // outside-survivor uniformity check and (b) an MBI-query fallback when
  // the kernel query fails.
  //
  // Lives at offset 20 as a tail-pad-reuse slot under the Itanium/MSVC ABI's
  // non-standard-layout tail-padding-reuse rule; 32-bit DWORD width matches
  // NT's `Protect` argument exactly.
  uint32_t view_prot{0};

  // Encoded (slot_idx | generation) reference into the DescBacking
  // partition; zero is the null sentinel. The pin on
  // g_va_tracker_skiplist_domain that keeps this desc alive does NOT extend
  // to the backing — callers must hold a g_va_tracker_backing_domain pin
  // for the duration of any kernel-handle dereference. See desc_backing.h
  // for the engine vs reader access discipline.
  uint64_t backing_ref{0};

  // NT LARGE_INTEGER section offset for this view. Partial unmaps that
  // split a region produce two descs whose section_offset differs by the
  // head fragment's byte length. Per-desc rather than on the backing
  // because the offset shift is unique per fragment.
  LARGE_INTEGER section_offset{};

  // Region shape tag. Atomic so a pinned reader observing the OLD desc
  // through the brief Swap-CAS-success-to-INVALIDATED-stamp window sees
  // a consistent value; never rewritten in place after publish — fresh
  // shapes ride on freshly-cloned descs through Swap-CAS.
  cpp::Atomic<uint16_t> shape{static_cast<uint16_t>(RegionShape::NONE)};

  // region_flag::* bitset. Same publish-window discipline as shape;
  // never rewritten in place post-publish.
  cpp::Atomic<uint16_t> flags{};

  // One bit per NUMA node (LSB = node 0); zero means kernel default policy.
  // 32-bit width covers every production Windows host (highest-NUMA SKUs
  // cap at 12–16 nodes); POSIX mbind can express more in principle but the
  // libc-internal representation truncates beyond bit 31 at acquire time.
  uint32_t numa_interleave_mask{0};

  // Per-slot canary derived from
  // partition_secret^class_id^chunk_id^slot_idx at allocation time;
  // validated in region_desc_release before any chunk-descriptor
  // dereference. Closes a heap-spray attack surface where an attacker
  // writing into a freed-but-not-reused slot crafts a bogus chunk_id/
  // slot_idx to redirect the FreeFn into a victim chunk's bitmap.
  // partition_secret is ProcessPrng-derived and lives in sealed PCB Zone 0.
  uint64_t node_canary{0};

  // Forward-compat headroom for future hardening (quarantine epoch,
  // GWP-ASan tag, telemetry counter) without disturbing the slot-size
  // invariant.
  uint64_t reserved{0};

  // ---- Helpers ---------------------------------------------------------

  // ACQUIRE-loaded so a reader that has only the desc pointer (no
  // skiplist-load fence in scope) still pairs with the writer-side
  // RELEASE store performed before publish. Raw `__atomic_load` rather
  // than `shape.load()` because cpp::Atomic::load is non-const and this
  // helper is const.
  [[nodiscard]] LIBC_INLINE RegionShape current_shape() const {
    uint16_t v;
    __atomic_load(&shape.val, &v, __ATOMIC_ACQUIRE);
    return static_cast<RegionShape>(v);
  }

  [[nodiscard]] LIBC_INLINE uint16_t flags_load() {
    return flags.load(cpp::MemoryOrder::ACQUIRE);
  }

  [[nodiscard]] LIBC_INLINE bool has_flag(uint16_t bit) {
    return (flags_load() & bit) != 0;
  }

  [[nodiscard]] LIBC_INLINE bool is_file_backed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::FILE_VIEW_MONO ||
           s == RegionShape::FILE_VIEW_CHUNKED ||
           s == RegionShape::FILE_VIEW_RESERVE;
  }

  // True for any NT section-backed shape — the three file shapes plus the
  // anonymous-section shape.
  [[nodiscard]] LIBC_INLINE bool is_section_backed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::FILE_VIEW_MONO ||
           s == RegionShape::FILE_VIEW_CHUNKED ||
           s == RegionShape::FILE_VIEW_RESERVE ||
           s == RegionShape::ANON_RESERVE_SECTION;
  }

  // True for anonymous shapes — placeholder commit or libc-owned section.
  [[nodiscard]] LIBC_INLINE bool is_anonymous() const {
    const RegionShape s = current_shape();
    return s == RegionShape::ANON_PLACEHOLDER ||
           s == RegionShape::ANON_RESERVE_SECTION;
  }

  [[nodiscard]] LIBC_INLINE bool is_foreign() const {
    return current_shape() == RegionShape::FOREIGN_SENTINEL;
  }

  [[nodiscard]] LIBC_INLINE bool is_cow() {
    return has_flag(region_flag::COW);
  }
};

// ---- Layout invariants ----------------------------------------------------
//
// RegionDesc is the slot type for the VaTrackerRegionDesc partition; slot
// recovery is by index arithmetic against a fixed slot size, so any size or
// offset drift desynchronises the FreeFn from the partition's bitmap.

static_assert(sizeof(RegionDesc) == 64,
              "RegionDesc must be exactly one cache line");
static_assert(alignof(RegionDesc) == 64,
              "RegionDesc must be cache-line aligned");

// view_prot at 20 is the Itanium/MSVC tail-pad-reuse slot — packed by natural
// alignment immediately after batch_link at offset 16 in the non-standard-
// layout base.
static_assert(offsetof(RegionDesc, view_prot) == 20,
              "RegionDesc::view_prot must reuse CrystallineNode tail-pad");
static_assert(offsetof(RegionDesc, backing_ref) == 24, "backing_ref offset");
static_assert(offsetof(RegionDesc, section_offset) == 32,
              "section_offset offset");
static_assert(offsetof(RegionDesc, shape) == 40, "shape offset");
static_assert(offsetof(RegionDesc, flags) == 42, "flags offset");
static_assert(offsetof(RegionDesc, numa_interleave_mask) == 44,
              "numa_interleave_mask offset");
static_assert(offsetof(RegionDesc, node_canary) == 48, "node_canary offset");
static_assert(offsetof(RegionDesc, reserved) == 56, "reserved offset");

// FreeFn clears the slot via __builtin_memset before returning it to the
// bitmap, so the type can hold no C++ state needing a destructor.
static_assert(__is_trivially_destructible(RegionDesc),
              "RegionDesc must be trivially destructible for slot recycling");

struct VaChunkDesc;

// Visitor for region_desc_fork_reinit_phase; invoked once per live
// VaChunkDesc reachable from the class's chunk table.
using RegionDescForkChunkVisitor = void (*)(VaChunkDesc *cd, void *ctx);

// Walks the per-class chunk table, refreshes per-chunk and per-slot canaries
// against the rotated partition_secret, and invokes `visit` on every
// reachable VaChunkDesc. The master reinit hook uses the visit callback to
// build its leaked-descriptor reclaim bitmap; pass nullptr to skip the visit.
void region_desc_fork_reinit_phase(RegionDescForkChunkVisitor visit,
                                   void *ctx);

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_VA_REGION_DESC_H
