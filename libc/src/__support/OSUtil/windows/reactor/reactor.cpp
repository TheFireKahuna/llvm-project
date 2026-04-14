//===-- Process-wide IOCP reactor -- implementation -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single IOCP + drain thread pool that multiplexes all runtime-internal and
// application-facing (epoll) asynchronous events. The NT kernel's native
// completion model promoted to process-wide infrastructure.
//
// Completion key encoding (64-bit):
//   [generation:15][tag:1][pointer:48]
//   Bit 48 is always set -- impossible for a valid user-mode pointer.
//   Bits [63:49] carry a 15-bit truncated generation counter for ABA
//   detection on slot reuse. Bits [47:0] are the ReactorSlot pointer.
//
// Slot allocation:
//   Reactor slots are dynamically allocated from a SlabPool. No fixed
//   slot limit -- capacity grows on demand. An intrusive doubly-linked
//   list of active slots enables iteration for fini/fork_reinit cleanup.
//
// Synchronization model:
//   - Registration (watch/unwatch): mutex-protected, rare (subsystem init)
//   - Dispatch (drain pool + epoll_wait inline): lock-free, hot path
//   - Per-slot exclusion: CAS on dispatching flag serializes callbacks for
//     the same registration across drain threads. Different registrations
//     dispatch concurrently.
//   - Unwatch safety: double-check generation + spin-wait on dispatching flag
//     guarantees no callback is running after unwatch() returns
//
// Routing model:
//   Drain pool threads are the primary IOCP consumers. They dispatch
//   reactor-keyed completions via slot pointer lookup. Non-reactor completions
//   (epoll events) are forwarded to an installed CompletionRouter callback,
//   which pushes them to per-epoll-instance pending queues. epoll_wait also
//   does non-blocking IOCP flushes, dispatching reactor completions inline
//   via dispatch_inline().
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_api.h"
#include "src/__support/OSUtil/windows/nt/nt_job.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/threads/raw_mutex.h"
#include "src/__support/threads/windows/futex_addr.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace reactor {

namespace {

// =========================================================================
// Constants
// =========================================================================

// Maximum completions dequeued per NtRemoveIoCompletionEx call.
constexpr ULONG DRAIN_BATCH_SIZE = 32;

// Drain thread stack size. Callbacks must be short (pend + return),
// so 64KB is generous.
constexpr SIZE_T DRAIN_STACK_SIZE = 0x10000;

// =========================================================================
// Completion key encoding
// =========================================================================
//
// The IOCP CompletionKey (PVOID, 64-bit) encodes a tagged pointer to the
// SlabPool-allocated ReactorSlot, plus a truncated generation counter.
//
// Layout: [generation:15][tag:1][pointer:48]
//
//   Bit 48: always set (reactor discriminant -- impossible for user-mode ptrs)
//   Bits [63:49]: 15-bit truncated generation (32768 values, ample ABA margin)
//   Bits [47:0]: ReactorSlot pointer (valid user-mode address, bit 47 = 0)
//
// The generation in the key is compared against the slot's generation to
// detect stale completions from unwatched/reused slots. Without key-side
// generation, a freed+reallocated slot at the same address would cause
// stale completions to dispatch to the wrong callback.

constexpr unsigned KEY_GEN_SHIFT = 49;
constexpr uintptr_t KEY_GEN_MASK = 0x7FFF; // 15 bits
constexpr uintptr_t KEY_PTR_MASK = (1ULL << 48) - 1; // bits [47:0]

PVOID pack_key(ReactorSlot *slot, uint32_t generation) {
  return reinterpret_cast<PVOID>(
      REACTOR_KEY_TAG |
      (static_cast<uintptr_t>(generation & KEY_GEN_MASK) << KEY_GEN_SHIFT) |
      (reinterpret_cast<uintptr_t>(slot) & KEY_PTR_MASK));
}

ReactorSlot *unpack_slot(PVOID key) {
  uintptr_t k = reinterpret_cast<uintptr_t>(key);
  if (!(k & REACTOR_KEY_TAG))
    return nullptr;
  return reinterpret_cast<ReactorSlot *>(k & KEY_PTR_MASK);
}

uint32_t unpack_generation(PVOID key) {
  return static_cast<uint32_t>(
      (reinterpret_cast<uintptr_t>(key) >> KEY_GEN_SHIFT) & KEY_GEN_MASK);
}

// =========================================================================
// Slot types
// =========================================================================

enum SlotKind : uint8_t {
  SLOT_FREE = 0, // Available for allocation (or deferred-free pending).
  SLOT_WCP = 1,  // WaitCompletionPacket-based watch.
  SLOT_ALPC = 2, // ALPC completion port association.
  SLOT_JOB = 3,  // Job object IOCP association.
};

// =========================================================================
// Reactor slot -- one per registration, SlabPool-allocated
// =========================================================================
//
// Slots are allocated via SlabPool::tls_alloc() in watch() and freed via
// SlabPool::free() in unwatch(). The drain thread reads slots without the
// mutex, relying on the atomic generation field for correctness.
//
// Active slot list: doubly-linked intrusive list for fini/fork_reinit
// iteration. Protected by the reactor mutex.

} // anonymous namespace (SlotKind needs to be visible to ReactorSlot)

struct ReactorSlot {
  ReactorCallback callback; // Dispatch target.
  void *context;            // Opaque context for callback.
  HANDLE target;            // Watched handle (WCP) or ALPC port.
  HANDLE wcp;               // WaitCompletionPacket handle (null for ALPC).

  // Atomic generation counter. Incremented on every unwatch. The drain
  // thread compares (generation & KEY_GEN_MASK) against the generation
  // encoded in the completion key to detect stale completions.
  cpp::Atomic<uint32_t> generation{0};

  // True while a callback is executing on the drain thread. Used by
  // unwatch() to spin-wait for in-flight dispatch to complete.
  cpp::Atomic<bool> dispatching{false};

  SlotKind kind{SLOT_FREE};

  // Active slot list linkage (mutex-protected).
  ReactorSlot *active_next{nullptr};
  ReactorSlot *active_prev{nullptr};
};

namespace {

// =========================================================================
// File-local statics — implementation-private, NOT in the PCB
// =========================================================================
//
// The PCB holds fixed kernel objects and metadata (iocp, reserve,
// drain_thread, shutdown, drain_cleaned, heartbeat, generation, router)
// that represent observable process-wide state. The slot pool, active
// list, and registration lock are implementation mechanisms that no
// external subsystem accesses — they stay file-local.

SlabPool g_slot_pool;
ReactorSlot *g_active_head = nullptr;
RawMutex g_lock;

// Validate CompletionRouter ↔ uintptr_t ABI compatibility. The PCB stores
// the router as Atomic<uintptr_t> to avoid pulling reactor.h into
// process_control_block.h while providing atomic access.
static_assert(sizeof(CompletionRouter) == sizeof(uintptr_t),
              "CompletionRouter must be pointer-sized for PCB uintptr_t storage");

// =========================================================================
// Drain thread handle accessor
// =========================================================================
//
// ReactorState stores drain_thread (index 0) followed by
// drain_threads_reserved[MAX-1] (indices 1..MAX-1). This helper provides
// uniform indexed access without UB (no pointer arithmetic across members).

HANDLE &drain_handle(ReactorState &rr, uint32_t i) {
  return i == 0 ? rr.drain_thread : rr.drain_threads_reserved[i - 1];
}

// =========================================================================
// Active processor count (from KUSER_SHARED_DATA)
// =========================================================================
//
// KUSER_SHARED_DATA is mapped read-only at a fixed VA in every process.
// Offset 0x03C0: ActiveProcessorCount — same source ntdll uses internally.

uint32_t active_processor_count() {
  return *reinterpret_cast<const volatile uint32_t *>(0x7FFE03C0ull);
}

// =========================================================================
// Active slot list helpers (must hold g_lock)
// =========================================================================

void active_list_insert(ReactorSlot *slot) {
  slot->active_prev = nullptr;
  slot->active_next = g_active_head;
  if (g_active_head)
    g_active_head->active_prev = slot;
  g_active_head = slot;
}

void active_list_remove(ReactorSlot *slot) {
  if (slot->active_prev)
    slot->active_prev->active_next = slot->active_next;
  else
    g_active_head = slot->active_next;
  if (slot->active_next)
    slot->active_next->active_prev = slot->active_prev;
  slot->active_next = nullptr;
  slot->active_prev = nullptr;
}

// =========================================================================
// Shared dispatch logic
// =========================================================================
//
// Used by both drain pool threads and dispatch_inline(). The CAS +
// double-check generation pattern is the core correctness mechanism:
//
//   1. Load generation from slot (ACQUIRE)
//   2. Compare truncated generation against key-side generation
//   3. CAS dispatching false→true (ACQ_REL) -- per-slot exclusion
//   4. Re-check generation (ACQUIRE) -- catches unwatch between steps 1-3
//   5. Call callback
//   6. Clear dispatching (RELEASE)
//   7. If slot was detached during callback, complete the deferred free

void dispatch_reactor_completion(PVOID key, NTSTATUS status,
                                 ULONG_PTR information) {
  ReactorSlot *slot = unpack_slot(key);
  if (!slot)
    return;

  uint32_t key_gen = unpack_generation(key);

  // Step 1-2: Generation check (lock-free).
  uint32_t slot_gen = slot->generation.load(cpp::MemoryOrder::ACQUIRE);
  if ((slot_gen & KEY_GEN_MASK) != key_gen)
    return;

  // Step 3: Acquire exclusive dispatch rights via CAS.
  // With multiple drain threads, this serializes per-slot dispatch:
  // at most one thread enters the callback for a given slot. If
  // another thread is already dispatching, the CAS fails and we
  // return -- safe because WCP is one-shot (no duplicate completions)
  // and ALPC/JOB callbacks drain all queued messages, so the running
  // callback handles everything the dropped completion would have.
  bool expected = false;
  if (!slot->dispatching.compare_exchange_strong(
          expected, true, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::RELAXED))
    return;

  // Step 4: Double-check generation after acquiring dispatch rights.
  // Catches unwatch() that raced between step 1 and the CAS above.
  if ((slot->generation.load(cpp::MemoryOrder::ACQUIRE) & KEY_GEN_MASK) !=
      key_gen) {
    slot->dispatching.store(false, cpp::MemoryOrder::RELEASE);
    return;
  }

  // Step 5: Dispatch.
  slot->callback(slot->context, status, information);

  // Step 6: Clear dispatching flag.
  slot->dispatching.store(false, cpp::MemoryOrder::RELEASE);

  // Step 7: If detach() was called during the callback, the slot was marked
  // SLOT_FREE but not returned to the pool (detach can't free while we still
  // reference it). Complete the deferred free now.
  if (slot->kind == SLOT_FREE)
    SlabPool::free(slot);
}

// Route a single completion: reactor-keyed -> dispatch, else -> router.
// router_fn is the CompletionRouter stored as uintptr_t in the PCB; cast
// here to recover the function pointer type.
void route_completion(const FILE_IO_COMPLETION_INFORMATION &ci,
                      uintptr_t router_fn) {
  PVOID key = ci.KeyContext;

  // Null key = wakeup sentinel (shutdown, manual wake).
  if (!key)
    return;

  if (is_reactor_key(key)) {
    dispatch_reactor_completion(key, ci.IoStatusBlock.Status,
                                ci.IoStatusBlock.Information);
  } else if (router_fn) {
    reinterpret_cast<CompletionRouter>(router_fn)(
        key, ci.ApcContext, ci.IoStatusBlock.Status,
        ci.IoStatusBlock.Information);
  }
  // If no router installed, non-reactor completions are silently dropped.
  // This is correct: before epoll is initialized, there are no epoll
  // completions on the IOCP.
}

// =========================================================================
// Drain thread self-cleanup
// =========================================================================
//
// The drain thread owns slot cleanup on shutdown. This eliminates the
// need for fini() to ever touch slots, removing the entire class of
// "fini frees while drain thread references" races. The drain thread
// is the last consumer of slots — it cleans up after itself.

void drain_cleanup_slots() {
  g_lock.lock();
  ReactorSlot *slot = g_active_head;
  while (slot) {
    ReactorSlot *next = slot->active_next;
    if (slot->kind == SLOT_WCP && slot->wcp) {
      ::NtCancelWaitCompletionPacket(slot->wcp, 1);
      ::NtClose(slot->wcp);
    }
    // Don't call SlabPool::free() under the lock — free is lock-free
    // but could trigger cross-thread operations on the slab. Collect
    // and free after releasing the lock to minimize hold time.
    // Actually, SlabPool::free is safe to call here — it's the owner
    // thread (drain thread) freeing. The lock protects the active list,
    // not the pool. Free inline for simplicity.
    slot->kind = SLOT_FREE;
    slot->active_next = nullptr;
    slot->active_prev = nullptr;
    SlabPool::free(slot);
    slot = next;
  }
  g_active_head = nullptr;
  g_lock.unlock();
}

// =========================================================================
// Drain thread
// =========================================================================

DWORD NTAPI drain_thread_proc(PVOID param) {
  uint32_t my_index =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
  ReactorState &rr = g_pcb.reactor;
  FILE_IO_COMPLETION_INFORMATION entries[DRAIN_BATCH_SIZE];

  while (!rr.shutdown.load(cpp::MemoryOrder::ACQUIRE)) {
    ULONG count = 0;
    NTSTATUS st = ::NtRemoveIoCompletionEx(
        rr.iocp, entries, DRAIN_BATCH_SIZE, &count,
        nullptr, // Infinite wait -- woken by completions or shutdown post.
        1);   // Alertable -- allows APC delivery to this thread.

    if (st == STATUS_USER_APC || st == STATUS_ALERTED)
      continue;

    if (!NT_SUCCESS(st))
      continue;

    // Snapshot the router once per batch (ACQUIRE pairs with RELEASE in
    // set_completion_router). A single load per batch is sufficient — the
    // router is set once during epoll init and never changed afterward.
    uintptr_t router_fn = rr.router.load(cpp::MemoryOrder::ACQUIRE);
    for (ULONG i = 0; i < count; ++i)
      route_completion(entries[i], router_fn);

    // Bump per-thread epoch (RCU fence target) and global heartbeat.
    // Per-thread epoch: fence_drain_cycle() snapshots and waits for
    // each thread's epoch to advance, giving an airtight guarantee
    // that every thread has cycled. Cache-line aligned, no contention.
    // Global heartbeat: drain_thread_healthy() checks any-thread
    // progress — simpler interface for health monitoring.
    rr.drain_epochs[my_index].value.fetch_add(1, cpp::MemoryOrder::RELEASE);
    rr.heartbeat.fetch_add(1, cpp::MemoryOrder::RELAXED);

    // Wake any fence_drain_cycle() caller parked on this epoch.
    // Hot-path cost when no waiter: hash + bucket.live_count load → 0
    // → return. One extra cache-line read per batch, negligible.
    futex_addr::wake(
        reinterpret_cast<volatile uint32_t *>(
            &rr.drain_epochs[my_index].value.val),
        1);
  }

  // Shutdown: coordinate exit across drain pool threads.
  // The last thread out owns cleanup — all others have left the dispatch
  // loop, so no concurrent dispatch is possible after this point.
  uint32_t prev = rr.drain_exit_count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  if (prev + 1 == rr.drain_thread_count) {
    drain_cleanup_slots();
    rr.drain_cleaned.store(1, cpp::MemoryOrder::RELEASE);
    // Wake fini() if it's parked in futex_addr::wait on drain_cleaned.
    futex_addr::wake(&rr.drain_cleaned, 1);
  }

  return 0;
}

// =========================================================================
// Drain thread lifecycle
// =========================================================================

HANDLE start_drain_thread(uint32_t index) {
  HANDLE thread = nullptr;
  auto oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateThreadEx(
      &thread,
      THREAD_ALL_ACCESS,
      &oa,               // Non-inheritable — internal drain thread.
      NtCurrentProcess(),
      reinterpret_cast<PVOID>(drain_thread_proc),
      reinterpret_cast<PVOID>(static_cast<uintptr_t>(index)),
      THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH |
          THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,
      0,                 // ZeroBits
      DRAIN_STACK_SIZE,
      DRAIN_STACK_SIZE,
      nullptr);          // AttributeList
  if (!NT_SUCCESS(st))
    return nullptr;

  // Thread is created running (not suspended). SKIP_THREAD_ATTACH prevents
  // the loader from sending DLL_THREAD_ATTACH notifications, avoiding a
  // deadlock when this is called during DLL_PROCESS_ATTACH (loader lock held).
  // HIDE_FROM_DEBUGGER keeps the drain thread out of casual debugger views.
  return thread;
}

void wake_drain_thread(ReactorState &rr) {
  if (rr.iocp && rr.reserve) {
    ::NtSetIoCompletionEx(rr.iocp, rr.reserve,
                          nullptr, // Null key = wakeup sentinel.
                          nullptr, STATUS_SUCCESS, 0);
  }
}

// No-op APC routine. Delivery itself is the effect — it causes any
// alertable wait to return STATUS_USER_APC, breaking the drain loop
// back to the shutdown check.
void NTAPI shutdown_apc(PVOID, PVOID, PVOID) {}

} // anonymous namespace

// =========================================================================
// Public API -- Lifecycle
// =========================================================================

int init() {
  ReactorState &rr = g_pcb.reactor;

  // First-time init: start generation at 1 so generation 0 is never assigned
  // (matches zero-init state of freed slots for stale-completion rejection).
  // After fork, the child inherits the parent's counter — this is a no-op.
  if (rr.generation.load(cpp::MemoryOrder::RELAXED) == 0)
    rr.generation.store(1, cpp::MemoryOrder::RELAXED);

  // Create the process-wide IOCP. NumberOfConcurrentThreads=0 (unlimited)
  // because both the drain thread and epoll_wait threads may dequeue
  // concurrently during non-blocking flush operations.
  auto iocp_oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateIoCompletion(&rr.iocp, IO_COMPLETION_ALL_ACCESS,
                                       &iocp_oa, 0);
  if (!NT_SUCCESS(st))
    return -1;

  // Allocate reserve object for guaranteed-delivery posts (shutdown
  // wakeup, synthetic epoll completions). Cannot fail under memory pressure.
  auto rsv_oa = windows::internal_oa();
  st = ::NtAllocateReserveObject(&rr.reserve, &rsv_oa,
                                 MemoryReserveIoCompletion);
  if (!NT_SUCCESS(st)) {
    ::NtClose(rr.iocp);
    rr.iocp = nullptr;
    return -1;
  }

  // Initialize the slot pool. sizeof(ReactorSlot) determines the slab
  // slot size. TLS gives lock-free alloc on the hot path.
  g_slot_pool.init(sizeof(ReactorSlot));
  g_slot_pool.init_tls();

  // Start the drain pool. Size is clamped to [2, MAX_DRAIN_THREADS]:
  //   - Floor of 2 gives resilience against a single stuck callback
  //   - Ceiling of MAX keeps resource use bounded
  // IOCP naturally load-balances completions across waiting threads.
  rr.shutdown.store(0, cpp::MemoryOrder::RELEASE);
  rr.drain_exit_count.store(0, cpp::MemoryOrder::RELAXED);

  uint32_t n = active_processor_count();
  if (n < 2)
    n = 2;
  if (n > ReactorState::MAX_DRAIN_THREADS)
    n = ReactorState::MAX_DRAIN_THREADS;
  rr.drain_thread_count = n;

  for (uint32_t i = 0; i < n; ++i) {
    drain_handle(rr, i) = start_drain_thread(i);
    if (!drain_handle(rr, i)) {
      // Failed to start thread i. If we have at least one thread,
      // proceed with a reduced pool. Otherwise, fail init entirely.
      rr.drain_thread_count = i;
      if (i == 0) {
        ::NtClose(rr.reserve);
        rr.reserve = nullptr;
        ::NtClose(rr.iocp);
        rr.iocp = nullptr;
        return -1;
      }
      break;
    }
  }

  return 0;
}

void fini() {
  ReactorState &rr = g_pcb.reactor;
  rr.shutdown.store(1, cpp::MemoryOrder::RELEASE);

  if (rr.drain_thread_count > 0) {
    // Wake all drain threads through both available mechanisms:
    //   - IOCP sentinels: one per thread, wakes NtRemoveIoCompletionEx
    //   - APCs: one per thread, wakes any alertable wait inside a callback
    for (uint32_t i = 0; i < rr.drain_thread_count; ++i) {
      wake_drain_thread(rr);
      if (drain_handle(rr, i))
        ::NtQueueApcThreadEx2(drain_handle(rr, i), nullptr,
                              QUEUE_USER_APC_FLAGS_NONE, shutdown_apc,
                              nullptr, nullptr, nullptr);
    }

    // Adaptive convergence: the last drain thread to exit cleans up all
    // active slots, stores drain_cleaned=1, and wakes us via futex. A
    // healthy pool reaches that point in microseconds after being woken.
    //
    //   Phase 1: Hardware-monitored spin (UMWAIT/MWAITX if available,
    //            wakes on cache-line write — near-zero power, sub-μs)
    //   Phase 2: futex_addr park on drain_cleaned (covers scheduling
    //            delays and in-flight callbacks, 100ms timeout bound)
    //
    // If all drain threads are stuck in buggy callbacks, they never
    // reach exit coordination. Slots are abandoned — the OS reclaims
    // all process memory on exit. No force-termination, no corruption.

    // Phase 1: Hardware spin on drain_cleaned (value 0 → non-zero).
    spin_wait::spin_on_raw_u32(&rr.drain_cleaned.val, 0u);

    // Phase 2: If the spin didn't converge, park in the futex parking
    // lot. The last drain thread out stores drain_cleaned=1 and calls
    // futex_addr::wake to unpark us.
    if (!rr.drain_cleaned.load(cpp::MemoryOrder::ACQUIRE)) {
      LARGE_INTEGER timeout;
      timeout.QuadPart = -1000000LL; // 100ms relative.
      futex_addr::wait_nt(&rr.drain_cleaned, 0u, &timeout);
    }

    // Close all drain thread handles.
    for (uint32_t i = 0; i < rr.drain_thread_count; ++i) {
      if (drain_handle(rr, i)) {
        ::NtClose(drain_handle(rr, i));
        drain_handle(rr, i) = nullptr;
      }
    }
    rr.drain_thread_count = 0;
  }

  if (rr.reserve) {
    ::NtClose(rr.reserve);
    rr.reserve = nullptr;
  }
  if (rr.iocp) {
    ::NtClose(rr.iocp);
    rr.iocp = nullptr;
  }

  rr.router.store(0, cpp::MemoryOrder::RELAXED);
}

void fork_reinit() {
  ReactorState &rr = g_pcb.reactor;

  // In the fork child, all reactor handles (IOCP, reserve, WCPs, drain
  // threads) were created with internal_oa() (non-inheritable), so they
  // don't exist in the child's handle table. Null all pointers.
  for (uint32_t i = 0; i < rr.drain_thread_count; ++i)
    drain_handle(rr, i) = nullptr;
  rr.drain_thread_count = 0;

  // Walk the active slot list, null stale WCP pointers, free all slots.
  ReactorSlot *slot = g_active_head;
  while (slot) {
    ReactorSlot *next = slot->active_next;
    slot->wcp = nullptr;
    // Bump generation so any stale completions are discarded.
    slot->generation.fetch_add(1, cpp::MemoryOrder::RELAXED);
    SlabPool::free(slot);
    slot = next;
  }
  g_active_head = nullptr;

  rr.reserve = nullptr;
  rr.iocp = nullptr;

  g_lock.reset_for_fork();

  // Reset all runtime flags for the child.
  rr.shutdown.store(0, cpp::MemoryOrder::RELAXED);
  rr.drain_exit_count.store(0, cpp::MemoryOrder::RELAXED);
  rr.drain_cleaned.store(0, cpp::MemoryOrder::RELAXED);
  // Preserve the router -- epoll reinstalls it in its fork_reinit.
  // (Actually, the function pointer is still valid across fork.)

  // init() reinitializes the slot pool (init + init_tls) and starts
  // a fresh drain pool for the child.
  init();
}

// =========================================================================
// Public API -- Watch Registration
// =========================================================================

ReactorToken watch(HANDLE waitable, ReactorCallback cb, void *context) {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || !cb || !waitable)
    return INVALID_TOKEN;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return INVALID_TOKEN;

  // Allocate a slot from the SlabPool (lock-free TLS hot path).
  auto *slot =
      static_cast<ReactorSlot *>(g_slot_pool.tls_alloc());
  if (!slot)
    return INVALID_TOKEN;

  // Assign a globally unique generation before any field init.
  // This prevents ABA: a stale completion from a prior occupant of this
  // SlabPool address will carry a different generation in its key.
  uint32_t gen = rr.generation.fetch_add(1, cpp::MemoryOrder::RELAXED);

  // Initialize slot fields. Don't memset — we already set generation above
  // and SlabPool guarantees zero-on-free for all other fields.
  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->wcp = nullptr;
  slot->generation.store(gen, cpp::MemoryOrder::RELAXED);
  slot->dispatching.store(false, cpp::MemoryOrder::RELAXED);
  slot->kind = SLOT_FREE;
  slot->active_next = nullptr;
  slot->active_prev = nullptr;

  g_lock.lock();

  HANDLE wcp = nullptr;
  auto wcp_oa = windows::internal_oa();
  NTSTATUS st = ::NtCreateWaitCompletionPacket(
      &wcp, WAIT_COMPLETION_PACKET_ALL_ACCESS, &wcp_oa);
  if (!NT_SUCCESS(st)) {
    g_lock.unlock();
    SlabPool::free(slot);
    return INVALID_TOKEN;
  }

  BOOLEAN already_signaled = 0;
  st = ::NtAssociateWaitCompletionPacket(
      wcp, rr.iocp, waitable,
      pack_key(slot, gen),
      nullptr, STATUS_SUCCESS, 0, &already_signaled);
  if (!NT_SUCCESS(st)) {
    ::NtClose(wcp);
    g_lock.unlock();
    SlabPool::free(slot);
    return INVALID_TOKEN;
  }

  slot->callback = cb;
  slot->context = context;
  slot->target = waitable;
  slot->wcp = wcp;
  slot->kind = SLOT_WCP;

  active_list_insert(slot);

  g_lock.unlock();

  return ReactorToken{slot, gen};
}

ReactorToken watch_alpc(HANDLE alpc_port, ReactorCallback cb, void *context) {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || !cb || !alpc_port)
    return INVALID_TOKEN;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return INVALID_TOKEN;

  auto *slot =
      static_cast<ReactorSlot *>(g_slot_pool.tls_alloc());
  if (!slot)
    return INVALID_TOKEN;

  uint32_t gen = rr.generation.fetch_add(1, cpp::MemoryOrder::RELAXED);

  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->wcp = nullptr;
  slot->generation.store(gen, cpp::MemoryOrder::RELAXED);
  slot->dispatching.store(false, cpp::MemoryOrder::RELAXED);
  slot->kind = SLOT_FREE;
  slot->active_next = nullptr;
  slot->active_prev = nullptr;

  g_lock.lock();

  ALPC_PORT_ASSOCIATE_COMPLETION_PORT assoc;
  assoc.CompletionKey = pack_key(slot, gen);
  assoc.CompletionPort = rr.iocp;

  NTSTATUS st = ::NtAlpcSetInformation(
      alpc_port, AlpcAssociateCompletionPortInformation,
      &assoc, sizeof(assoc));
  if (!NT_SUCCESS(st)) {
    g_lock.unlock();
    SlabPool::free(slot);
    return INVALID_TOKEN;
  }

  slot->callback = cb;
  slot->context = context;
  slot->target = alpc_port;
  slot->wcp = nullptr;
  slot->kind = SLOT_ALPC;

  active_list_insert(slot);

  g_lock.unlock();

  return ReactorToken{slot, gen};
}

ReactorToken watch_job(HANDLE job, ReactorCallback cb, void *context) {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || !cb || !job)
    return INVALID_TOKEN;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return INVALID_TOKEN;

  auto *slot =
      static_cast<ReactorSlot *>(g_slot_pool.tls_alloc());
  if (!slot)
    return INVALID_TOKEN;

  uint32_t gen = rr.generation.fetch_add(1, cpp::MemoryOrder::RELAXED);

  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->wcp = nullptr;
  slot->generation.store(gen, cpp::MemoryOrder::RELAXED);
  slot->dispatching.store(false, cpp::MemoryOrder::RELAXED);
  slot->kind = SLOT_FREE;
  slot->active_next = nullptr;
  slot->active_prev = nullptr;

  g_lock.lock();

  // Associate the job with the reactor's IOCP. The packed reactor key
  // becomes the CompletionKey for all job notifications.
  JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc;
  assoc.CompletionKey = pack_key(slot, gen);
  assoc.CompletionPort = rr.iocp;

  NTSTATUS st = ::NtSetInformationJobObject(
      job, JobObjectAssociateCompletionPortInformation,
      &assoc, sizeof(assoc));
  if (!NT_SUCCESS(st)) {
    g_lock.unlock();
    SlabPool::free(slot);
    return INVALID_TOKEN;
  }

  slot->callback = cb;
  slot->context = context;
  slot->target = job;
  slot->wcp = nullptr;
  slot->kind = SLOT_JOB;

  active_list_insert(slot);

  g_lock.unlock();

  return ReactorToken{slot, gen};
}

void unwatch(ReactorToken token) {
  if (!token.valid())
    return;

  ReactorState &rr = g_pcb.reactor;

  // During shutdown, the drain thread owns all slot cleanup. Touching
  // the slot here would race with drain_cleanup_slots().
  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return;

  ReactorSlot *slot = token.slot;

  g_lock.lock();

  if (slot->generation.load(cpp::MemoryOrder::RELAXED) != token.generation ||
      slot->kind == SLOT_FREE) {
    g_lock.unlock();
    return;
  }

  if (slot->kind == SLOT_WCP && slot->wcp) {
    ::NtCancelWaitCompletionPacket(slot->wcp, 1);
    ::NtClose(slot->wcp);
    slot->wcp = nullptr;
  }

  // Do NOT set kind = SLOT_FREE yet. dispatch_reactor_completion Step 7
  // checks (kind == SLOT_FREE) after setting dispatching=false to handle
  // detach()'s deferred free. If we set SLOT_FREE here while a dispatch
  // is in-flight, both Step 7 and our free below would fire — double free.
  // Instead, mark the slot as dead (null callback + bumped generation) so
  // no new dispatch can match, then set SLOT_FREE after the spin-wait.
  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->generation.fetch_add(1, cpp::MemoryOrder::RELEASE);

  active_list_remove(slot);

  g_lock.unlock();

  // Spin-wait for any in-flight dispatch to complete before freeing.
  // CAS exclusion guarantees at most one thread is dispatching per slot.
  while (slot->dispatching.load(cpp::MemoryOrder::ACQUIRE))
    spin_wait::relax_processor();

  slot->kind = SLOT_FREE;
  SlabPool::free(slot);
}

void detach(ReactorToken token) {
  if (!token.valid())
    return;

  ReactorState &rr = g_pcb.reactor;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return;

  ReactorSlot *slot = token.slot;

  g_lock.lock();

  if (slot->generation.load(cpp::MemoryOrder::RELAXED) != token.generation ||
      slot->kind == SLOT_FREE) {
    g_lock.unlock();
    return;
  }

  if (slot->kind == SLOT_WCP && slot->wcp) {
    ::NtCancelWaitCompletionPacket(slot->wcp, 1);
    ::NtClose(slot->wcp);
    slot->wcp = nullptr;
  }

  // Mark free but do NOT call SlabPool::free(). The dispatch loop still
  // holds a reference to this slot (the caller is inside the callback).
  // dispatch_reactor_completion() checks kind == SLOT_FREE after the
  // callback returns and completes the deferred free.
  slot->kind = SLOT_FREE;
  slot->callback = nullptr;
  slot->context = nullptr;
  slot->target = nullptr;
  slot->generation.fetch_add(1, cpp::MemoryOrder::RELEASE);

  active_list_remove(slot);

  g_lock.unlock();
  // No dispatching spin -- caller IS the dispatch context.
  // No SlabPool::free -- deferred to dispatch_reactor_completion step 7.
}

int rearm(ReactorToken token) {
  if (!token.valid())
    return -1;

  ReactorState &rr = g_pcb.reactor;

  if (rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return -1;

  ReactorSlot *slot = token.slot;

  g_lock.lock();

  if (slot->generation.load(cpp::MemoryOrder::RELAXED) != token.generation ||
      slot->kind != SLOT_WCP || !slot->wcp) {
    g_lock.unlock();
    return -1;
  }

  BOOLEAN already_signaled = 0;
  NTSTATUS st = ::NtAssociateWaitCompletionPacket(
      slot->wcp, rr.iocp, slot->target,
      pack_key(slot, token.generation),
      nullptr, STATUS_SUCCESS, 0, &already_signaled);

  g_lock.unlock();

  return NT_SUCCESS(st) ? 0 : -1;
}

// =========================================================================
// Public API -- Completion Routing
// =========================================================================

void set_completion_router(CompletionRouter router) {
  g_pcb.reactor.router.store(reinterpret_cast<uintptr_t>(router),
                             cpp::MemoryOrder::RELEASE);
}

// =========================================================================
// Public API -- Inline Dispatch
// =========================================================================

void dispatch_inline(PVOID key, NTSTATUS status, ULONG_PTR information) {
  dispatch_reactor_completion(key, status, information);
}

// =========================================================================
// Public API -- IOCP Access
// =========================================================================

HANDLE iocp_handle() { return g_pcb.reactor.iocp; }
HANDLE reserve_handle() { return g_pcb.reactor.reserve; }

bool drain_thread_healthy() {
  // Load the current heartbeat and compare against the previous snapshot.
  // Uses a thread-local to store the last-seen value so each caller gets
  // independent health tracking.
  static thread_local uint64_t last_seen = 0;
  uint64_t current = g_pcb.reactor.heartbeat.load(cpp::MemoryOrder::RELAXED);
  bool advanced = (current != last_seen);
  last_seen = current;
  return advanced;
}

uint64_t current_heartbeat() {
  return g_pcb.reactor.heartbeat.load(cpp::MemoryOrder::ACQUIRE);
}

void fence_drain_cycle() {
  ReactorState &rr = g_pcb.reactor;
  if (!rr.iocp || rr.shutdown.load(cpp::MemoryOrder::ACQUIRE))
    return;

  uint32_t n = rr.drain_thread_count;
  if (n == 0)
    return;

  // RCU-style fence: snapshot each drain thread's per-thread epoch,
  // then wait for every thread to advance past its snapshot. This
  // guarantees that every drain thread has completed at least one full
  // batch since the fence point — no in-flight completion from before
  // the fence can still be executing.
  //
  // Scales from 1 to MAX_DRAIN_THREADS with no behavioral change.
  // Per-thread epochs are cache-line aligned, so there's zero
  // contention on the hot path (each thread writes its own line).

  // Phase 0: Snapshot all per-thread epochs.
  uint64_t snapshots[ReactorState::MAX_DRAIN_THREADS];
  for (uint32_t i = 0; i < n; ++i)
    snapshots[i] = rr.drain_epochs[i].value.load(cpp::MemoryOrder::ACQUIRE);

  // Wake all drain threads so they cycle even if idle (blocked on
  // IOCP with infinite timeout). One sentinel per thread.
  for (uint32_t i = 0; i < n; ++i)
    wake_drain_thread(rr);

  // Wait for each thread to advance past its snapshot. For each
  // unconverged thread: hardware spin (UMWAIT/MWAITX on the epoch
  // cache line), then futex_addr park with a per-thread timeout
  // slice. No yield loops, no busy polling.
  //
  // Total timeout budget: 10ms spread across N threads. Each thread
  // gets 10ms/N. If all threads are healthy, each converges in
  // microseconds (the hardware spin catches it). The futex park is
  // only reached if a thread is mid-callback.
  LARGE_INTEGER per_thread_timeout;
  per_thread_timeout.QuadPart =
      -100000LL / static_cast<long long>(n); // 10ms / N, relative.

  for (uint32_t i = 0; i < n; ++i) {
    // Fast check: already advanced?
    if (rr.drain_epochs[i].value.load(cpp::MemoryOrder::ACQUIRE) !=
        snapshots[i])
      continue;

    // Hardware spin on the low 32 bits of the epoch. UMWAIT/MWAITX
    // monitors the cache line — wakes on write with near-zero power.
    uint32_t snap_lo = static_cast<uint32_t>(snapshots[i]);
    if (spin_wait::spin_on_raw_u32(
            reinterpret_cast<volatile uint32_t *>(
                &rr.drain_epochs[i].value.val),
            snap_lo))
      continue;

    // Futex park: sleep until the drain thread bumps the epoch.
    // futex_addr::wait_nt checks *addr != expected on entry, does
    // its own pre-spin, then parks in the kernel if still equal.
    // The drain thread calls futex_addr::wake after each epoch
    // bump, unparking us immediately. Timeout is a safety net.
    futex_addr::wait_nt(
        reinterpret_cast<volatile uint32_t *>(
            &rr.drain_epochs[i].value.val),
        snap_lo, &per_thread_timeout);
  }
}

// =========================================================================
// CRT section registration
// =========================================================================
//
// Init:  reactor_startup_init() — Phase 7, after fd_table, before signals.
// Fini:  reactor_startup_fini() — after signal fini.
// Fork:  reactor_fork_reinit() — after fd_table, before signal_fork_reinit().

} // namespace reactor
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::reactor_startup_init() {
  LIBC_NAMESPACE::internal::reactor::init();
  return 0;
}

void LIBC_NAMESPACE::internal::reactor_startup_fini() {
  LIBC_NAMESPACE::internal::reactor::fini();
}

void LIBC_NAMESPACE::internal::reactor_fork_reinit() {
  LIBC_NAMESPACE::internal::reactor::fork_reinit();
}
