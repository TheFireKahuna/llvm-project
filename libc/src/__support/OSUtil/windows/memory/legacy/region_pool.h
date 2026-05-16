//===--- RegionPool — descriptor allocator for logical mmap regions ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Backs every logical `mmap` call with a RegionDesc, accessed through a
// compact 32-bit `region_id` that the mapping table stores per slot.
//
// Storage: IndexedPool<RegionDesc, ChunkShift=14> — 16 384 slots per chunk,
// guarded, with lock-free hot-path access and bitmap-driven iteration.
// Allocated chunks remain in place; slots are recycled via an `alloc_id`
// generation counter that protects readers against ABA when a slot is
// released and re-acquired by an unrelated mapping.
//
// region_id = 0 is reserved as the null sentinel (never allocated). Valid
// IDs run from 1 to SLOTS_PER_CHUNK * MAX_CHUNKS − 1.
//
// Concurrency model:
//   acquire:   slot CAS refcount 0 → 1, then plain-store fields, bump
//              alloc_id, and set the IndexedPool bitmap bit. Lock-free.
//   add_ref:   fetch_add on refcount. Caller must already hold a ref.
//   release:   fetch_sub on refcount. When it drops the last ref, the
//              caller MUST hold MmapLock writer (no reader may be mid-
//              dereference) and closes the handles, clears the descriptor,
//              and marks the IndexedPool slot dead.
//   resolve:   reader calls after a seqlock snapshot of the mapping table.
//              Validates alloc_id against the snapshot-captured value; a
//              mismatch signals pool reuse and forces the reader to retry
//              its snapshot.
//
// Handle ownership:
//   acquire_owned — takes ownership of the caller's handle; release closes.
//   acquire_dup   — duplicates the caller's handle; release closes the dup.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_POOL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_POOL_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/indexed_pool.h"
#include "src/__support/OSUtil/windows/memory/legacy/mmap_lock.h"
#include "src/__support/OSUtil/windows/memory/legacy/region_desc.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory {

// Forward decls used by the ticket type below.
class RegionPool;

// ============================================================================
// AcquireSpec — initialization payload for a new region.
// ============================================================================
//
// All fields are set at acquire time and, except for shape, are immutable
// for the region's life. Using a struct keeps the acquire API readable and
// lets the compiler spot missing fields at call sites.

struct AcquireSpec {
  // Section handle. For ANON_PLACEHOLDER / FOREIGN_SENTINEL,
  // must be NULL. For file-backed shapes, must be a valid section handle.
  HANDLE section_handle = nullptr;

  // File handle backing the section. May be NULL even for file-backed
  // shapes if the section is pagefile-backed (SEC_COMMIT anon).
  HANDLE file_handle = nullptr;

  // Starting offset within the section.
  LARGE_INTEGER section_offset{};

  // Initial shape. MUST NOT be RegionShape::NONE.
  RegionShape shape = RegionShape::NONE;

  // region_flag::* bits. Immutable for the region's life.
  uint16_t flags = 0;

  // Bounding range for the region's slots, in `view_base >> 16` units.
  // last_slot_key is exclusive. For a single-view region: last = first +
  // (size / 65536).
  uintptr_t first_slot_key = 0;
  uintptr_t last_slot_key = 0;
};

// ============================================================================
// RegionTicket — RAII reservation that auto-releases unless committed.
// ============================================================================
//
// Usage pattern: a caller about to perform a destructive NT mutation
// pre-reserves every region descriptor it will need to publish *afterwards*
// (`RegionPool::reserve(spec)`). Reservation is full acquire — refcount = 1,
// fields populated, handles owned. If any reservation fails (pool OOM), the
// caller bails with the original mapping-table state intact.
//
// Once the destructive op succeeds and the publish step (register_mapping /
// register_placeholder) returns true, the caller calls `commit()` to detach
// the ticket. The dtor then becomes a no-op. If the caller drops the ticket
// without committing — early return on error, scope exit by exception, or
// the publish step returning false in a stress-injected test path — the dtor
// drops the region's reference, which closes its handles via the standard
// release pathway.
//
// The ticket is move-only; no slicing; no implicit conversion. Empty tickets
// (`region_id == NONE`) are valid no-op state for default construction and
// post-commit rest.
//
// All callers of `commit()` must hold whatever lock the surrounding
// RegionPool::release contract requires (MmapLock writer when the drop may
// be the last ref). Reserve / commit themselves take no locks.

class [[nodiscard]] RegionTicket {
public:
  LIBC_INLINE RegionTicket() = default;

  RegionTicket(const RegionTicket &) = delete;
  RegionTicket &operator=(const RegionTicket &) = delete;

  LIBC_INLINE RegionTicket(RegionTicket &&other) noexcept
      : region_id_(other.region_id_), alloc_id_(other.alloc_id_),
        desc_(other.desc_) {
    other.region_id_ = 0;
    other.alloc_id_ = 0;
    other.desc_ = nullptr;
  }
  LIBC_INLINE RegionTicket &operator=(RegionTicket &&other) noexcept {
    if (this != &other) {
      release_if_held();
      region_id_ = other.region_id_;
      alloc_id_ = other.alloc_id_;
      desc_ = other.desc_;
      other.region_id_ = 0;
      other.alloc_id_ = 0;
      other.desc_ = nullptr;
    }
    return *this;
  }
  ~RegionTicket() { release_if_held(); }

  LIBC_INLINE bool valid() const { return region_id_ != 0; }
  LIBC_INLINE explicit operator bool() const { return valid(); }
  LIBC_INLINE uint32_t region_id() const { return region_id_; }
  LIBC_INLINE uint8_t alloc_id() const { return alloc_id_; }
  LIBC_INLINE RegionDesc *desc() const { return desc_; }

  // Detach. Caller adopts the region reference; dtor becomes a no-op.
  // Returns the region_id so call sites can write
  //   `g_mapping_table.register_mapping(..., t.alloc_id(), ...); t.commit();`
  // in the natural order.
  LIBC_INLINE uint32_t commit() {
    uint32_t id = region_id_;
    region_id_ = 0;
    alloc_id_ = 0;
    desc_ = nullptr;
    return id;
  }

private:
  friend class RegionPool;

  LIBC_INLINE RegionTicket(uint32_t rid, uint8_t aid, RegionDesc *desc)
      : region_id_(rid), alloc_id_(aid), desc_(desc) {}

  // Out-of-line so the header doesn't need RegionPool's full definition;
  // defined right after RegionPool below.
  void release_if_held();

  uint32_t region_id_ = 0;
  uint8_t alloc_id_ = 0;
  RegionDesc *desc_ = nullptr;
};

// ============================================================================
// RegionPool — the pool itself.
// ============================================================================

class alignas(64) RegionPool {
public:
  // Matches the slab-pool naming in the rest of the libc. ChunkShift=14 →
  // 16 384 slots per chunk; one chunk is 1 MiB of descriptor storage.
  static constexpr unsigned CHUNK_SHIFT = 14;

  // Reserved sentinels. IDs 1..4 are claimed at `init()` and carry a
  // permanently-high refcount (never released). Every LIBC_INTERNAL /
  // IMAGE_REGION / KERNEL_REGION / FOREIGN_SENTINEL slot in the mapping
  // table points at the matching sentinel region_id. Fast-path
  // registrations (register_mapping_{internal,image,kernel,foreign})
  // bypass RegionPool::acquire entirely; they just stamp the slot with
  // the pre-reserved id.
  //
  // `release()` short-circuits on these ids so decrementing never
  // reaches zero, never closes handles (none are open), and never
  // returns the descriptor to the free list.
  static constexpr uint32_t NONE                       = 0;
  static constexpr uint32_t INTERNAL_REGION_ID         = 1;
  static constexpr uint32_t IMAGE_SENTINEL_REGION_ID   = 2;
  static constexpr uint32_t KERNEL_SENTINEL_REGION_ID  = 3;
  static constexpr uint32_t FOREIGN_SENTINEL_REGION_ID = 4;
  static constexpr uint32_t FIRST_DYNAMIC_REGION_ID    = 5;

  LIBC_INLINE static bool is_sentinel_id(uint32_t region_id) {
    return region_id >= INTERNAL_REGION_ID &&
           region_id <= FOREIGN_SENTINEL_REGION_ID;
  }

  // Resolve a sentinel region_id → const descriptor. Used by the
  // mapping-table snapshot short-circuit so the four skip-resolve
  // shapes don't pay RegionPool::resolve_unlocked's ABA dance.
  [[nodiscard]] const RegionDesc *sentinel_desc(uint32_t region_id);

  // Idempotent initialization. Thread-safe; concurrent callers see a
  // consistent post-init state. Must be called once before any other
  // method.
  void init();

  // Post-fork reinit. Called from child under MmapLock::fork_reinit hook.
  // Resets the pool's internal locks that dead threads may have held, and
  // decommits empty chunks to return physical memory inherited from parent.
  // Region descriptors themselves survive — the child inherits parent's
  // VA and the CoW'd section views that point at them.
  LIBC_INLINE void fork_reinit() { pool_.fork_reinit(); }

  // --------------------------------------------------------------------------
  // Acquire — allocate a new region and take ownership of the handles.
  // --------------------------------------------------------------------------
  //
  // The caller hands over the section and file handles to the pool. They
  // will be closed by release() when the last reference drops.
  //
  // Returns a valid region_id (1..max) on success, or RegionPool::NONE on
  // allocation failure. On failure the caller retains ownership of the
  // handles and must close them.
  //
  // Postconditions on success:
  //   - The returned region_id carries refcount = 1.
  //   - alloc_id is the freshly bumped generation (1..255, skipping 0).
  //   - The caller may read the descriptor via get_mutable() under its
  //     own synchronization; readers elsewhere must use resolve() with a
  //     seqlock-captured alloc_id.
  [[nodiscard]] uint32_t acquire_owned(const AcquireSpec &spec);

  // --------------------------------------------------------------------------
  // Acquire — duplicate the caller's handles into a new region.
  // --------------------------------------------------------------------------
  //
  // Convenience for call sites that share a handle with a cache (e.g., the
  // OFD section cache). The pool duplicates via NtDuplicateObject; release
  // closes the dup. The caller retains its originals.
  //
  // Returns RegionPool::NONE on allocation or duplication failure.
  [[nodiscard]] uint32_t acquire_dup(const AcquireSpec &spec);

  // --------------------------------------------------------------------------
  // Reserve — pre-acquire for the prepare-then-commit transaction pattern.
  // --------------------------------------------------------------------------
  //
  // Returns a RegionTicket that owns the acquired region's reference. If the
  // pool cannot allocate (OOM), the returned ticket is empty (`!t.valid()`).
  // On the empty path the caller's section/file handles are NOT closed —
  // identical to acquire_owned's failure contract.
  //
  // The intended pattern:
  //
  //   RegionTicket t = g_region_pool.reserve(spec);
  //   if (!t) return -ENOMEM;       // bail before any destructive op
  //   // ... destructive NT mutation ...
  //   if (!g_mapping_table.register_mapping(base, size,
  //                                         t.region_id(), t.alloc_id(),
  //                                         prot, flags)) {
  //     // publish failed: ~t releases the region; handles closed.
  //     return -EFAULT;
  //   }
  //   t.commit();   // adopt — dtor becomes a no-op.
  //
  // The reserve+commit boundary is what closes the "untracked-VA on publish
  // OOM" race: the only failure mode that survives reserve is publish-side
  // refusal of the slot, which under our locking discipline implies a real
  // CAS conflict (and in that case rollback is the right behavior).
  [[nodiscard]] LIBC_INLINE RegionTicket reserve(const AcquireSpec &spec) {
    uint32_t rid = acquire_owned(spec);
    if (rid == NONE)
      return RegionTicket{};
    RegionDesc *desc = get_mutable(rid);
    // RELAXED is sufficient: this thread just acquired the slot exclusively,
    // so the alloc_id we read is our own most-recent write — no cross-
    // thread synchronization needed.
    return RegionTicket{rid, desc->alloc_id.load(cpp::MemoryOrder::RELAXED),
                        desc};
  }

  // --------------------------------------------------------------------------
  // Reference counting.
  // --------------------------------------------------------------------------

  // Increment refcount. Caller must already hold at least one reference.
  // Used for ring-buffer double-map, fork inheritance accounting, and
  // split-remap fragment reparenting.
  //
  // Sentinel ids (INTERNAL / IMAGE / KERNEL / FOREIGN) short-circuit —
  // their refcount is pinned at init and paired release()s are no-ops,
  // so an extra fetch_add would drift the pinned value without any
  // matching subtract.
  LIBC_INLINE void add_ref(uint32_t region_id) {
    LIBC_ASSERT(region_id != NONE && "add_ref on NONE");
    if (is_sentinel_id(region_id))
      return;
    RegionDesc *rd = raw_slot(region_id);
    LIBC_ASSERT(rd != nullptr && "add_ref on unallocated slot");
    uint32_t prev = rd->refcount.fetch_add(1, cpp::MemoryOrder::RELAXED);
    LIBC_ASSERT(prev >= 1 && "add_ref on dead region");
    (void)prev;
  }

  // Decrement refcount. When the call drops the last reference, the pool
  // closes the handles, frees the chunk list (if any), and marks the slot
  // dead. Caller MUST hold MmapLock writer in that case to exclude
  // concurrent readers.
  //
  // It is safe to call release() without the writer lock so long as the
  // caller knows the drop will not be the last (e.g., a ring-buffer slot
  // detach where the other slot still holds a ref).
  //
  // Every release wakes any thread parked on the descriptor's refcount
  // through wait_for_release(). The wake is unconditional (not just on
  // last drop) so monitoring code can observe each step toward zero.
  void release(uint32_t region_id);

  // --------------------------------------------------------------------------
  // Park-on-refcount — block until refcount drops to zero or the slot is
  // recycled out from under us.
  // --------------------------------------------------------------------------
  //
  // Used by teardown coordinators that must wait for in-flight ops to
  // complete before the descriptor's section handle can be safely closed
  // (exec self-hollow, diagnostic shutdown, fork reconciliation cleanups).
  //
  // The caller passes the alloc_id captured when they obtained their
  // observation of the region; if the pool recycles the slot mid-wait,
  // the alloc_id check trips and the call returns. Either outcome
  // (refcount==0 or alloc_id mismatch) means the caller's reference
  // either no longer matters or never existed.
  //
  // Multi-waiter safe: futex_addr is a lock-free Treiber-stack parking
  // lot keyed on the refcount address. Wakers (release()) hit a single
  // ACQUIRE load of the bucket's live_count fast-path on the no-waiter
  // path.
  void wait_for_release(uint32_t region_id, uint8_t expected_alloc_id);

  // --------------------------------------------------------------------------
  // Resolve — reader access with alloc_id validation.
  // --------------------------------------------------------------------------
  //
  // Called by every code path that obtains a region_id from a mapping-table
  // seqlock snapshot. Validates that the pool slot still carries the
  // alloc_id captured by the snapshot; returns nullptr if a concurrent
  // release + re-acquire has reused the slot (reader must retry its
  // snapshot).
  //
  // Returns a pointer the caller may read (handles, shape). The pointer
  // remains valid for the rest of the caller's MmapLock reader window.
  // Returned non-const because cpp::Atomic<primitive>::load is non-const;
  // callers that do not intend to mutate bind the result to a `const *`.
  // Lifetime contract:
  //   The pointer returned by resolve() is safe to dereference while the
  //   calling thread holds MmapLock (shared or exclusive). A last-ref
  //   release() that closes the descriptor's section/file handles requires
  //   MmapLock writer; any reader holding the shared side is therefore
  //   mutually exclusive against teardown for the duration of its critical
  //   section. The runtime defenses (alloc_id ACQUIRE check + refcount
  //   ACQUIRE > 0 check) close the obvious race windows; the lock keeps
  //   the descriptor alive long enough for the caller to complete its
  //   reads of section_handle / file_handle.
  //
  // VEH demand-commit is the explicit exception. It runs from a fault
  // handler that cannot acquire MmapLock without risking deadlock against
  // a writer holding the lock on the same thread. VEH calls
  // resolve_unlocked() and is restricted to atomic-field reads (shape,
  // flags) — it never touches section_handle / file_handle. The
  // alloc_id + refcount guards still apply and the data the handler
  // consults are independently consistent.
  [[nodiscard]] LIBC_INLINE RegionDesc *resolve(uint32_t region_id,
                                                uint8_t expected_alloc_id) {
    LIBC_ASSERT(windows::MmapLock::is_held_by_current_thread() &&
                "RegionPool::resolve requires MmapLock held by caller; "
                "use resolve_unlocked() from VEH-style contexts");
    return resolve_unlocked(region_id, expected_alloc_id);
  }

  // Lock-free resolve. Use only from contexts that cannot acquire MmapLock
  // (VEH fault handlers) and that read atomic fields only. The returned
  // pointer's lifetime is guaranteed only against the alloc_id the caller
  // captured — concurrent release+re-acquire trips the alloc_id check on
  // the next reload, but a release that races AFTER this call returned can
  // free the descriptor's plain (non-atomic) fields. Callers must restrict
  // themselves to rd->shape / rd->flags / rd->refcount and never touch
  // rd->section_handle, rd->file_handle, rd->section_offset.
  [[nodiscard]] LIBC_INLINE RegionDesc *
  resolve_unlocked(uint32_t region_id, uint8_t expected_alloc_id) {
    if (region_id == NONE)
      return nullptr;
    RegionDesc *rd = raw_slot(region_id);
    if (rd == nullptr)
      return nullptr;
    // ACQUIRE: paired with try_claim's RELEASE fence. If we observe the
    // expected alloc_id, every other descriptor field written by that
    // try_claim is visible to us.
    if (rd->alloc_id.load(cpp::MemoryOrder::ACQUIRE) != expected_alloc_id)
      return nullptr;
    if (rd->refcount.load(cpp::MemoryOrder::ACQUIRE) == 0)
      return nullptr;
    return rd;
  }

  // --------------------------------------------------------------------------
  // Mutable access for trusted callers (under MmapLock writer).
  // --------------------------------------------------------------------------
  //
  // No alloc_id check. Use only when the caller already has a valid live
  // reference to the region and is about to mutate fields that require the
  // writer lock (shape promotion, bounds adjustment, chunk-list updates).
  [[nodiscard]] LIBC_INLINE RegionDesc *get_mutable(uint32_t region_id) {
    if (region_id == NONE)
      return nullptr;
    RegionDesc *rd = raw_slot(region_id);
    LIBC_ASSERT(rd != nullptr && "get_mutable on unallocated slot");
    LIBC_ASSERT(rd->refcount.load(cpp::MemoryOrder::RELAXED) >= 1 &&
                "get_mutable on dead region");
    return rd;
  }

  // --------------------------------------------------------------------------
  // Diagnostic / testing helpers.
  // --------------------------------------------------------------------------

  // Number of currently-live regions across the whole pool. O(chunks) —
  // walks each allocated chunk's live_count. For tests and telemetry only.
  [[nodiscard]] uint32_t live_count_estimate();

private:
  // Storage. RegionDesc itself is trivially destructible (static_assert in
  // region_desc.h) so the pool can recycle slots without running dtors.
  internal::IndexedPool<RegionDesc, CHUNK_SHIFT> pool_;

  // Next chunk hint — bumped by the scanner when all chunks we examined
  // were full. RELAXED is sufficient: a wrong hint only costs an extra
  // scan pass.
  cpp::Atomic<uint32_t> next_scan_chunk_{0};

  // Generation counter bumped on every release(). Acquire callers that
  // exhaust the directory (pathological — the pool capacity is ~134 M
  // descriptors) snapshot this value, park on it via futex_addr::wait,
  // and retry once a peer release wakes them. Wraparound is benign: any
  // observable change marks "the world might have new free slots".
  cpp::Atomic<uint32_t> release_gen_{0};

  // Raw slot access helpers. Return nullptr if the region_id refers to a
  // chunk that has not been allocated yet.
  LIBC_INLINE RegionDesc *raw_slot(uint32_t region_id) {
    return pool_.slot_for(region_id);
  }

  // Try to claim a specific slot by CAS on refcount 0 → 1. On success,
  // initializes the descriptor from `spec` (with `section`/`file` already
  // resolved by the caller to its owned/duped versions) and returns true.
  [[nodiscard]] bool try_claim(uint32_t region_id, HANDLE section, HANDLE file,
                               const AcquireSpec &spec);

  // Scan a chunk for a free slot (refcount == 0) and attempt to claim it.
  // On success, returns the region_id; on failure (chunk fully live or
  // CAS losses exhaust the chunk) returns RegionPool::NONE.
  [[nodiscard]] uint32_t scan_chunk(unsigned chunk_index, HANDLE section,
                                    HANDLE file, const AcquireSpec &spec);

  // Core acquire after handle resolution. Walks chunks starting at the
  // cached hint, expanding to new chunks as needed, and returns a claimed
  // region_id or RegionPool::NONE on failure.
  [[nodiscard]] uint32_t acquire_resolved(HANDLE section, HANDLE file,
                                          const AcquireSpec &spec);

  // Duplicate a HANDLE via NtDuplicateObject with SAME_ACCESS. Returns
  // nullptr on failure or when `src` is nullptr (nullable dup is a no-op).
  [[nodiscard]] static HANDLE dup_handle(HANDLE src);

  // Close a HANDLE unconditionally. NullHandle-safe.
  static void close_handle(HANDLE h);

  // Free the chunk list (and its overflow buffer if present). Called at
  // release time when a CHUNKED region drops its last ref.
  static void free_chunk_list(ChunkList *list);
};

// Forwarder declared here to avoid an include cycle on the global instance
// declaration below; defined in region_pool.cpp.
void g_region_pool_release_ticket(uint32_t region_id);

// Out-of-line ticket release (the forwarder above is now visible).
LIBC_INLINE void RegionTicket::release_if_held() {
  if (region_id_ != 0) {
    g_region_pool_release_ticket(region_id_);
    region_id_ = 0;
    alloc_id_ = 0;
    desc_ = nullptr;
  }
}

// ============================================================================
// Global instance.
// ============================================================================
//
// One pool per process, initialized eagerly at libc init. External linkage
// so the fork hook and tests can reach it.

extern RegionPool g_region_pool;

// ============================================================================
// ChunkList helpers — used during MONO -> CHUNKED shape promotion and
// subsequent partial-munmap edits.
// ============================================================================
//
// All mutating helpers require the caller to hold MmapLock writer. Readers
// access ChunkEntry data via RegionDesc::chunk_list (an atomic pointer)
// and the seqlock fence already in place around region resolution.
//
// Storage strategy: a ChunkList is page-allocated (the smallest NT VA
// reservation is 64 KB, but allocations are rare — only on first promotion
// per region). Overflow buffers are page-allocated separately when
// count > INLINE_CAPACITY (4); each overflow page holds 8192 ChunkEntry,
// which is more than any realistic mapping needs.

// Allocate a fresh, empty ChunkList. Returns nullptr on OOM.
[[nodiscard]] ChunkList *chunk_list_alloc();

// Install a freshly-allocated list onto a region's chunk_list pointer.
// CAS-based: if a concurrent caller already installed one, the loser's
// list is freed and the winner's is returned. Caller must hold MmapLock
// writer; CAS is for paranoia, not contention.
//
// Returns the list now bound to the region (caller's or winner's),
// or nullptr if both alloc paths failed.
[[nodiscard]] ChunkList *install_chunk_list(RegionDesc *rd);

// Append a chunk at the end. Grows overflow if count would exceed inline
// + overflow_capacity. Caller must hold MmapLock writer.
//
// Returns false on OOM during overflow growth (chunk not appended).
[[nodiscard]] bool chunk_list_append(ChunkList *list, ChunkEntry entry);

// Remove the chunk at `index`, compacting the tail with a swap. Caller
// must hold MmapLock writer. `index < list->count` asserted.
void chunk_list_erase(ChunkList *list, uint32_t index);

// Find the index of the chunk that covers `base_units` (relative to the
// owning region's first_slot_key). Returns ChunkList::NPOS if none.
[[nodiscard]] uint32_t chunk_list_find(const ChunkList *list,
                                       uint32_t base_units);

// Punch a hole [hole_base, hole_base + hole_size) out of the chunk at
// `index`. The hole MUST be fully contained inside that chunk. The chunk
// is replaced in place by 0, 1, or 2 survivors (head / tail). Caller
// must hold MmapLock writer.
//
// Returns false on OOM when the 2-way split needs overflow growth and
// the grow fails — in that case the list is unchanged and the caller
// must abort the unmap (or fall back to a full-region teardown).
[[nodiscard]] bool chunk_list_punch(ChunkList *list, uint32_t index,
                                    uint32_t hole_base, uint32_t hole_size);

} // namespace memory
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_REGION_POOL_H
