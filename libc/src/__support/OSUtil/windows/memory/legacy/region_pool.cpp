//===--- RegionPool — implementation --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/legacy/region_pool.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace memory {

// ============================================================================
// Global pool instance.
// ============================================================================
//
// Constant-initialized (every atomic has a constexpr value ctor; IndexedPool
// is designed for constant init). init() must be called once before any
// operation — libc startup invokes it at the memory-subsystem init phase.
RegionPool g_region_pool;

// ============================================================================
// Sentinel reservation — IDs 1..4 at init().
// ============================================================================
//
// The mapping table fast-paths (register_mapping_{internal,image,kernel,
// foreign}) stamp slots with these fixed region_ids. Each sentinel gets one
// `RegionDesc` in the pool, but the descriptor carries no section / file
// handle and its refcount is pinned so `release()` never reaches zero.
//
// Layout invariant: region_id 0 is NONE (never allocated). IDs 1..4 are the
// four sentinels in the order enumerated in region_pool.h. `FIRST_DYNAMIC_
// REGION_ID = 5` is the first id an ordinary `acquire_owned` / `acquire_dup`
// will ever return.
//
// Sentinels are claimed before the pool is visible to any allocator, so
// single-threaded construction is safe: the CAS dance exists to rule out
// staleness from a prior `reset_for_test` call, not cross-thread races.
//
// Concurrency: every reservation is RELEASE-published; the bitmap bit is
// set after the descriptor write. Subsequent resolve paths ACQUIRE-load
// alloc_id and see the finished descriptor.

namespace {

struct SentinelSpec {
  uint32_t region_id;
  RegionShape shape;
};

constexpr SentinelSpec SENTINEL_SPECS[] = {
    {RegionPool::INTERNAL_REGION_ID, RegionShape::LIBC_INTERNAL},
    {RegionPool::IMAGE_SENTINEL_REGION_ID, RegionShape::IMAGE_REGION},
    {RegionPool::KERNEL_SENTINEL_REGION_ID, RegionShape::KERNEL_REGION},
    {RegionPool::FOREIGN_SENTINEL_REGION_ID, RegionShape::FOREIGN_SENTINEL},
};

// Value the sentinel refcount is pinned to. Any plausible number of
// live slots stays far below the saturation point; `release()` short-
// circuits on sentinel ids anyway, so this is purely a belt-and-braces
// guard against diagnostic paths that read `get_refcount()`.
constexpr uint32_t SENTINEL_REFCOUNT_PIN = 0x7FFFFFFFu;

} // namespace

void RegionPool::init() {
  pool_.init();

  // Idempotent: if the sentinels are already claimed (repeat init() call,
  // or init() after fork_reinit), leave them alone. A sentinel is "claimed"
  // iff its refcount is nonzero — we set it to SENTINEL_REFCOUNT_PIN below
  // so a stale zero on a first-time-init is the only miss case.
  using Pool = internal::IndexedPool<RegionDesc, CHUNK_SHIFT>;
  for (const SentinelSpec &spec : SENTINEL_SPECS) {
    RegionDesc *rd = raw_slot(spec.region_id);
    if (rd == nullptr) {
      // IndexedPool hadn't materialized chunk 0 — force it.
      RegionDesc *scan = pool_.acquire_for_scan(0);
      (void)scan;
      rd = raw_slot(spec.region_id);
      LIBC_ASSERT(rd != nullptr &&
                  "RegionPool::init: chunk 0 must materialize for sentinels");
      // Drop the scan pin — we'll hold each sentinel slot via its own
      // bitmap bit + pinned refcount, not through the chunk-level
      // scan reference.
      pool_.release_scan_ref(0);
    }

    uint32_t cur = rd->refcount.load(cpp::MemoryOrder::RELAXED);
    if (cur != 0)
      continue; // Already claimed in a prior init().

    // CAS 0 → pinned. On success, we own the slot exclusively long
    // enough to write the descriptor. No handles to install — sentinels
    // are handle-less.
    if (!rd->refcount.compare_exchange_strong(cur, SENTINEL_REFCOUNT_PIN,
                                              cpp::MemoryOrder::ACQUIRE,
                                              cpp::MemoryOrder::RELAXED))
      continue;

    // Bump alloc_id so a stale snapshot from before fork / re-init
    // doesn't accidentally validate against this sentinel.
    uint8_t prev_aid = rd->alloc_id.load(cpp::MemoryOrder::RELAXED);
    uint8_t next_aid = static_cast<uint8_t>(prev_aid + 1);
    if (next_aid == 0)
      next_aid = 1;
    rd->alloc_id.store(next_aid, cpp::MemoryOrder::RELAXED);

    rd->section_handle = nullptr;
    rd->file_handle = nullptr;
    rd->section_offset.QuadPart = 0;
    rd->flags = 0;
    rd->first_slot_key = 0;
    rd->last_slot_key = 0;
    rd->chunk_list.store(nullptr, cpp::MemoryOrder::RELAXED);
    rd->chunk_list_lock.store(0, cpp::MemoryOrder::RELAXED);
    rd->shape.store(static_cast<uint16_t>(spec.shape),
                    cpp::MemoryOrder::RELEASE);

    // Set the bitmap bit so diagnostic iteration (live_count_estimate,
    // test sweeps) sees the slot as occupied.
    RegionDesc *chunk0 = pool_.chunk_slots(0);
    if (chunk0 != nullptr) {
      auto *meta = Pool::meta_for(chunk0);
      (void)meta->bitmap.try_acquire(spec.region_id);
    }
  }
}

const RegionDesc *RegionPool::sentinel_desc(uint32_t region_id) {
  LIBC_ASSERT(is_sentinel_id(region_id) &&
              "sentinel_desc: id is not a reserved sentinel");
  return raw_slot(region_id);
}

// ============================================================================
// Handle helpers.
// ============================================================================

HANDLE RegionPool::dup_handle(HANDLE src) {
  if (src == nullptr)
    return nullptr;
  HANDLE out = nullptr;
  NTSTATUS st = ::NtDuplicateObject(NtCurrentProcess(), src, NtCurrentProcess(),
                                    &out, /*DesiredAccess=*/0, /*Attributes=*/0,
                                    DUPLICATE_SAME_ACCESS);
  return NT_SUCCESS(st) ? out : nullptr;
}

void RegionPool::close_handle(HANDLE h) {
  if (h != nullptr)
    ::NtClose(h);
}

// =============================================================================
// ChunkList header allocator — slab-pooled, dense, lock-free.
// =============================================================================
//
// ChunkList is ~48 bytes. Allocating it via page_alloc would reserve 64 KB
// of VA per CHUNKED region (first-page commit ≈ 4 KB RAM). A SlabPool with
// a 64-byte slot size packs ~1000 ChunkLists into a single 64 KB slab —
// 64 B per region instead of 4 KB.
//
// Lifetime:
//   * `init` and `init_tls` are idempotent / latched; call lazily on first
//     allocation. Hot-path cost is one ACQUIRE load on the latch.
//   * `tls_alloc` returns zero-on-free memory (SlabPool contract), so the
//     freshly-acquired ChunkList is already zero-initialized — count = 0,
//     overflow = nullptr, overflow_capacity = 0.
//   * `SlabPool::free` is static, lock-free, and routes cross-thread frees.
//
// Overflow buffers stay on page_alloc (one page per buffer, holds 8192
// ChunkEntry — more than any realistic mapping). Different lifetime, much
// larger size; not worth pooling.
internal::SlabPool g_chunk_list_pool;

namespace {

LIBC_INLINE void ensure_chunk_list_pool() {
  // Both calls are idempotent and lock-free past the latch's READY state.
  // The slot size includes a small alignment headroom (alignof(void*)).
  g_chunk_list_pool.init(sizeof(ChunkList), alignof(ChunkList));
  g_chunk_list_pool.init_tls();
}

} // namespace

void RegionPool::free_chunk_list(ChunkList *list) {
  if (list == nullptr)
    return;
  if (list->overflow != nullptr) {
    // Overflow entries live in a single page_alloc reservation sized at
    // allocation time; release by VA. No leak even if overflow_capacity
    // was bumped (we grow in-place with idempotent commits).
    internal::page_free(list->overflow);
    list->overflow = nullptr;
    list->overflow_capacity = 0;
  }
  internal::SlabPool::free(list);
}

// ============================================================================
// Descriptor initialization helpers.
// ============================================================================
//
// The claim sequence is:
//   1. Scanner finds a candidate slot with bitmap bit == 0 and refcount == 0.
//   2. Scanner calls bitmap.try_acquire(local_idx) — atomic CAS on the bit.
//   3. Scanner calls try_claim() — which writes fields and CASes refcount
//      from 0 to 1. If the slot was genuinely free, the CAS succeeds and
//      the region is live.
//   4. On CAS failure (race loser), scanner clears the bitmap bit and moves
//      on to the next candidate.
//
// alloc_id is bumped inside try_claim so each new generation starts with a
// distinct ABA guard. `0` is reserved; wrap-around skips it.

namespace {

LIBC_INLINE uint8_t next_alloc_id(uint8_t prev) {
  // 0 is reserved as "never allocated" — skip it on wrap.
  uint8_t next = static_cast<uint8_t>(prev + 1);
  return next == 0 ? static_cast<uint8_t>(1) : next;
}

LIBC_INLINE void write_descriptor(RegionDesc *rd, HANDLE section, HANDLE file,
                                  const AcquireSpec &spec) {
  rd->section_handle = section;
  rd->file_handle = file;
  rd->section_offset = spec.section_offset;
  rd->flags = spec.flags;
  rd->first_slot_key = spec.first_slot_key;
  rd->last_slot_key = spec.last_slot_key;
  // chunk_list starts null. Shape promotion (MONO -> CHUNKED) allocates one
  // on first partial munmap.
  rd->chunk_list.store(nullptr, cpp::MemoryOrder::RELAXED);
  // Per-region chunk-list spinlock starts unlocked. Reset on every claim
  // so a stale value from a prior occupant cannot wedge a future locker.
  rd->chunk_list_lock.store(0, cpp::MemoryOrder::RELAXED);
  rd->shape.store(static_cast<uint16_t>(spec.shape), cpp::MemoryOrder::RELAXED);
}

// Bump the generation stamp under exclusive slot ownership. RELAXED is
// sufficient against the writer side because try_claim's RELEASE fence
// publishes this store together with every other field; readers consume
// via ACQUIRE on the same field. The single-writer-per-slot invariant
// (held via the refcount CAS) means no other thread observes intermediate
// values.
LIBC_INLINE void bump_alloc_id(RegionDesc *rd) {
  uint8_t prev = rd->alloc_id.load(cpp::MemoryOrder::RELAXED);
  rd->alloc_id.store(next_alloc_id(prev), cpp::MemoryOrder::RELAXED);
}

} // namespace

// ============================================================================
// try_claim — CAS refcount 0 → 1 and initialize.
// ============================================================================

bool RegionPool::try_claim(uint32_t region_id, HANDLE section, HANDLE file,
                           const AcquireSpec &spec) {
  LIBC_ASSERT(region_id != NONE && "try_claim on NONE");
  LIBC_ASSERT(spec.shape != RegionShape::NONE && "try_claim with NONE shape");

  RegionDesc *rd = raw_slot(region_id);
  if (rd == nullptr)
    return false;

  // The slot's bitmap bit is already held by the caller (try_acquire). A
  // concurrent releaser cannot drop-to-zero on this slot while we hold the
  // bit because the releaser clears the bit as part of its cleanup sequence
  // (pool_.mark_dead under MmapLock writer). So refcount is stably 0 here,
  // but we still CAS to publish the transition atomically.
  uint32_t expected = 0;
  if (!rd->refcount.compare_exchange_strong(expected, 1,
                                            cpp::MemoryOrder::ACQUIRE,
                                            cpp::MemoryOrder::RELAXED))
    return false;

  // CAS won — we own the slot exclusively until refcount returns to 0. No
  // other thread can observe field writes in progress.
  bump_alloc_id(rd);
  write_descriptor(rd, section, file, spec);

  // RELEASE: publish every field write above to any reader that later
  // observes this slot through refcount >= 1.
  cpp::atomic_thread_fence(cpp::MemoryOrder::RELEASE);
  return true;
}

// ============================================================================
// scan_chunk — find a free slot in one chunk and try to claim it.
// ============================================================================

uint32_t RegionPool::scan_chunk(unsigned chunk_index, HANDLE section,
                                HANDLE file, const AcquireSpec &spec) {
  using Pool = internal::IndexedPool<RegionDesc, CHUNK_SHIFT>;

  RegionDesc *chunk = pool_.acquire_for_scan(chunk_index);
  if (chunk == nullptr)
    return NONE;

  auto *meta = Pool::meta_for(chunk);

  for (unsigned w = 0; w < Pool::BITMAP_WORDS; ++w) {
    uint64_t occupied = meta->bitmap.template word_at<cpp::MemoryOrder::ACQUIRE>(w);
    uint64_t free_bits = ~occupied;

    // Reserve region_id = 0 (chunk 0, slot 0) as the null sentinel.
    if (chunk_index == 0 && w == 0)
      free_bits &= ~uint64_t{1};

    while (free_bits != 0) {
      unsigned b = static_cast<unsigned>(__builtin_ctzll(free_bits));
      unsigned local_idx = w * 64u + b;

      if (!meta->bitmap.try_acquire(local_idx)) {
        free_bits &= free_bits - 1;
        continue;
      }

      // We hold the bitmap bit. Attempt the refcount CAS.
      const uint32_t region_id =
          (chunk_index << Pool::CHUNK_SHIFT) | local_idx;

      if (!try_claim(region_id, section, file, spec)) {
        // Race: another path (or stale bitmap bit from a mid-release
        // sibling) held the slot. Release the bit and keep scanning.
        meta->bitmap.clear(local_idx);
        free_bits &= free_bits - 1;
        continue;
      }

      // Successfully claimed. Pin from acquire_for_scan stays — it
      // represents this slot's contribution to the chunk's live_count.
      return region_id;
    }
  }

  // Chunk fully occupied (for this scan pass). Undo the pin; the pool may
  // recycle physical pages if all slots drain.
  pool_.release_scan_ref(chunk_index);
  return NONE;
}

// ============================================================================
// acquire_resolved — walk chunks starting at the hint, with a bounded
// park-and-retry on directory exhaustion.
// ============================================================================
//
// The scan walks the entire directory exactly once (offset rotation around
// `next_scan_chunk_`) and grows the directory lazily inside scan_chunk via
// IndexedPool::ensure_chunk. Allocated chunks are 64 KB of VA each; we only
// pay that cost when no existing chunk has a free slot.
//
// Pool capacity is the directory ceiling (`Pool::INLINE_CAP +
// Pool::DIR_MAX_ENTRIES` chunks × 2^CHUNK_SHIFT slots). With CHUNK_SHIFT=14
// and 8206 directory entries that is ~134 M descriptors — true exhaustion
// is not a realistic operating point. The retry loop exists for the
// transient case where every slot is briefly held during fork-reinit
// teardown or an adversarial test that drains the pool: snapshot the
// release-generation counter, futex-park on it, and retry on wake. Bounded
// at ACQUIRE_RETRY_BUDGET so a genuine OOM still surfaces as NONE.

uint32_t RegionPool::acquire_resolved(HANDLE section, HANDLE file,
                                      const AcquireSpec &spec) {
  using Pool = internal::IndexedPool<RegionDesc, CHUNK_SHIFT>;
  constexpr unsigned DIRECTORY_CAP = Pool::DIRECTORY_CAPACITY;
  constexpr unsigned ACQUIRE_RETRY_BUDGET = 4;

  for (unsigned attempt = 0; attempt < ACQUIRE_RETRY_BUDGET; ++attempt) {
    // Snapshot the release-gen BEFORE the scan so we don't miss a wake
    // that fires while we're walking the directory.
    uint32_t seen_gen = release_gen_.load(cpp::MemoryOrder::ACQUIRE);

    uint32_t start = next_scan_chunk_.load(cpp::MemoryOrder::RELAXED);
    if (start >= DIRECTORY_CAP)
      start = 0;

    // Wrapping linear walk: visit every chunk exactly once, starting at
    // the hint. scan_chunk grows the directory on the first miss past
    // the existing high-water mark. First-success early-return prevents
    // gratuitous chunk allocation when free slots already exist.
    for (unsigned offset = 0; offset < DIRECTORY_CAP; ++offset) {
      const unsigned ci = (start + offset) % DIRECTORY_CAP;
      const uint32_t id = scan_chunk(ci, section, file, spec);
      if (id != NONE) {
        if (ci != start)
          next_scan_chunk_.store(ci, cpp::MemoryOrder::RELAXED);
        return id;
      }
    }

    // Genuine miss. Park on the release generation; a peer release will
    // wake us. The wait returns immediately if the gen already advanced
    // during our walk — no risk of indefinite stall on benign races.
    if (attempt + 1 == ACQUIRE_RETRY_BUDGET)
      break;
    futex_addr::wait(&release_gen_, seen_gen, nullptr);
  }

  return NONE;
}

// ============================================================================
// Public acquire entry points.
// ============================================================================

uint32_t RegionPool::acquire_owned(const AcquireSpec &spec) {
  LIBC_ASSERT(spec.shape != RegionShape::NONE && "acquire with NONE shape");
  return acquire_resolved(spec.section_handle, spec.file_handle, spec);
}

uint32_t RegionPool::acquire_dup(const AcquireSpec &spec) {
  LIBC_ASSERT(spec.shape != RegionShape::NONE && "acquire with NONE shape");

  HANDLE section_dup = dup_handle(spec.section_handle);
  if (spec.section_handle != nullptr && section_dup == nullptr)
    return NONE; // Section handle present but dup failed.

  HANDLE file_dup = dup_handle(spec.file_handle);
  if (spec.file_handle != nullptr && file_dup == nullptr) {
    close_handle(section_dup);
    return NONE;
  }

  uint32_t id = acquire_resolved(section_dup, file_dup, spec);
  if (id == NONE) {
    close_handle(section_dup);
    close_handle(file_dup);
    return NONE;
  }
  return id;
}

// ============================================================================
// Forwarder for RegionTicket dtor.
// ============================================================================
//
// Defined here so the inline ticket dtor in the header doesn't need the
// global-pool definition in scope. Hot path is the ticket-empty case which
// doesn't reach this function.

void g_region_pool_release_ticket(uint32_t region_id) {
  g_region_pool.release(region_id);
}

// ============================================================================
// release — ref decrement with cleanup on last drop.
// ============================================================================

void RegionPool::release(uint32_t region_id) {
  LIBC_ASSERT(region_id != NONE && "release on NONE");

  // Sentinel ids (INTERNAL / IMAGE / KERNEL / FOREIGN) carry a pinned
  // refcount and are never freed. register_mapping_internal / image /
  // kernel / foreign stamp their slots with these ids without calling
  // acquire; so every drop of a slot that happened to reference a
  // sentinel id must skip the decrement and the handle-close path. The
  // refcount remains at kSentinelRefcountPin.
  if (is_sentinel_id(region_id))
    return;

  RegionDesc *rd = raw_slot(region_id);
  LIBC_ASSERT(rd != nullptr && "release on unallocated slot");

  // ACQ_REL: acquires prior writes from threads that decremented before us
  // (so we see the full descriptor state on last drop) and publishes our
  // decrement.
  uint32_t prev = rd->refcount.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  LIBC_ASSERT(prev >= 1 && "release underflow");

  if (prev != 1) {
    // Intermediate decrement — wake any park-on-refcount waiters so they
    // can re-poll. The futex_addr fast path bails on an empty bucket in a
    // single ACQUIRE load (~6 ns), so unconditional wake is acceptable.
    futex_addr::wake(&rd->refcount, UINT32_MAX);
    return;
  }

  // Last reference. Caller must hold MmapLock writer so no reader is mid-
  // dereference of this descriptor. Close handles, free chunk list, clear
  // observable fields, and return the slot to the pool.
  //
  // Debug-only assertion: enforces the writer-held contract documented in
  // region_pool.h:288-300. RegionTicket dtor and any other release() caller
  // that may drop the last ref MUST run inside an `MmapLockWriterGuard`
  // scope; without the writer lock a concurrent shared-side reader could
  // be mid-dereference of section_handle / file_handle when we close them.
  // In release builds this compiles to nothing; in debug builds it traps
  // on the first contract violation.
  LIBC_ASSERT(g_mmap_lock.is_writer_held_by_current_thread() &&
              "RegionPool::release last-ref drop requires MmapLock writer; "
              "wrap with MmapLockWriterGuard at the call site");
  HANDLE section = rd->section_handle;
  HANDLE file = rd->file_handle;
  ChunkList *list = rd->chunk_list.load(cpp::MemoryOrder::RELAXED);

  // Clear observable fields. alloc_id is intentionally NOT cleared — the
  // next acquire bumps it via next_alloc_id, which guarantees a fresh
  // generation that differs from any stale snapshot held by a reader that
  // missed the writer lock boundary.
  rd->section_handle = nullptr;
  rd->file_handle = nullptr;
  rd->section_offset.QuadPart = 0;
  rd->flags = 0;
  rd->first_slot_key = 0;
  rd->last_slot_key = 0;
  rd->chunk_list.store(nullptr, cpp::MemoryOrder::RELAXED);
  rd->shape.store(static_cast<uint16_t>(RegionShape::NONE),
                  cpp::MemoryOrder::RELEASE);

  close_handle(section);
  close_handle(file);
  free_chunk_list(list);

  // Return the slot to the pool. mark_dead clears the bitmap bit and
  // decrements the chunk's live_count; if this was the chunk's last slot,
  // physical memory is reclaimed via MEM_RESET.
  pool_.mark_dead(region_id);

  // Wake park-on-refcount waiters AFTER teardown so they observe the
  // cleared state when they reload. The address used is the same
  // refcount field address; bucket hashing is by raw address.
  futex_addr::wake(&rd->refcount, UINT32_MAX);

  // Wake any acquire path parked on directory exhaustion. RELEASE so the
  // waker's view of the cleared slot is visible to any acquirer that
  // observes the new generation.
  release_gen_.fetch_add(1, cpp::MemoryOrder::RELEASE);
  futex_addr::wake(&release_gen_, UINT32_MAX);
}

// ============================================================================
// wait_for_release — park on refcount until it drops to zero, or until the
// slot is recycled out from under us (alloc_id mismatch).
// ============================================================================

void RegionPool::wait_for_release(uint32_t region_id,
                                  uint8_t expected_alloc_id) {
  if (region_id == NONE)
    return;
  // Sentinels never drop — their refcount is pinned. A waiter here
  // would block forever; there is nothing for it to observe.
  if (is_sentinel_id(region_id))
    return;
  RegionDesc *rd = raw_slot(region_id);
  if (rd == nullptr)
    return;

  for (;;) {
    // alloc_id check first — if the slot was freed and re-acquired by an
    // unrelated mapping, our reference is irrelevant and the wait would
    // never terminate against the new region's lifecycle.
    if (rd->alloc_id.load(cpp::MemoryOrder::ACQUIRE) != expected_alloc_id)
      return;

    uint32_t v = rd->refcount.load(cpp::MemoryOrder::ACQUIRE);
    if (v == 0)
      return;

    // Park on the refcount. wait() returns immediately when the value
    // already differs from `v`; the outer loop catches both the zero
    // condition and any alloc_id swap.
    futex_addr::wait(&rd->refcount, v,
                     /*timeout=*/static_cast<const struct timespec *>(nullptr));
  }
}

// ============================================================================
// ChunkList helpers.
// ============================================================================
//
// Storage strategy:
//   * The ChunkList header is one page_alloc allocation. The 64 KB minimum
//     is wasteful per region but CHUNKED regions are produced only by the
//     first partial-munmap of a file mapping, which is rare in practice.
//   * Overflow buffers are page-allocated separately. One overflow page
//     holds 8192 ChunkEntry — more than any realistic mapping needs.
//
// Concurrency:
//   * All mutations require MmapLock writer.
//   * Readers see ChunkEntry data via the seqlock fence around region
//     resolution; the chunk_list pointer itself is atomic (RELAXED writes
//     under writer lock; ACQUIRE reads).
//   * install_chunk_list uses CAS purely to avoid double-publish in the
//     unlikely case two writer-lock holders race a promotion (e.g.,
//     reentrant unmap inside a fault handler).

namespace {

LIBC_INLINE uint32_t overflow_entries_per_page() {
  return static_cast<uint32_t>(windows::get_page_size() / sizeof(ChunkEntry));
}

// Compact the tail of a list when we want to swap-remove `index`. Caller
// has already decremented count; this just moves the previous-last entry
// into the freed slot. No-op when `index` was the last live entry.
void swap_remove_tail(ChunkList *list, uint32_t index) {
  const uint32_t last = list->count; // already decremented
  if (index == last)
    return;
  list->at(index) = list->at(last);
}

// Grow the overflow buffer to hold at least `min_capacity` total entries
// beyond INLINE_CAPACITY. Idempotent if the existing buffer is already
// large enough. Returns false on OOM (existing buffer is left intact).
[[nodiscard]] bool grow_overflow(ChunkList *list, uint32_t min_capacity) {
  if (list->overflow_capacity >= min_capacity)
    return true;

  const uint32_t per_page = overflow_entries_per_page();
  // Round up to the next page worth of entries.
  uint32_t target_capacity =
      ((min_capacity + per_page - 1u) / per_page) * per_page;
  if (target_capacity < per_page)
    target_capacity = per_page;

  const SIZE_T bytes =
      static_cast<SIZE_T>(target_capacity) * sizeof(ChunkEntry);
  ChunkEntry *fresh =
      static_cast<ChunkEntry *>(internal::page_alloc(bytes));
  if (fresh == nullptr)
    return false;

  // Migrate existing overflow contents (if any) and release the old page.
  if (list->overflow != nullptr && list->count > ChunkList::INLINE_CAPACITY) {
    const uint32_t live_overflow =
        list->count - ChunkList::INLINE_CAPACITY;
    for (uint32_t i = 0; i < live_overflow; ++i)
      fresh[i] = list->overflow[i];
  }
  if (list->overflow != nullptr)
    internal::page_free(list->overflow);

  list->overflow = fresh;
  list->overflow_capacity = target_capacity;
  return true;
}

} // namespace

ChunkList *chunk_list_alloc() {
  ensure_chunk_list_pool();
  // SlabPool::tls_alloc guarantees zero-on-free + bump-init, so every
  // field starts zero — count = 0, overflow = nullptr,
  // overflow_capacity = 0, inline_chunks all zero. No re-init needed.
  return static_cast<ChunkList *>(g_chunk_list_pool.tls_alloc());
}

ChunkList *install_chunk_list(RegionDesc *rd) {
  LIBC_ASSERT(rd != nullptr && "install_chunk_list on null region");
  // Fast path: list already installed.
  ChunkList *existing = rd->chunk_list.load(cpp::MemoryOrder::ACQUIRE);
  if (existing != nullptr)
    return existing;

  ChunkList *fresh = chunk_list_alloc();
  if (fresh == nullptr)
    return nullptr;

  ChunkList *expected = nullptr;
  if (!rd->chunk_list.compare_exchange_strong(expected, fresh,
                                              cpp::MemoryOrder::RELEASE,
                                              cpp::MemoryOrder::ACQUIRE)) {
    // Lost the race — somebody else installed first. Free our allocation
    // and return the winner.
    internal::SlabPool::free(fresh);
    return expected;
  }
  return fresh;
}

bool chunk_list_append(ChunkList *list, ChunkEntry entry) {
  LIBC_ASSERT(list != nullptr && "chunk_list_append on null list");

  if (list->count < ChunkList::INLINE_CAPACITY) {
    list->inline_chunks[list->count] = entry;
    ++list->count;
    return true;
  }

  const uint32_t needed = list->count - ChunkList::INLINE_CAPACITY + 1u;
  if (needed > list->overflow_capacity && !grow_overflow(list, needed))
    return false;

  list->overflow[list->count - ChunkList::INLINE_CAPACITY] = entry;
  ++list->count;
  return true;
}

void chunk_list_erase(ChunkList *list, uint32_t index) {
  LIBC_ASSERT(list != nullptr && "chunk_list_erase on null list");
  LIBC_ASSERT(index < list->count && "chunk_list_erase: index out of range");
  --list->count;
  swap_remove_tail(list, index);
}

uint32_t chunk_list_find(const ChunkList *list, uint32_t base_units) {
  LIBC_ASSERT(list != nullptr && "chunk_list_find on null list");
  for (uint32_t i = 0; i < list->count; ++i) {
    const ChunkEntry &c = list->at(i);
    if (base_units >= c.base_units &&
        base_units < c.base_units + c.size_units)
      return i;
  }
  return ChunkList::NPOS;
}

bool chunk_list_punch(ChunkList *list, uint32_t index, uint32_t hole_base,
                      uint32_t hole_size) {
  LIBC_ASSERT(list != nullptr && "chunk_list_punch on null list");
  LIBC_ASSERT(index < list->count && "chunk_list_punch: index out of range");
  LIBC_ASSERT(hole_size > 0 && "chunk_list_punch: zero-size hole");

  ChunkEntry chunk = list->at(index);
  LIBC_ASSERT(hole_base >= chunk.base_units &&
              hole_base + hole_size <= chunk.base_units + chunk.size_units &&
              "chunk_list_punch: hole not contained in chunk");

  const uint32_t head_base = chunk.base_units;
  const uint32_t head_size = hole_base - chunk.base_units;
  const uint32_t tail_base = hole_base + hole_size;
  const uint32_t tail_size =
      chunk.base_units + chunk.size_units - tail_base;

  // Case 1: hole == whole chunk. Erase.
  if (head_size == 0 && tail_size == 0) {
    chunk_list_erase(list, index);
    return true;
  }

  // Case 2: hole at the head (no head survivor, only tail).
  if (head_size == 0) {
    list->at(index).base_units = tail_base;
    list->at(index).size_units = tail_size;
    return true;
  }

  // Case 3: hole at the tail (no tail survivor, only head).
  if (tail_size == 0) {
    list->at(index).base_units = head_base;
    list->at(index).size_units = head_size;
    return true;
  }

  // Case 4: hole in the middle — produces two survivors. Pre-grow overflow
  // so we never half-split on OOM.
  if (list->count >= ChunkList::INLINE_CAPACITY) {
    const uint32_t needed = list->count - ChunkList::INLINE_CAPACITY + 1u;
    if (needed > list->overflow_capacity && !grow_overflow(list, needed))
      return false;
  } else if (list->count + 1u > ChunkList::INLINE_CAPACITY) {
    // Will spill the new entry into overflow — ensure capacity for one.
    if (!grow_overflow(list, 1u))
      return false;
  }

  // Replace `index` in place with the head survivor; append the tail.
  list->at(index).base_units = head_base;
  list->at(index).size_units = head_size;

  ChunkEntry tail{tail_base, tail_size};
  if (list->count < ChunkList::INLINE_CAPACITY) {
    list->inline_chunks[list->count] = tail;
  } else {
    list->overflow[list->count - ChunkList::INLINE_CAPACITY] = tail;
  }
  ++list->count;
  return true;
}

// ============================================================================
// live_count_estimate.
// ============================================================================

uint32_t RegionPool::live_count_estimate() {
  // Walk directory, sum each chunk's live_count. Approximate because slots
  // in transition contribute instantaneously to either side. Diagnostic use
  // only (tests, telemetry); not load-bearing for correctness.
  uint32_t total = 0;
  using Pool = internal::IndexedPool<RegionDesc, CHUNK_SHIFT>;

  for (unsigned ci = 0;; ++ci) {
    RegionDesc *slots = pool_.chunk_slots(ci);
    if (slots == nullptr)
      break;
    auto *meta = Pool::meta_for(slots);
    total += meta->live_count.load(cpp::MemoryOrder::RELAXED);
  }
  return total;
}

} // namespace memory
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
