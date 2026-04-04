//===--- Logical-mmap region descriptor ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A Region is the logical-mmap-unit descriptor: one per `mmap` call, owning
// one or more slots in the mapping table. Every mapping-table slot carries a
// `region_id` (32-bit pool index) and `alloc_id` (8-bit generation). The
// region descriptor holds the single section/file handle duplicated once at
// acquire time, refcounted across all its slots and across fork / ring-buffer
// double-map.
//
// The shape field tells every downstream op (munmap, mprotect, mremap, fork)
// which syscall recipe to use, with zero runtime classification of the VA
// state. See the design plan at ~/.claude/plans/humble-roaming-lampson.md
// for the full rationale.
//
// Concurrency:
//   refcount     — atomic; add_ref / release from any thread
//   shape        — atomic; mutated by shape promotion (MONO -> CHUNKED) under
//                  MmapLock writer, read by everyone after seqlock fence
//   alloc_id     — atomic; written by try_claim under exclusive ownership of
//                  a freshly-CAS'd refcount slot, read by every snapshot
//                  resolver and wait_for_release path with ACQUIRE ordering
//                  so the read is naturally synchronized with the writer's
//                  RELEASE-published refcount transition
//   section/file — plain; written at acquire, immutable thereafter, closed
//                  when last ref drops (release caller must hold MmapLock
//                  writer so no reader is concurrently dereferencing)
//   chunk_list   — atomic pointer; null unless shape == FILE_VIEW_CHUNKED
//   range keys   — plain; mutated only under MmapLock writer during shape
//                  transitions
//
// Size invariant: one cache line (64 bytes). Pool-allocated via
// `IndexedPool<RegionDesc, 14>` (16 384 descriptors per chunk).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_DESC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_DESC_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory {

// ============================================================================
// Region shape — picks the syscall recipe at creation time.
// ============================================================================
//
// Shape is immutable for a region except for the MONO -> CHUNKED promotion
// that happens on the first partial munmap of a file view. All other state
// (refcount, bounds, chunk list) is derived from the shape.
//
// Numeric values are part of the snapshot protocol: readers copy shape via
// atomic load after the seqlock fence, so assigning stable small integers is
// convenient. Do not renumber without auditing every `switch (shape)` site.

enum class RegionShape : uint16_t {
  // Sentinel — region_id == 0 is never allocated; this value stamps the
  // null descriptor.
  NONE = 0,

  // Retired (value 1): ANON_ONESHOT — every anon allocation is now
  // placeholder-origin; use ANON_PLACEHOLDER with region_flag::COMMITTED
  // for the old one-shot semantics. Value reserved for wire stability.

  // Anonymous placeholder-origin VA. Three sub-flavors distinguished by
  // region_flag bits and slot state:
  //   * COMMITTED         — placeholder replaced with private committed VA
  //                         (state=LIVE). Kernel-identical to the retired
  //                         ANON_ONESHOT: partial munmap uses MEM_DECOMMIT,
  //                         whole munmap uses MEM_RELEASE.
  //   * NORESERVE         — placeholder replaced with MEM_RESERVE private
  //                         (state=LIVE, no commit). VEH commits on fault.
  //                         Partial munmap uses split_placeholder.
  //   * neither flag set  — bare placeholder (state=PLACEHOLDER, PROT_NONE).
  //                         Partial munmap uses split_placeholder.
  ANON_PLACEHOLDER = 2,

  // File-backed single view — placeholder + section + NtMapViewOfSection.
  // The common file-mmap shape. Partial munmap promotes this shape to
  // FILE_VIEW_CHUNKED on first split.
  FILE_VIEW_MONO = 3,

  // File-backed multi-view — single section handle, multiple view slots
  // within the region's bounding range. Produced by partial munmap of a
  // MONO region. Chunk list records the live ranges.
  FILE_VIEW_CHUNKED = 4,

  // File-backed with SEC_RESERVE — section is reserved but not committed.
  // VEH drives demand commit. Used for MAP_NORESERVE file mappings.
  FILE_VIEW_RESERVE = 5,

  // Reserved (was MIXED — deleted). MAP_FIXED is destructive: prepare_for_fixed
  // tears the covered range down and the new mapping installs as a single-
  // shape region. No producer ever generated MIXED, so the value is unused.
  // Reserved here to keep wire numbering stable; do not reuse without an
  // owner and a producer story.

  // Not ours — foreign placeholder or view created by the loader, heap,
  // thread pool, or user VirtualAlloc2. Cordoned from merge / hint /
  // release paths. refcount counts our FOREIGN slots stamped over the
  // range, not NT ownership of the VA.
  FOREIGN_SENTINEL = 7,

  // Anonymous, pagefile-backed SEC_RESERVE section — used by mremap to
  // splice growth capacity onto a region whose original NT allocation
  // cannot be extended in place. Has a section_handle (so split-remap
  // and the chunk-list machinery work the same way as for file views)
  // but no file_handle (the pagefile backing is anonymous from POSIX's
  // perspective: msync is a no-op, no fd, no inode).
  //
  // Distinct from FILE_VIEW_RESERVE because the absence of a file_handle
  // changes downstream behavior (msync, mincore reporting, fork CoW
  // semantics for the section's backing pages, /proc/self/maps formatting
  // when that lands).
  ANON_RESERVE_SECTION = 8,

  // Libc-internal anonymous VA — every `page_alloc` / `page_reserve`
  // / `page_commit` from every Tier B subsystem (SlabPool arenas,
  // IndexedPool chunks, brk, alpc bus buffers, fcntl lock table, fd
  // table backing, ...) stamps its result with this shape via the
  // INTERNAL_REGION_ID sentinel. The mapping table then knows every
  // page the libc itself has allocated. Policy: MAP_FIXED is rejected
  // with -EINVAL; reconcile skips (our lifecycle, no NT re-probe
  // needed).
  LIBC_INTERNAL = 9,

  // MEM_IMAGE-backed loaded module — c.dll, ntdll, the main exe,
  // every DLL loaded via LdrLoadDll. Stamped at `mapping_table_startup_init`
  // (Phase 0c.5) for pre-existing modules and maintained across
  // LdrLoadDll / LdrUnloadDll via the DLL notification callback.
  // Policy: MAP_FIXED rejected; reconcile skips (image is bolted to
  // its load address for the module's lifetime).
  IMAGE_REGION = 10,

  // Kernel-managed VA — TEB, PEB, main-thread stack reservation.
  // OS-owned, immutable for process lifetime (stack grows but never
  // relocates). Policy: MAP_FIXED rejected; reconcile skips. One
  // shape, no subtype enum — the distinction worth making here is
  // "kernel-managed, never relocates" vs "third-party, may shift".
  KERNEL_REGION = 11,
};

// ============================================================================
// Region flags — static attributes set at acquire time.
// ============================================================================
//
// Separate bit-field from shape because multiple flags can coexist (COW +
// SHARED, for instance, are mutually exclusive by MAP_* semantics but both
// are independent of shape numbering).

namespace region_flag {

// Section mapped PAGE_WRITECOPY. Splits must preserve dirty CoW pages via
// `CowContext` (region_snapshot.h).
inline constexpr uint16_t COW = 0x1;

// MAP_SHARED — the section backs shared state across processes. Fork
// preserves the shared view; writes are visible to all mappers.
inline constexpr uint16_t SHARED = 0x2;

// Section created with SEC_64K_PAGES or SEC_LARGE_PAGES. Alignment
// constraints apply to remap / protect operations.
inline constexpr uint16_t HUGE_PAGES = 0x4;

// MAP_NORESERVE equivalent — kernel is not obliged to back the range with
// physical memory up front. Commits happen lazily via VEH.
inline constexpr uint16_t NORESERVE = 0x8;

// Region is pinned for NUMA interleave — per-slot NUMA mask is stored in
// the mapping table cold union.
inline constexpr uint16_t NUMA_INTERLEAVE = 0x10;

// ANON_PLACEHOLDER is fully committed (placeholder was replaced with
// MEM_COMMIT private VA). Mutually exclusive with NORESERVE. When unset for
// ANON_PLACEHOLDER the slot is either PROT_NONE (state=PLACEHOLDER, pages
// never committed) or NORESERVE (demand-commit via VEH). Drives the
// single-syscall MEM_DECOMMIT partial-unmap recipe and the mprotect fast
// path in is_fully_committed_shape.
inline constexpr uint16_t COMMITTED = 0x20;

} // namespace region_flag

// ============================================================================
// Chunk list — backing for FILE_VIEW_CHUNKED shape.
// ============================================================================
//
// When a FILE_VIEW_MONO region is first partial-munmap'd, it promotes to
// FILE_VIEW_CHUNKED and builds a list of the remaining live sub-views.
// Subsequent partial munmaps update the list in place.
//
// The list is allocated lazily on first promotion and pointed to via the
// descriptor's `chunk_list` atomic pointer. The inline header covers the
// common case (2–4 chunks); an `overflow` pointer extends into a
// page-allocated tail when callers produce more.
//
// Concurrency: all mutations happen under MmapLock writer. Readers copy the
// pointer under the seqlock fence and then access fields with RELAXED
// ordering; the writer lock ordering is sufficient for safe access.

struct ChunkEntry {
  // Bounding range of the live view, in 64 KB units relative to the
  // region's first_slot_key. base_units <= last_units; both inclusive.
  uint32_t base_units;
  uint32_t size_units;
};

struct ChunkList {
  // Inline storage for the common case. Four entries cover the usual
  // { head, middle, tail } split patterns with a little headroom.
  static constexpr unsigned INLINE_CAPACITY = 4;

  // Sentinel returned by chunk_list_find when no chunk covers the key.
  static constexpr uint32_t NPOS = static_cast<uint32_t>(-1);

  ChunkEntry inline_chunks[INLINE_CAPACITY];

  // Overflow buffer (page-allocated when count > INLINE_CAPACITY).
  // Capacity stored in `overflow_capacity`. Null when unused.
  ChunkEntry *overflow;
  uint32_t overflow_capacity;

  // Total live chunks (inline + overflow).
  uint32_t count;

  // Random-access by logical index; bounds-checked via LIBC_ASSERT.
  // Caller must hold MmapLock writer for any mutating access.
  LIBC_INLINE ChunkEntry &at(uint32_t index) {
    return (index < INLINE_CAPACITY) ? inline_chunks[index]
                                      : overflow[index - INLINE_CAPACITY];
  }
  LIBC_INLINE const ChunkEntry &at(uint32_t index) const {
    return (index < INLINE_CAPACITY) ? inline_chunks[index]
                                      : overflow[index - INLINE_CAPACITY];
  }
};

// ============================================================================
// RegionDesc — the descriptor itself.
// ============================================================================
//
// Layout is fixed at one cache line. All sizes / offsets are static_asserted
// below. No fields may be added without either displacing a reserved byte or
// auditing the callers that depend on the layout for snapshot consistency.

struct alignas(64) RegionDesc {
  // Owned section handle. Duplicated once at acquire; closed when refcount
  // drops to 0 under MmapLock writer. NULL for ANON_PLACEHOLDER /
  // ANON_PLACEHOLDER / FOREIGN_SENTINEL shapes.
  HANDLE section_handle; // 0

  // Owned file handle. Duplicated once at acquire; closed when refcount
  // drops to 0. NULL when the section does not have a file backing.
  HANDLE file_handle; // 8

  // Section offset for the first view. Multi-view regions (CHUNKED) are
  // at (section_offset + chunk.base_units * 65536).
  LARGE_INTEGER section_offset; // 16

  // Live-reference count. `0` means the slot is free and may be claimed by
  // acquire. `>=1` means live; readers may dereference fields.
  //
  // acquire: CAS 0 -> 1.
  // add_ref: fetch_add — caller already holds at least one ref.
  // release: fetch_sub; when result is 1 (i.e. old was 1, now 0), the
  //   releaser closes handles, clears descriptor state, and is the last
  //   user of the slot. Caller MUST hold MmapLock writer when a release
  //   may drop the last ref, so no concurrent reader is mid-dereference.
  cpp::Atomic<uint32_t> refcount; // 24

  // Shape. Mutated only by promotion (MONO -> CHUNKED) via atomic CAS under
  // MmapLock writer. Read by everyone after their seqlock snapshot fence.
  cpp::Atomic<uint16_t> shape; // 28

  // Flags bitset — region_flag::* constants. Written at acquire, immutable
  // for the rest of the region's life.
  uint16_t flags; // 30

  // Bounding-range low key (view_base >> 16 of the first slot). Used by the
  // mapping-table walk_range fast path to know where to start scanning for
  // this region's chunks. Mutated only under MmapLock writer during shape
  // transitions and splits.
  uintptr_t first_slot_key; // 32

  // Bounding-range high key (view_end >> 16, exclusive). Mutated only under
  // MmapLock writer.
  uintptr_t last_slot_key; // 40

  // Chunk list pointer — null except for FILE_VIEW_CHUNKED. Page-allocated
  // once on shape promotion; destroyed when refcount hits 0.
  cpp::Atomic<ChunkList *> chunk_list; // 48

  // Generation stamp. Bumped on every successful acquire and used by
  // readers for ABA-safe pool-reuse validation. A value of 0 is reserved
  // to mean "never allocated"; live regions carry 1..255 and wrap by
  // skipping 0 on roll-over. Atomic so resolve() and wait_for_release()
  // see writes published by try_claim's RELEASE fence consistently —
  // before the field was atomic resolve read it plain while
  // wait_for_release read it ACQUIRE, an asymmetry that this fixes.
  cpp::Atomic<uint8_t> alloc_id; // 56

  // Per-region parking mutex byte for chunk_list mutations during
  // shape promotion (MONO -> CHUNKED) and punch/append edits under
  // REMAPPING. Zero = unlocked, 1 = locked. Acquired via CAS; on
  // contention the loser parks via futex_addr::wait and the releaser
  // broadcasts via futex_addr::wake. Multi-waiter (N partial-unmaps of
  // the same CHUNKED region queue here) — not a candidate for
  // ThreadLocalWord, which is single-owner-single-waiter. Contention is
  // rare in practice.
  cpp::Atomic<uint8_t> chunk_list_lock; // 57

  // Padding to cache-line boundary. Explicitly named to keep layout
  // checks simple and to document that extensions land here.
  uint8_t reserved[6]; // 58–63

  // ---- Helpers -----------------------------------------------------------
  //
  // All read-only accessors are const. cpp::Atomic<T>::load is non-const
  // in this libc, so the shape load goes through __atomic_load_n directly
  // — same memory model, just bypasses the wrapper's non-const surface.
  // This lets SlotSnapshot::region carry `const RegionDesc *` and have
  // the read paths compose cleanly.

  LIBC_INLINE RegionShape current_shape() const {
    uint16_t v;
    __atomic_load(&shape.val, &v, __ATOMIC_RELAXED);
    return static_cast<RegionShape>(v);
  }

  LIBC_INLINE bool has_flag(uint16_t bit) const {
    return (flags & bit) != 0;
  }

  // True for shapes whose msync / mincore / page-cache semantics are
  // file-backed (i.e. there is a backing fd whose blocks the kernel
  // page cache reflects). ANON_RESERVE_SECTION is section-backed but
  // pagefile-only — excluded from this predicate even though it does
  // own a section handle.
  LIBC_INLINE bool is_file_backed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::FILE_VIEW_MONO ||
           s == RegionShape::FILE_VIEW_CHUNKED ||
           s == RegionShape::FILE_VIEW_RESERVE;
  }

  // True for shapes whose downstream split-remap / chunk-list / shape
  // promotion machinery applies. Section-backed regions (file or
  // pagefile SEC_RESERVE extension) all need chunk lists when partial
  // unmap fragments their VA.
  LIBC_INLINE bool is_section_backed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::FILE_VIEW_MONO ||
           s == RegionShape::FILE_VIEW_CHUNKED ||
           s == RegionShape::FILE_VIEW_RESERVE ||
           s == RegionShape::ANON_RESERVE_SECTION;
  }

  LIBC_INLINE bool is_anonymous() const {
    const RegionShape s = current_shape();
    return s == RegionShape::ANON_PLACEHOLDER ||
           s == RegionShape::ANON_RESERVE_SECTION;
  }

  LIBC_INLINE bool is_foreign() const {
    return current_shape() == RegionShape::FOREIGN_SENTINEL;
  }

  // True for the four shapes whose underlying VA is not a valid
  // MAP_FIXED target. `prepare_for_fixed` Pass 0 rejects on this
  // predicate; reconcile uses the stricter trio (LIBC_INTERNAL /
  // IMAGE_REGION / KERNEL_REGION) to skip revalidation.
  LIBC_INLINE bool blocks_map_fixed() const {
    const RegionShape s = current_shape();
    return s == RegionShape::LIBC_INTERNAL ||
           s == RegionShape::IMAGE_REGION ||
           s == RegionShape::KERNEL_REGION ||
           s == RegionShape::FOREIGN_SENTINEL;
  }

  // True for the three shapes whose lifecycle we own or whose
  // underlying VA never shifts (LIBC_INTERNAL from our own allocators,
  // IMAGE_REGION from the loader, KERNEL_REGION from NT). Reconcile
  // skips these — no NT re-probe is warranted.
  LIBC_INLINE bool skip_revalidation() const {
    const RegionShape s = current_shape();
    return s == RegionShape::LIBC_INTERNAL ||
           s == RegionShape::IMAGE_REGION ||
           s == RegionShape::KERNEL_REGION;
  }

  LIBC_INLINE bool is_cow() const { return has_flag(region_flag::COW); }

  // Const-friendly atomic loads for inspection paths (tests, diagnostics,
  // const-bound callers from SlotSnapshot::region). These bypass the
  // cpp::Atomic wrapper's non-const load() surface; the memory model is
  // unchanged (acquire-load on the underlying value).

  LIBC_INLINE ChunkList *get_chunk_list() const {
    ChunkList *p;
    __atomic_load(&chunk_list.val, &p, __ATOMIC_ACQUIRE);
    return p;
  }

  LIBC_INLINE uint32_t get_refcount() const {
    uint32_t v;
    __atomic_load(&refcount.val, &v, __ATOMIC_RELAXED);
    return v;
  }

  LIBC_INLINE uint8_t get_alloc_id() const {
    uint8_t v;
    __atomic_load(&alloc_id.val, &v, __ATOMIC_ACQUIRE);
    return v;
  }
};

// ---- Layout invariants ------------------------------------------------------

static_assert(sizeof(RegionDesc) == 64,
              "RegionDesc must be exactly one cache line");
static_assert(alignof(RegionDesc) == 64,
              "RegionDesc must be cache-line aligned");

static_assert(__builtin_offsetof(RegionDesc, section_handle) == 0);
static_assert(__builtin_offsetof(RegionDesc, file_handle) == 8);
static_assert(__builtin_offsetof(RegionDesc, section_offset) == 16);
static_assert(__builtin_offsetof(RegionDesc, refcount) == 24);
static_assert(__builtin_offsetof(RegionDesc, shape) == 28);
static_assert(__builtin_offsetof(RegionDesc, flags) == 30);
static_assert(__builtin_offsetof(RegionDesc, first_slot_key) == 32);
static_assert(__builtin_offsetof(RegionDesc, last_slot_key) == 40);
static_assert(__builtin_offsetof(RegionDesc, chunk_list) == 48);
static_assert(__builtin_offsetof(RegionDesc, alloc_id) == 56);
static_assert(__builtin_offsetof(RegionDesc, chunk_list_lock) == 57);

// RegionDesc must be trivially destructible so the pool can recycle slots
// without running destructors. Lifetime is managed explicitly via
// acquire/release; no C++ object semantics inside the slot.
static_assert(__is_trivially_destructible(RegionDesc),
              "RegionDesc must be trivially destructible for pool recycling");

} // namespace memory
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_DESC_H
