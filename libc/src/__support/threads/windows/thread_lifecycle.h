//===-- Per-thread lifecycle state for Windows ---------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ThreadLifecycle: separately-allocated per-thread state, root of every
// Windows-specific subsystem's per-thread storage.
//
// Layout invariants
// -----------------
//   * Inherits `ThreadRegistryNode` at offset 0 — Crystalline retire
//     dispatches on `kind == Lifecycle`. Stamp once at allocation.
//   * Inlines `ThreadAttributes attrib` so `pthread_t == &lc->attrib`.
//     `attrib.platform_data` back-points to the enclosing lifecycle —
//     this is the cross-thread libc-internal bypass: any caller with
//     `pthread_t pt` in scope reaches `lc` via `pt->platform_data`
//     without touching the thread registry.
//   * Inlines `signal_state::ThreadSignalState sig_state`. The
//     `signal` field always points at `&this->sig_state` for owned
//     threads; foreign-thread paths set it to a separate allocation.
//   * Carries `next_iter` (Harris-marked) for the global iter list
//     and `bucket_entry` (the entry that points back at this lifecycle
//     in the task_id hash) so deregister is O(1).
//
// FIELDS REMOVED (vs. pre-Crystalline registry):
//   * slot_page / slot_index / slot_generation — the page-slab
//     registry is gone.
//   * retired_next / retired_epoch — Crystalline batches retires;
//     no per-lifecycle retire-list link.
//   * pool_allocated / dynamically_allocated / signal_owned —
//     all lifecycles are slab-allocated and inline their signal
//     state. The single allocation IS the "where did this come from"
//     answer.
//   * attrib_storage — the lifecycle IS the storage. `attrib` lives
//     inline.
//   * owner_tid (atomic) — replaced with the immutable `tid` field
//     set once at register. NT TIDs are recycled by the kernel; the
//     stable identity is `task_id` (monotonic, never recycled).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LIFECYCLE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LIFECYCLE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/futex_word.h"
#include "src/__support/threads/windows/thread_local_word.h"
#include "src/__support/threads/windows/thread_registry_node.h"

struct __pthread_cleanup_t;

namespace LIBC_NAMESPACE_DECL {

struct BucketEntry; // From lockfree_hash.h.
struct ThreadLifecycle;

// ---------------------------------------------------------------------------
// Notification-word bit constants
// ---------------------------------------------------------------------------
//
// Bits in `ThreadLifecycle::notify_word`. Cross-thread writers set
// bits via `ThreadLocalWord::signal_or()`; the owning thread reads
// via `read()` (plain MOV, zero atomics). Low bits checked first
// via ctz.
namespace notify {
inline constexpr uint32_t CANCEL = 1u << 0; // Cancellation requested.
inline constexpr uint32_t SIGNAL = 1u << 1; // Pending signal(s).
inline constexpr uint32_t STOP = 1u << 2;   // Cooperative SIGSTOP/SIGTSTP.
inline constexpr uint32_t COORD_DEAD = 1u << 3; // Coordinator died mid-stop.
} // namespace notify

// ---------------------------------------------------------------------------
// Robust-mutex sidecar
// ---------------------------------------------------------------------------
//
// Bookkeeping for held robust mutexes — kept outside the mutex
// payload so the public pthread_mutex_t storage stays compact.
struct RobustRecord {
  RobustRecord *next;
  RobustRecord **prev_next;
  Futex *futex_word;
};

// ---------------------------------------------------------------------------
// ThreadHandle — canonical cross-thread reference
// ---------------------------------------------------------------------------
//
// `task_id` is the monotonic 30-bit identity that NEVER recycles in
// the process — every captured cross-thread reference stores this
// pair. `tid` is along for the ride: it's what
// `NtAlertThreadByThreadId` takes, but using `tid` alone for
// resolution is structurally unsafe (the kernel recycles TIDs).
// Resolution always goes through `task_id`; `tid` is read off the
// freshly-resolved lifecycle.
struct ThreadHandle {
  uint32_t tid;
  uint32_t task_id;

  LIBC_INLINE static constexpr ThreadHandle invalid() { return {0, 0}; }
  LIBC_INLINE constexpr bool is_valid() const { return task_id != 0; }
  LIBC_INLINE constexpr bool operator==(const ThreadHandle &o) const {
    return tid == o.tid && task_id == o.task_id;
  }
  LIBC_INLINE constexpr bool operator!=(const ThreadHandle &o) const {
    return !(*this == o);
  }

  // 64-bit packing for atomic stash. Layout: high 32 = task_id, low
  // 32 = tid. `task_id == 0` means invalid (matches is_valid()).
  LIBC_INLINE constexpr uint64_t pack() const {
    return (static_cast<uint64_t>(task_id) << 32) | static_cast<uint64_t>(tid);
  }
  LIBC_INLINE static constexpr ThreadHandle unpack(uint64_t raw) {
    return ThreadHandle{static_cast<uint32_t>(raw),
                        static_cast<uint32_t>(raw >> 32)};
  }
};

// ---------------------------------------------------------------------------
// ThreadLifecycle — per-thread, slab-allocated, Crystalline-managed
// ---------------------------------------------------------------------------
struct ThreadLifecycle : public ThreadRegistryNode {
  // === Identity (set once at register; immutable thereafter) ===

  // Monotonic 30-bit identifier; never recycled in the process. Fits
  // in the robust mutex `ROBUST_TID_MASK`. The canonical reference
  // for cross-thread lookups.
  uint32_t task_id;

  // NT thread ID — the only valid use is as a kernel-syscall
  // argument (`NtAlertThreadByThreadId`, `NtWaitForAlertByThreadId`).
  // NEVER use `tid` alone for libc-internal lookup or validation —
  // the kernel recycles TIDs.
  uint32_t tid;

  // === Iter list link (global Harris chain) ===

  // Bit 0 of the loaded value is the Harris mark; bit 1 is reserved
  // for Crystalline's internal slow-path masking. The unmarked
  // pointer goes through Crystalline `read()` for protection.
  cpp::Atomic<ThreadLifecycle *> next_iter;

  // === Hash entry pointing at this lifecycle ===
  //
  // Stored at register so deregister is O(1) — we know which
  // BucketEntry to mark+unlink+retire without walking the bucket
  // chain. Cleared on deregister. Crystalline-protected like any
  // BucketEntry pointer.
  cpp::Atomic<BucketEntry *> bucket_entry;

  // === Inline ThreadAttributes (pthread_t = &lc->attrib) ===
  //
  // `attrib.platform_data` back-points to `this`, enabling the
  // pthread_t-bypass cross-thread fast path:
  //     ThreadLifecycle *lc = pt->platform_data;
  ThreadAttributes attrib;

  // === Inline signal state ===
  //
  // The `signal` pointer below points at `&this->sig_state` for
  // every lifecycle owned by a libc-managed thread. Foreign threads
  // (registered via robust mutex / signal init paths) may have a
  // separately-allocated state pointed to by `signal`.
  signal_state::ThreadSignalState sig_state;

  // === Kernel handle ===

  // The single owned thread handle; cross-thread users borrow it
  // through registry helpers. Released by the join/detach cleanup
  // path on the final reference.
  cpp::Atomic<HANDLE> thread_handle;

  // === Join / detach ===

  Futex exit_word{0};                // Joiners wait here. 0 = exited.
  cpp::Atomic<uint32_t> detach_state; // DetachState enum values.

  // === Cancellation state ===

  // Bit 0: state (0 = ENABLE, 1 = DISABLE).
  // Bit 1: type  (0 = DEFERRED, 1 = ASYNCHRONOUS).
  // Owner accesses are RELAXED (same codegen as plain load/store on
  // x86); cross-thread reads are RELAXED too — stale type bits are
  // benign.
  cpp::Atomic<uint8_t> cancel_state;

  // Active blocking syscall frame. Points to a stack-allocated
  // SyscallFrame during alertable waits — enables pthread_cancel
  // and fork to cancel in-flight I/O.
  cpp::Atomic<signal_state::SyscallFrame *> active_syscall;

  // pthread_cleanup_push chain. Owning-thread access only.
  ::__pthread_cleanup_t *cancel_cleanup_stack;

  // === Stack metadata (for deallocation and fork TEB fixup) ===

  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  unsigned char owned_stack;

  // === Return value ===
  //
  // Lives here so it survives stack deallocation for detached
  // threads that transition to EXITING before the joiner reads it.
  union {
    void *posix_retval;
    int stdc_retval;
  } retval;

  // === Robust-mutex sidecar ===

  RobustRecord *robust_list;

  // === Subsystem pointers ===

  // For owned threads: always `&this->sig_state`. For foreign-thread
  // registrations (robust mutex / signal init), may point to a
  // separately-allocated state. Subsystem teardown handles both
  // shapes uniformly.
  //
  // Atomic so cross-thread signal senders observing `signal` while a
  // peer thread (parent during pthread_create / register_thread_state
  // on the child) writes the back-pointer have a defined happens-
  // before. The two writers always store the same pointer
  // (`&lc->sig_state`) for owned threads, so RELAXED reads from non-
  // owner contexts are safe; senders that need a defined value go
  // through ACQUIRE.
  cpp::Atomic<signal_state::ThreadSignalState *> signal;
  void *thread_ring; // ThreadRing*, lazy.
  void *atexit_data; // Reserved.

  // === Per-thread NUMA preferred node (lazy-resolved) ===
  //
  // The id of the NUMA node this thread should prefer for type-isolated
  // partition reservations (`alloc::partition::pick_node_for_alloc`).
  // Resolved on the first user-facing allocation that reaches the
  // selector — `NtGetCurrentProcessorNumberEx` + the sealed
  // `g_pcb.zone0.numa_topology()` cpu→node table — and stashed here
  // for every subsequent allocation.
  //
  // Lazy in the child thread (not parent-prequeried) because the
  // parent's CPU at `pthread_create` time is not the child's CPU; NT
  // picks the child's CPU during `NtCreateThreadEx` and even reading
  // the child's ideal-processor cross-thread would be stale by the
  // time `thread_entry_impl` runs. The first malloc on the child
  // resolves once and pays the syscall (~50 ns); every malloc after
  // that takes a single byte-load + predictable branch.
  //
  // Sentinel `kPreferredNodeUnresolved` (0xFF) marks "not yet probed"
  // — distinct from node 0 which is a valid id on every multi-socket
  // box. `0` is also a valid concrete node id, so the sentinel can't
  // be plain zero. `zero_lifecycle` and the fork-reinit hook both
  // re-stamp this field to the sentinel.
  //
  // Atomic because foreign signal handlers and hardened-build paths
  // may read the lifecycle's NUMA preference cross-thread; the ABI
  // cost on x86 is identical to a plain byte load/store.
  cpp::Atomic<uint8_t> preferred_node;

  // === Cross-cycle latched-alert expectation (owner-only) ===
  //
  // The single distinguisher between a real Futex/parking-lot wake
  // (which already published SIGNALED on the slot.link before
  // alerting) and a stale latched alert from a prior cycle.
  //
  // Set by:
  //   - `drain_waker_alert` when its in-cycle drain timed out
  //     without consuming.
  //   - SIGNALED-observation sites that exit without consuming the
  //     alert (Phase 4 entry SIGNALED), gated on
  //     slot.link.is_alert_fired() — the bit is the structural
  //     "alert in flight" publish from the waker's pre-mark CAS.
  //
  // Consumed on the next Phase 4 STATUS_ALERTED. Cleared on fork.
  // Replaces the prior futex_wake_epoch counter, which fed cross-
  // thread RELEASE bumps from each validated waker; coverage is now
  // entirely own-thread RELAXED + the LINK_ALERT_FIRED_BIT publish.
  cpp::Atomic<uint8_t> expect_late_alert{0};

  // === Per-thread notification word (last; cache-line aligned) ===
  //
  // Asymmetric primitive: owner reads via `read()` (plain MOV); cross-
  // thread writers set bits via `ThreadLocalWord::signal_or()`. The
  // alignas(64) inside `ThreadLocalWord` ensures it occupies its own
  // cache line — cross-thread writes never false-share with owner-hot
  // fields. `init()` must run on the owning thread post-create;
  // `fork_reinit()` + `write(0)` runs in the fork child.
  ThreadLocalWord notify_word;
};

// Trivially-constructible invariants.
static_assert(__is_trivially_constructible(cpp::Atomic<uint32_t>),
              "Atomic<uint32_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<uint8_t>),
              "Atomic<uint8_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<HANDLE>),
              "Atomic<HANDLE> must be trivially constructible");

// Sentinel for `ThreadLifecycle::preferred_node`. Stored when the field
// has not yet been resolved by the partition-layer NUMA selector. Any
// real node id is in `[0, 63]` (cf. `kNumaCpuTableSize` budget), so
// `0xFF` is unambiguous.
inline constexpr uint8_t kPreferredNodeUnresolved = 0xFF;

// ---------------------------------------------------------------------------
// Process-wide monotonic task_id counter
// ---------------------------------------------------------------------------
//
// Starts at 1; 0 reserved for "no owner" in the robust mutex
// encoding. 30-bit range (~1 billion) is sufficient for any single
// process lifetime — 1 thread/μs sustained for ~12 days exhausts it.
//
// Wrap-trap: task_id MUST stay strictly within bits [1, 2^30 - 1] to
// fit ROBUST_TID_MASK. A wrap would silently alias a live thread's
// task_id with a future one and corrupt cross-thread lookups. Trap on
// reaching the ceiling rather than wrap.
inline constexpr uint32_t kTaskIdMax = (1u << 30) - 1;
inline cpp::Atomic<uint32_t> g_next_task_id{1};

inline uint32_t allocate_task_id() {
  // RELAXED fetch_add is fine — task_ids are only used as opaque
  // identifiers; no synchronization through the counter itself.
  uint32_t id =
      g_next_task_id.fetch_add(1, cpp::MemoryOrder::RELAXED);
  // The fetch_add may have wrapped the 32-bit counter past 2^30 - 1.
  // Trap once we cross the ceiling. Idempotent — every subsequent
  // allocator from this counter traps too, so the process exits as a
  // unit rather than slipping into an aliased-task_id state. With a
  // monotonic counter and no reseeding, the first id past the ceiling
  // is the only one that races with kTaskIdMax across CASes.
  if (LIBC_UNLIKELY(id == 0 || id > kTaskIdMax))
    __builtin_trap();
  return id;
}

// ---------------------------------------------------------------------------
// Lifecycle TLS root — single TEB slot
// ---------------------------------------------------------------------------

// Initialize the lifecycle TLS slot. Phase 4 startup hook.
void lifecycle_init();

// Get/set the current thread's lifecycle root.
ThreadLifecycle *get_current_lifecycle();
void set_current_lifecycle(ThreadLifecycle *lc);

// TLS index accessor (for subsystems that need direct TEB access).
DWORD get_lifecycle_tls_index();

// ---------------------------------------------------------------------------
// Lifecycle allocation (slab-backed)
// ---------------------------------------------------------------------------
//
// All lifecycles come from the process-wide SlabPool. The allocator
// stamps the `ThreadRegistryNode` header with `kind = Lifecycle` and
// initializes the Crystalline node fields via `init_node` at
// register time (NOT here — this hands back zero-initialized
// memory the registry then prepares).
//
// Free is the FreeFn-side path invoked by Crystalline reclamation
// once no reservation pins the lifecycle. Direct callers do not free
// lifecycles; they go through `registry_deregister_and_retire` which
// hands the lifecycle to Crystalline.
ThreadLifecycle *alloc_lifecycle();
void free_lifecycle(ThreadLifecycle *lc);

// Zero-initialize a freshly-allocated lifecycle. Memset alone is
// insufficient because:
//   * ThreadRegistryNode::kind defaults to 0 (invalid kind) —
//     stamp Lifecycle so the FreeFn dispatcher routes correctly
//     on a never-registered lifecycle.
//   * detach_state defaults to 0, which is NOT a valid DetachState
//     value — the lifecycle_cleanup switch would trap on it.
//     Stamp DETACHED, the safe default for foreign-thread paths
//     (signal_state's lazy lifecycle alloc, main thread init,
//     anything that goes through alloc_lifecycle without later
//     overriding via Thread::run). Threads created through
//     Thread::run override this to JOINABLE or explicit DETACHED
//     based on pthread_attr.
inline void zero_lifecycle(ThreadLifecycle *lc) {
  __builtin_memset(lc, 0, sizeof(ThreadLifecycle));
  lc->kind = ThreadRegistryNodeKind::Lifecycle;
  lc->detach_state.store(uint32_t(DetachState::DETACHED),
                         cpp::MemoryOrder::RELAXED);
  // `preferred_node` defaults to the unresolved sentinel — the
  // partition-layer NUMA selector lazily resolves it on first use.
  // Plain zero (the memset default) would alias node 0, which is a
  // valid concrete id on every multi-socket system.
  lc->preferred_node.store(kPreferredNodeUnresolved,
                           cpp::MemoryOrder::RELAXED);
}

// Sentinel value stored in `exit_word` before the thread starts running.
inline constexpr uint32_t EXIT_WORD_LIVE = 0xABCD1234;

// Sentinel pointer stored briefly in `lc->bucket_entry` to claim the
// registration window. The first registrar (parent or self-register
// race) CAS-installs this value as `bucket_entry` BEFORE doing any of
// the iter_prepend / hash insert / handle dup work; concurrent
// registrars observe non-null and short-circuit (verifying identity)
// after a hardware-monitored spin until the winner publishes either a
// real entry or rolls back to null on OOM.
//
// The value `(BucketEntry*)(uintptr_t)1` is misaligned for a real
// BucketEntry (which is at least 8-byte aligned via slab pool), so it
// can never alias a valid pointer. Bit 0 is also never set on a real
// entry pointer because slab slots are always at least 16-byte aligned
// in the BucketEntry pool — the sentinel is unambiguous.
LIBC_INLINE BucketEntry *registration_pending_sentinel() {
  return reinterpret_cast<BucketEntry *>(static_cast<uintptr_t>(1));
}
LIBC_INLINE bool is_registration_pending(BucketEntry *p) {
  return reinterpret_cast<uintptr_t>(p) == static_cast<uintptr_t>(1);
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LIFECYCLE_H
