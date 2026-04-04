//===-- Linear-hashing lock-free hash table ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lock-free linear-hashing task_id → ThreadLifecycle* hash table.
//
// Why linear hashing: open-addressing-with-doubling-resize incurs an
// O(N) burst per resize event. Linear hashing splits ONE bucket per
// trigger, amortizing growth into O(1) per insert with no big-bang
// resize. Combined with external Harris bucket entries, growth is
// fully demand-commit and reclamation is fully Crystalline-managed —
// no upfront VA, no lingering memory.
//
// Structure
// ---------
//   * Top-level pointer array (32KB, fixed-size, demand-paged):
//     4096 entries × `BucketHeadPage*`. Pages allocated on demand
//     by the splitter as bucket index `S'` is created.
//   * BucketHeadPage: 512 `Atomic<BucketEntry*>` heads packed into
//     ~4KB. Crystalline-retired when the table is torn down (fork or
//     fini).
//   * BucketEntry: `{task_id, lc, next}`. Harris-marked `next` (mark
//     bit at position 0). Crystalline-retired on delete or split-
//     migration.
//
// State word
// ----------
//   `(L: 32, S: 31, split_pending: 1)` packed into one 64-bit atomic.
//   * `L`            — log2 of "round-base" bucket count.
//   * `S`            — next bucket index to split, in [0, 2^L).
//   * `split_pending`— a splitter has claimed bucket S; readers must
//                      probe both bucket S and bucket S+2^L while set.
//
//   Total live bucket count = 2^L + S (during a round, monotonic).
//
// Hash function (uniform on monotonic task_ids — no avalanche needed):
//   b = task_id & ((1 << L) - 1)
//   if b < S:                       # already-split: use one extra bit
//     b = task_id & ((1 << (L+1)) - 1)
//   if split_pending and b == S:    # currently splitting: probe both
//     check b and b + (1 << L)
//
// Insert protocol — when split_pending && L_bits == S, use L+1 bits
// directly, so the inserter lands in the FINAL bucket (S or S').
// This bounds the splitter's migration work to PRE-EXISTING entries.
//
// Split protocol — single splitter at a time, claimed via the
// split_pending bit:
//   1. CAS (L, S, false) → (L, S, true).
//   2. Ensure bucket S' = S + 2^L's BucketHeadPage exists.
//   3. Walk bucket S; for each entry whose task_id has bit L set:
//        a. Allocate fresh BucketEntry e' with same {task_id, lc}.
//        b. Crystalline init_node(e').
//        c. CAS-prepend e' onto bucket S' head.
//        d. Harris mark + unlink old from bucket S.
//        e. Crystalline retire(old).
//   4. CAS (L, S, true) → (L, S+1, false), or (L+1, 0, false) if
//      S+1 == 2^L (round complete).
//
// Why this is correct under concurrent inserts/deletes/lookups:
//   * Lookups while split_pending probe BOTH S and S' — they cannot
//     miss a partially-migrated entry.
//   * Inserts while split_pending land directly in the final bucket
//     via L+1 bits, so the splitter's migration is bounded.
//   * Each migration step "alloc-new-then-unlink-old" — at any moment
//     every reachable task_id appears in at least one of {S, S'}.
//     A reader that finds the same task_id in both gets the same lc
//     (by construction); the duplicate window is benign.
//   * Crystalline reservations transitively pin bucket entries AND
//     the lifecycles they point at, so plain pointer derefs through
//     the chain are safe under one reservation index.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_LOCKFREE_HASH_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_LOCKFREE_HASH_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_registry_node.h"

namespace LIBC_NAMESPACE_DECL {

struct ThreadLifecycle;

// ---------------------------------------------------------------------------
// Compile-time parameters
// ---------------------------------------------------------------------------

// Buckets per BucketHeadPage. 512 heads × 8B = 4096B fits a 4KB OS
// page, with the 32-byte ThreadRegistryNode header consuming 32B and
// the heads array spanning the remainder when allocation is rounded
// up. BucketHeadPage is allocated via page_alloc which returns
// page-multiple sizes; one page is enough for the header + 504 heads
// or two pages for the header + 511 heads. We use two pages (8KB) so
// `kBucketsPerPage` is a clean power of 2 (no modulo on the hot path).
inline constexpr uint32_t kBucketsPerPage = 512;

// Top-level pointer array capacity. 4096 × `BucketHeadPage*` = 32KB
// upfront. Demand-paged by the OS (only touched pages back real
// memory). Combined with `kBucketsPerPage` this caps the table at
// 4096 × 512 = 2,097,152 buckets — at average chain length 4 that
// supports 8M live threads, far beyond any realistic process.
inline constexpr uint32_t kTopLevelCapacity = 4096;

// Average chain length the splitter targets. The split trigger fires
// when `live_count > kTargetLoadFactor × bucket_count`. 4 is a
// standard balance: short chains keep lookup near-O(1), but tolerating
// chains up to 4 amortizes split work and limits BucketHeadPage churn.
inline constexpr uint32_t kTargetLoadFactor = 4;

// ---------------------------------------------------------------------------
// Harris mark-bit helpers (bit 0 of a `BucketEntry*` or `ThreadLifecycle*`)
// ---------------------------------------------------------------------------
//
// Crystalline's `slow_path` strips bits 0-1 internally before
// dereferencing the result for its `batch_link` check, AND returns
// the masked pointer to the caller. Crystalline's `fast_path` returns
// the raw atomic load (mark bit may be present). We unify by always
// stripping bit 0 explicitly on the caller side after `read()`, and
// checking the mark via a separate plain load.
//
// Bit 1 is reserved for Crystalline's internal use (its slow_path
// strips both bits 0 and 1). We use only bit 0 for Harris.

inline constexpr uintptr_t kHarrisMarkBit = 1;

template <typename T> LIBC_INLINE bool harris_is_marked(T *p) {
  return (reinterpret_cast<uintptr_t>(p) & kHarrisMarkBit) != 0;
}

template <typename T> LIBC_INLINE T *harris_unmark(T *p) {
  return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(p) &
                                ~kHarrisMarkBit);
}

template <typename T> LIBC_INLINE T *harris_with_mark(T *p) {
  return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(p) |
                                kHarrisMarkBit);
}

// ---------------------------------------------------------------------------
// BucketEntry — Harris node living in a hash bucket's chain
// ---------------------------------------------------------------------------
//
// One per registered thread (during the thread's registered lifetime).
// Allocated and Crystalline-retired independently of `ThreadLifecycle` —
// the entry's lifetime is bound to the bucket chain, the lifecycle's
// lifetime is bound to the thread's registration. They reach the
// retire side at independent moments via independent retire() calls.
//
// `next` carries the Harris mark in bit 0. Plain loads check the
// mark; the unmarked pointer is what `read()` consumers get back
// (after explicit `harris_unmark`).
//
// `lc` is a plain pointer. Crystalline transitively pins it: any
// reservation that pinned the entry's bucket page also pins this
// entry, and any reservation that pinned this entry pins the
// lifecycle it references (by the deregister ordering invariant —
// the entry is retired BEFORE the lifecycle, so observing the entry
// implies observing the lifecycle as still-live at our era).
//
// `task_id` is plain; it's set once at construction and never mutated.
struct BucketEntry : public ThreadRegistryNode {
  uint32_t task_id;
  uint32_t _pad;
  ThreadLifecycle *lc;
  cpp::Atomic<BucketEntry *> next;
};

static_assert(sizeof(BucketEntry) <= 64,
              "BucketEntry should fit in one cache line — header(32) + "
              "task_id(4) + pad(4) + lc(8) + next(8) = 56 bytes");

// ---------------------------------------------------------------------------
// BucketHeadPage — array of bucket heads, demand-allocated
// ---------------------------------------------------------------------------
//
// Sized so the header + heads array fit cleanly. Allocated through
// `internal::page_alloc` (rounded up to page granularity). Crystalline-
// retired on table teardown (fork-reinit, process fini).
//
// During a split, the splitter allocates a new BucketHeadPage for the
// destination range only when bucket S' first lands on a fresh page —
// otherwise the existing page is reused.
struct BucketHeadPage : public ThreadRegistryNode {
  // Page index in the top-level array. Stamped at allocation; helpful
  // for diagnostics; never mutated.
  uint32_t page_index;
  uint32_t _pad;

  cpp::Atomic<BucketEntry *> heads[kBucketsPerPage];
};

// ---------------------------------------------------------------------------
// HashState — packed (L, S, split_pending) atomic word
// ---------------------------------------------------------------------------
//
// Encoded:
//   bits  0..30 — S (next bucket to split; up to 2^31 − 1)
//   bit   31    — split_pending
//   bits 32..63 — L (log2 of round-base bucket count)
//
// CAS-driven: split claim flips bit 31, split publish advances S
// (and possibly L) while clearing bit 31. Any read of the state by
// inserters/lookups uses `decode_hash_state` below to extract the
// fields without aliasing concerns.
inline constexpr uint64_t kHashStateSplitPendingBit = 1ULL << 31;
inline constexpr uint64_t kHashStateSMask = (1ULL << 31) - 1;
inline constexpr int kHashStateLShift = 32;

struct HashState {
  uint32_t L;
  uint32_t S;
  bool split_pending;
};

LIBC_INLINE constexpr HashState decode_hash_state(uint64_t raw) {
  return HashState{
      /*L=*/static_cast<uint32_t>(raw >> kHashStateLShift),
      /*S=*/static_cast<uint32_t>(raw & kHashStateSMask),
      /*split_pending=*/(raw & kHashStateSplitPendingBit) != 0};
}

LIBC_INLINE constexpr uint64_t encode_hash_state(uint32_t L, uint32_t S,
                                                   bool split_pending) {
  return (static_cast<uint64_t>(L) << kHashStateLShift) |
         (static_cast<uint64_t>(S) & kHashStateSMask) |
         (split_pending ? kHashStateSplitPendingBit : 0);
}

// Total bucket count under a given (L, S). Equals 2^L + S during the
// L-th round; once S reaches 2^L the splitter publishes (L+1, 0).
LIBC_INLINE constexpr uint32_t bucket_count_at(uint32_t L, uint32_t S) {
  return (1u << L) + S;
}

// Resolve a bucket index `b` to (page_index, in_page_index) using
// `kBucketsPerPage` (a power of 2, so this is a shift+mask).
struct BucketLocation {
  uint32_t page_index;
  uint32_t in_page_index;
};

LIBC_INLINE constexpr BucketLocation locate_bucket(uint32_t b) {
  return BucketLocation{/*page_index=*/b / kBucketsPerPage,
                        /*in_page_index=*/b % kBucketsPerPage};
}

// ---------------------------------------------------------------------------
// Hash function (matching the lookup/insert protocol)
// ---------------------------------------------------------------------------
//
// Returns the bucket index a key maps to under (L, S). When a key
// hashes to bucket S during split_pending, the caller MUST probe
// BOTH the returned bucket and `+ (1 << L)` — encoded by the
// `is_splitting_target` flag below.
struct HashedBucket {
  uint32_t bucket;
  // When true, caller must also probe `bucket + (1 << L)`. Set only
  // when split_pending and the L-bit hash equals S.
  bool is_splitting_target;
};

LIBC_INLINE HashedBucket hash_for_lookup(uint32_t task_id, HashState st) {
  uint32_t L = st.L;
  uint32_t L_bits = task_id & ((1u << L) - 1u);
  if (st.split_pending && L_bits == st.S) {
    // Mid-split on our bucket: caller probes both halves.
    return HashedBucket{/*bucket=*/L_bits, /*is_splitting_target=*/true};
  }
  if (L_bits < st.S) {
    // Already-split: use L+1 bits to disambiguate.
    return HashedBucket{
        /*bucket=*/task_id & ((1u << (L + 1)) - 1u),
        /*is_splitting_target=*/false};
  }
  return HashedBucket{/*bucket=*/L_bits, /*is_splitting_target=*/false};
}

// Insert variant: when split_pending && L_bits == S, the inserter
// uses L+1 bits directly, landing in the FINAL bucket (S or S').
// This bounds the splitter's migration work to pre-existing entries.
LIBC_INLINE uint32_t hash_for_insert(uint32_t task_id, HashState st) {
  uint32_t L = st.L;
  uint32_t L_bits = task_id & ((1u << L) - 1u);
  if (st.split_pending && L_bits == st.S) {
    return task_id & ((1u << (L + 1)) - 1u);
  }
  if (L_bits < st.S) {
    return task_id & ((1u << (L + 1)) - 1u);
  }
  return L_bits;
}

// ---------------------------------------------------------------------------
// Harris CAS primitives on `Atomic<BucketEntry*>` chains
// ---------------------------------------------------------------------------
//
// These operate on the next-pointer field (or the bucket-head field,
// which has the same shape — `Atomic<BucketEntry*>` semantically).
// Each primitive is small enough to inline at every call site; their
// purpose is to make the protocol's mark/unmark/CAS pattern explicit
// and consistent.
//
// Crystalline-protected `read()`s of `Atomic<BucketEntry*>` happen at
// the registry level (where the domain object is in scope); the
// helpers here operate on plain atomic ops that participate in the
// Harris protocol but do NOT themselves invoke Crystalline.

// Try to set the Harris mark bit on `next_field`, expecting the
// current value to be exactly `expected_unmarked`. Fails (returns
// false) if `next_field` no longer matches `expected_unmarked` — for
// example, if the mark is already set or if the next pointer changed.
LIBC_INLINE bool
harris_try_mark(cpp::Atomic<BucketEntry *> &next_field,
                BucketEntry *expected_unmarked) {
  BucketEntry *desired = harris_with_mark(expected_unmarked);
  return next_field.compare_exchange_strong(expected_unmarked, desired,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE);
}

// Try to unlink `target` from its predecessor's next-pointer, where
// `pred_next` is `&pred->next` (or the bucket head pointer's storage)
// and `target_next_unmarked` is what comes after target. The CAS
// validates pred is unmarked AND pred->next is exactly `target` — so
// a concurrent op marking pred or splicing past pred would fail.
LIBC_INLINE bool
harris_try_unlink(cpp::Atomic<BucketEntry *> &pred_next, BucketEntry *target,
                  BucketEntry *target_next_unmarked) {
  BucketEntry *expected = target;
  return pred_next.compare_exchange_strong(expected, target_next_unmarked,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::ACQUIRE);
}

// CAS-prepend `entry` at the head of a bucket chain. Loops on weak-
// CAS spurious failures and concurrent prepends. `entry->next` is set
// to the observed head before each CAS. Lock-free.
LIBC_INLINE void
harris_prepend(cpp::Atomic<BucketEntry *> &head, BucketEntry *entry) {
  for (;;) {
    BucketEntry *cur_head = head.load(cpp::MemoryOrder::ACQUIRE);
    // Sanity: cur_head should never be marked at the bucket-head
    // level — Harris marks live on each entry's own next-pointer,
    // and the bucket head is "before" any entry. Defensively unmask
    // in case a future protocol tweak leaves a stale bit.
    cur_head = harris_unmark(cur_head);
    entry->next.store(cur_head, cpp::MemoryOrder::RELAXED);
    if (head.compare_exchange_weak(cur_head, entry,
                                    cpp::MemoryOrder::RELEASE,
                                    cpp::MemoryOrder::RELAXED))
      return;
  }
}

// Same shape, on the global iter list head. Distinct only because
// `ThreadLifecycle` is the node type instead of `BucketEntry`. The
// Harris discipline is identical.
LIBC_INLINE bool harris_is_marked_iter(ThreadLifecycle *p) {
  return harris_is_marked(p);
}
LIBC_INLINE ThreadLifecycle *harris_unmark_iter(ThreadLifecycle *p) {
  return harris_unmark(p);
}
LIBC_INLINE ThreadLifecycle *harris_with_mark_iter(ThreadLifecycle *p) {
  return harris_with_mark(p);
}

LIBC_INLINE bool
harris_try_mark_iter(cpp::Atomic<ThreadLifecycle *> &next_field,
                     ThreadLifecycle *expected_unmarked) {
  ThreadLifecycle *desired = harris_with_mark_iter(expected_unmarked);
  return next_field.compare_exchange_strong(expected_unmarked, desired,
                                             cpp::MemoryOrder::ACQ_REL,
                                             cpp::MemoryOrder::ACQUIRE);
}

LIBC_INLINE bool
harris_try_unlink_iter(cpp::Atomic<ThreadLifecycle *> &pred_next,
                       ThreadLifecycle *target,
                       ThreadLifecycle *target_next_unmarked) {
  ThreadLifecycle *expected = target;
  return pred_next.compare_exchange_strong(expected, target_next_unmarked,
                                            cpp::MemoryOrder::ACQ_REL,
                                            cpp::MemoryOrder::ACQUIRE);
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_LOCKFREE_HASH_H
