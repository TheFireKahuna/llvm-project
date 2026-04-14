//===-- Flat-slab thread registry for Windows --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread Registry: bitmap-indexed slab pages + TID hash table.
//
// Design goals:
//   - Lock-free reads with epoch-based reclamation (no global reader counter).
//   - O(1) TID lookup via open-addressing hash table.
//   - O(live) iteration via 64-bit bitmap scan per page.
//   - One cache line (64 bytes) per registered thread — zero false sharing.
//   - Slot reuse — memory proportional to peak live threads, not cumulative.
//   - Batch TID extraction for NtAlertMultipleThreadByThreadId.
//   - Fork-friendly: clear bitmaps + rebuild hash with one entry.
//
// Storage layout:
//   SlotPage (4096 B) = 64-byte header (bitmap + metadata) + 63 Slots.
//   Each Slot (64 B) = lifecycle pointer + epoch pin + cached TID + generation.
//   Pages are indexed by a demand-committed pointer array.
//   A separate open-addressing hash table maps TID → slot coordinates.
//
// Epoch protocol:
//   Readers store the current global_epoch into their slot's pinned_epoch.
//   Writers (synchronize, hash resize) bump global_epoch and wait for all
//   pinned_epoch values to be 0 or past the target. Reader cost: one
//   thread-local store (no shared atomic RMW). Writer cost: epoch advance
//   + per-slot scan (rare path).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/type_traits/remove_reference.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry_state.h"

#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

// =========================================================================
// Core data structures
// =========================================================================

/// One registered thread = one cache line.
struct alignas(64) RegistrySlot {
  /// Lifecycle pointer. Null means the slot is free.
  cpp::Atomic<ThreadLifecycle *> lifecycle;

  /// Epoch pin for the reader protocol. The owning thread stores the current
  /// global_epoch here before reading registry data; stores 0 when done.
  /// Lives in the thread's own cache line — zero contention on pin/unpin.
  cpp::Atomic<uint64_t> pinned_epoch;

  /// Cached NT thread ID for batch extraction (collect_tids). Avoids chasing
  /// the lifecycle pointer when building a TID array. Atomic because the hash
  /// resize reads it concurrently with deregister clearing it.
  ///
  /// Deregister clears lifecycle and tid BEFORE clearing the bitmap bit.
  /// Hash rebuild checks lifecycle != null before reading tid, so a slot
  /// mid-deregister is skipped — no phantom hash entries.
  cpp::Atomic<DWORD> tid;

  /// Monotonically incrementing generation counter. Bumped each time the slot
  /// is reused. Combined with (page, index), provides ABA protection.
  uint32_t generation;

  /// Nesting depth for epoch pin. Only the owning thread reads/writes this,
  /// so no atomic is needed. Incremented by registry_pin, decremented by
  /// registry_unpin. pinned_epoch is only cleared when depth reaches 0.
  uint32_t pin_depth;

  uint8_t reserved[36];
};

static_assert(sizeof(RegistrySlot) == 64,
              "RegistrySlot must be exactly one cache line");

/// Number of slots per page. 63 slots fit in 4096 - 64 (header) = 4032 bytes.
inline constexpr uint32_t kSlotsPerPage = 63;

/// Maximum number of pages in the page index.
/// 1024 pages × 63 slots = 64,512 threads.
inline constexpr uint32_t kMaxRegistryPages = 1024;

/// Bitmap sentinel: all bits set = "page is being retired". Prevents
/// allocate_slot from finding free bits (~UINT64_MAX == 0). Iterators
/// see all-bits-set but find null lifecycles and skip them harmlessly.
inline constexpr uint64_t kBitmapRetiring = UINT64_MAX;

/// One page of thread slots. Exactly one NT page (4096 bytes).
struct alignas(4096) RegistryPage {
  /// Bitmap of live slots. Bit i is set iff slots[i] holds a live thread.
  /// Set to kBitmapRetiring during page retirement.
  cpp::Atomic<uint64_t> bitmap;

  /// Index of this page within the page array.
  uint32_t page_index;

  uint32_t reserved_pad0;

  /// Linked list pointer for epoch-safe retirement. Only used after the page
  /// has been removed from the page index array; overlaps header padding.
  RegistryPage *retired_next;

  /// Epoch at which this page was retired. Items with retired_epoch less than
  /// the minimum pinned epoch across all live slots are safe to free.
  uint64_t retired_epoch;

  uint32_t reserved_pad[8];

  /// Thread slots. Each slot is one cache line.
  RegistrySlot slots[kSlotsPerPage];
};

static_assert(sizeof(RegistryPage) == 4096,
              "RegistryPage must be exactly one NT page");

// pack_tid_entry uses 6 bits for slot index and 10 bits for page index.
// These must cover the full range of valid values.
static_assert(kSlotsPerPage <= 63,
              "Slot index must fit in 6 bits (max 63)");
static_assert(kMaxRegistryPages <= 1024,
              "Page index must fit in 10 bits (max 1024)");

// =========================================================================
// Slot reference — lightweight handle to a registered slot
// =========================================================================

struct SlotRef {
  uint32_t page;
  uint32_t index;
  uint32_t generation;

  LIBC_INLINE static constexpr SlotRef invalid() {
    return {UINT32_MAX, UINT32_MAX, 0};
  }
  LIBC_INLINE bool is_valid() const { return page != UINT32_MAX; }
};

// =========================================================================
// TID hash table — open-addressing, linear probing, power-of-2
// =========================================================================

/// Packed hash table entry. Zero = empty, 1 = tombstone.
/// Otherwise: high 32 bits = TID, low 32 bits = slot coordinates.
///
/// Slot coordinates: bits [15:6] = page index, bits [5:0] = slot index.
/// This supports up to 1024 pages (10 bits) and 63 slots (6 bits).
struct TidHashTable {
  cpp::Atomic<uint64_t> *entries;
  uint32_t capacity;
  uint32_t mask; // capacity - 1

  // Linked list for epoch-safe retirement of old tables.
  TidHashTable *retired_next;

  // Epoch at which this table was retired.
  uint64_t retired_epoch;
};

inline constexpr uint64_t kHashEmpty = 0;
inline constexpr uint64_t kHashTombstone = 1;
inline constexpr uint32_t kInitialHashCapacity = 128;

/// Pack a TID + slot coordinates into a hash entry value.
LIBC_INLINE uint64_t pack_tid_entry(DWORD tid, uint32_t page, uint32_t index) {
  uint32_t slot_packed = (page << 6) | (index & 0x3F);
  return (static_cast<uint64_t>(tid) << 32) | slot_packed;
}

/// Extract TID from a packed hash entry.
LIBC_INLINE DWORD unpack_tid(uint64_t packed) {
  return static_cast<DWORD>(packed >> 32);
}

/// Extract page index from a packed hash entry.
LIBC_INLINE uint32_t unpack_page(uint64_t packed) {
  return (static_cast<uint32_t>(packed) >> 6) & 0x3FF;
}

/// Extract slot index from a packed hash entry.
LIBC_INLINE uint32_t unpack_index(uint64_t packed) {
  return static_cast<uint32_t>(packed) & 0x3F;
}

/// Simple hash mixing for TID values.
LIBC_INLINE uint32_t hash_tid(DWORD tid) {
  // Windows TIDs are multiples of 4 with moderate entropy.
  // Multiplicative hash distributes them across power-of-2 tables.
  uint32_t x = static_cast<uint32_t>(tid);
  x = ((x >> 16) ^ x) * 0x45D9F3B;
  x = ((x >> 16) ^ x) * 0x45D9F3B;
  x = (x >> 16) ^ x;
  return x;
}

// =========================================================================
// Public API — free functions (matches codebase convention)
// =========================================================================

// --- Registration ---

/// Register a thread in the registry. Returns a SlotRef identifying the slot.
/// If `duplicate_handle` is true, the handle is duplicated; otherwise adopted.
SlotRef registry_register(ThreadLifecycle *lc, HANDLE thread_handle,
                             DWORD tid, bool duplicate_handle);

/// Self-register: uses NtCurrentThread() and NtCurrentThreadId().
SlotRef registry_register_self(ThreadLifecycle *lc);

/// Remove a thread from the registry by slot reference.
void registry_deregister(SlotRef ref);

/// Remove a thread from the registry by lifecycle pointer.
/// No-op if lc is nullptr or not registered.
void registry_deregister(ThreadLifecycle *lc);

// --- Epoch Protocol ---

/// Pin the current global epoch in the calling thread's slot.
/// Must be called before any read operation (find_by_tid, borrow_handle, etc).
/// The calling thread must be registered.
void registry_pin();

/// Release the epoch pin.
void registry_unpin();

// --- Point Lookups (caller must be pinned) ---

/// Find a lifecycle by NT thread ID. Returns nullptr if not found.
ThreadLifecycle *registry_find_by_tid(DWORD tid);

/// Find a lifecycle by monotonic task_id. Linear scan — for robust mutex
/// owner-dead checks (not a hot path). Returns nullptr if not found.
ThreadLifecycle *registry_find_by_task_id(uint32_t task_id);

// --- Iteration ---

/// Type-erased visitor callback for the non-template loop in the .cpp.
using RegistryVisitorFn = bool (*)(void *ctx, ThreadLifecycle *lc);

/// Non-template bitmap scan loop. Lives in the .cpp — exactly one copy
/// in the binary. Template wrappers below generate thin trampolines.
bool registry_for_each_impl(RegistryVisitorFn visitor, void *ctx,
                                DWORD skip_tid);

/// Visit every live thread. `visitor(ThreadLifecycle*)` returns true to stop.
/// `skip_tid`: skip threads with this TID (0 = don't skip).
/// Returns true if a visitor returned true (early exit).
///
/// Auto-pins the epoch for the duration of the scan — no external EpochGuard
/// needed. The visitor may safely borrow handles and read lifecycle fields.
/// The visitor must not call register/deregister (would corrupt iteration —
/// bitmap changes may cause slots to be skipped or double-visited).
///
/// Icache-friendly: the bitmap scan loop exists once in the binary. The
/// lambda is wrapped in a static trampoline that the compiler can inline
/// at the call site (just the trampoline, not the whole loop).
template <typename Fn>
LIBC_INLINE bool registry_for_each(Fn &&visitor, DWORD skip_tid = 0) {
  using FnVal = cpp::remove_reference_t<Fn>;
  auto trampoline = [](void *ctx, ThreadLifecycle *lc) -> bool {
    return (*static_cast<FnVal *>(ctx))(lc);
  };
  return registry_for_each_impl(trampoline, &visitor, skip_tid);
}

/// Find the first lifecycle matching a predicate. Returns nullptr if none.
template <typename Pred>
LIBC_INLINE ThreadLifecycle *registry_find_if(Pred &&predicate) {
  ThreadLifecycle *result = nullptr;
  auto wrapper = [&predicate, &result](ThreadLifecycle *lc) -> bool {
    if (predicate(lc)) {
      result = lc;
      return true;
    }
    return false;
  };
  registry_for_each(wrapper, 0);
  return result;
}

// --- Batch TID Extraction ---

/// Collect TIDs of all live threads into `buf`. Returns the number written.
/// Skips threads with `skip_tid` (0 = don't skip). Does not exceed `capacity`.
/// The returned buffer is suitable for NtAlertMultipleThreadByThreadId.
///
/// For pagination: pass an opaque `cursor` to resume from a previous call.
/// 0 = start from the beginning. If `next_cursor` is non-null, it receives
/// the cursor for the next call (UINT32_MAX = all entries scanned).
/// The cursor packs (page_index, slot_index) so resuming never re-emits TIDs.
uint32_t registry_collect_tids(HANDLE *buf, uint32_t capacity,
                                  DWORD skip_tid = 0,
                                  uint32_t cursor = 0,
                                  uint32_t *next_cursor = nullptr);

/// Alert all registered threads via NtAlertMultipleThreadByThreadId.
/// Allocates a temporary buffer, collects all TIDs, issues one batch syscall.
/// Skips threads with `skip_tid` (0 = don't skip).
void registry_alert_all(DWORD skip_tid = 0);

// --- Handle Borrow (caller must be pinned) ---

/// Borrow the thread handle from a lifecycle. The caller must be epoch-pinned;
/// the epoch guarantee keeps the lifecycle (and its handle) alive.
/// Returns nullptr if the lifecycle has no handle.
LIBC_INLINE HANDLE registry_borrow_handle(ThreadLifecycle *lc) {
  if (!lc)
    return nullptr;
  return lc->thread_handle.load(cpp::MemoryOrder::ACQUIRE);
}

// --- Suspend / Resume ---

/// Suspend a thread via NtCreateThreadStateChange + NtChangeThreadState.
bool registry_suspend(ThreadLifecycle *lc);

/// Resume a thread via NtCreateThreadStateChange + NtChangeThreadState.
bool registry_resume(ThreadLifecycle *lc);

// --- Lifecycle ---

/// Deregister a lifecycle and retire it for deferred epoch-safe free.
/// Returns immediately — the lifecycle memory is freed later by
/// registry_try_reclaim when no reader can still reference it.
/// No-op if lc is nullptr or not registered.
///
/// Amortized reclamation: every 16th call triggers a non-blocking
/// reclaim pass that frees items from old epochs.
void registry_deregister_and_free(ThreadLifecycle *lc);

/// Non-blocking reclamation pass. Scans all live slots to find the
/// minimum pinned epoch, then frees all retired items (hash tables,
/// pages, lifecycles) stamped before that epoch. Items still protected
/// by active readers are pushed back onto the retired lists.
///
/// Cost: O(live_threads) bitmap reads + one walk of each retired list.
/// Called amortized by registry_deregister_and_free, but may also be
/// called explicitly to force a reclamation pass.
void registry_try_reclaim();

/// Blocking synchronize — waits until all epoch-pinned readers quiesce.
/// Used only by fork_reinit where the child process is single-threaded
/// and must free all resources synchronously before returning.
///
/// Safe for concurrent callers: fetch_add serializes epoch bumps, each
/// caller waits for its own target epoch independently.
void registry_synchronize();

/// Reinitialize the registry after fork(). Only `self` survives; all other
/// lifecycles are cleaned up and their slots freed. Single-threaded.
void registry_fork_reinit(ThreadLifecycle *self);

/// Unconditionally drain all retired lists (tables, pages, lifecycles).
/// For fork_reinit only — caller must guarantee no readers are active.
void registry_collect_retired();

// --- Stats ---

/// Number of currently registered threads.
LIBC_INLINE uint32_t registry_live_count();

// =========================================================================
// EpochGuard — RAII epoch pin with integrated lookups
// =========================================================================

class EpochGuard {
  bool active_;

public:
  LIBC_INLINE explicit EpochGuard() : active_(true) { registry_pin(); }
  LIBC_INLINE ~EpochGuard() {
    if (active_)
      registry_unpin();
  }

  EpochGuard(const EpochGuard &) = delete;
  EpochGuard &operator=(const EpochGuard &) = delete;
  EpochGuard(EpochGuard &&) = delete;
  EpochGuard &operator=(EpochGuard &&) = delete;

  LIBC_INLINE void release() {
    if (active_) {
      registry_unpin();
      active_ = false;
    }
  }

  /// Find a lifecycle by NT thread ID. Returns nullptr if not found.
  /// The returned pointer is valid until this guard is released/destroyed.
  LIBC_INLINE ThreadLifecycle *find(DWORD tid) {
    return registry_find_by_tid(tid);
  }

  /// Find a lifecycle by monotonic task_id (linear scan).
  /// The returned pointer is valid until this guard is released/destroyed.
  LIBC_INLINE ThreadLifecycle *find_by_task_id(uint32_t task_id) {
    return registry_find_by_task_id(task_id);
  }
};

// (The bitmap scan loop is in registry_for_each_impl in the .cpp.
//  Template wrappers above generate thin trampolines for type safety.)

LIBC_INLINE uint32_t registry_live_count() {
  // Implemented in .cpp; forward-declared here for inline access via PCB.
  // The actual load is from g_pcb.thread_registry.live_count.
  extern uint32_t registry_live_count_impl();
  return registry_live_count_impl();
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_H
