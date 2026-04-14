//===-- Per-thread lifecycle state for Windows ---------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ThreadLifecycle: separately-allocated per-thread state that outlives the
// thread's stack. Owns join/detach state, thread identity, cancellation,
// kernel handles, and the robust mutex list.
//
// Motivation: the previous design embedded clear_tid, detach_state, and
// ThreadSignalState in the thread's own mmap'd stack. For detached threads
// this created a UAF window — the stack (and everything in it) was freed
// before the exit_word could be safely accessed. ThreadLifecycle lives in
// its own allocation, so joiners and the exit path both have stable memory.
//
// Allocated via a process-wide SlabPool (works before malloc, no futex dep).
// One TEB TLS slot points to the current thread's ThreadLifecycle as the
// root of all per-thread state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LIFECYCLE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LIFECYCLE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/futex_word.h"

struct __pthread_cleanup_t;

namespace LIBC_NAMESPACE_DECL {

struct ThreadAttributes;

// Forward declarations.
namespace signal_state {
struct ThreadSignalState;
struct SyscallFrame;
} // namespace signal_state

struct RobustRecord {
  RobustRecord *next;
  RobustRecord **prev_next;
  Futex *futex_word;
};

// ---------------------------------------------------------------------------
// ThreadLifecycle — per-thread, separately allocated
// ---------------------------------------------------------------------------

struct ThreadLifecycle {
  // --- Join/detach (stable memory, survives stack deallocation) ---

  Futex exit_word{0};               // Joiners wait here. 0 = exited.
  cpp::Atomic<uint32_t> detach_state; // DetachState enum values.

  // --- Thread identity ---

  uint32_t task_id;                 // Monotonic, never reused in-process.
                                    // Fits in ROBUST_TID_MASK (30 bits).
  int tid;                          // NT thread ID (externally visible).

  // --- Kernel handle ---
  // Single owned thread handle used for both join/wait and registry-mediated
  // cross-thread operations. Cross-thread users borrow it through the thread
  // registry helpers; the owner releases the final reference during
  // join/detach cleanup.

  cpp::Atomic<HANDLE> thread_handle;

  // --- Cancellation state ---

  // Packed cancellation word — single atomic for the cancel_check hot path.
  // Bit 0: state   (0 = ENABLE, 1 = DISABLE)
  // Bit 1: type    (0 = DEFERRED, 1 = ASYNCHRONOUS)
  // Bit 2: pending (0 = none, 1 = cancellation requested)
  cpp::Atomic<uint8_t> cancel_word;

  // Active blocking syscall frame. Points to a stack-allocated SyscallFrame
  // during alertable waits. Enables pthread_cancel and fork to cancel
  // in-flight I/O. Atomic: cross-thread readers always suspend first, but
  // Atomic prevents compiler reordering.
  cpp::Atomic<signal_state::SyscallFrame *> active_syscall;

  // pthread_cleanup_push chain. Thread-local only (owning thread reads/writes).
  ::__pthread_cleanup_t *cancel_cleanup_stack;

  // --- Stack metadata (for deallocation and fork TEB fixup) ---

  void *stack_base;                 // pthread-visible stack base (past guard).
  size_t stack_size;
  size_t guard_size;
  unsigned char owned_stack;        // True if we allocated the stack.
  void *attrib_storage;             // Stable startup/ThreadAttributes storage.
  struct ThreadAttributes *attrib;  // Stable pthread_t target for this thread.

  // --- Return value ---

  // Stored here so it survives stack deallocation for detached threads
  // that transition to EXITING before the joiner reads it.
  // (ThreadAttributes::retval is still set for compatibility, but the
  //  authoritative copy for join() lives here.)

  // We forward-declare ThreadReturnValue's shape to avoid pulling thread.h.
  // The actual ThreadReturnValue is a union { void*; int; } — 8 bytes.
  union {
    void *posix_retval;
    int stdc_retval;
  } retval;

  // --- Robust mutex sidecar state ---
  // Robust mutex bookkeeping lives outside the mutex payload so the public
  // pthread_mutex_t storage can stay compact and opaque. Each held robust
  // mutex owns one RobustRecord from the process-wide robust_pool.
  RobustRecord *robust_list;

  // --- Registry link (for process-wide thread registry) ---

  cpp::Atomic<DWORD> owner_tid;    // NT TID; 0 = dead/deregistered.
  bool dynamically_allocated;      // Slab-allocated lifecycle (not PCB-resident).
  bool pool_allocated;             // True for foreign-thread lifecycle objects.
  // --- Registry slot reference ---
  // Stored during registration so pin/unpin and deregister are O(1).
  // slot_page == UINT32_MAX means "not registered" — doubles as the
  // registration sentinel (replaces the old `registered_in_registry` bool).
  // Atomic because cross-thread readers may check registration state.
  //
  // IMPORTANT: slot_page == 0 is a valid page index. After memset(0),
  // zero_lifecycle() MUST be called to restore the UINT32_MAX sentinel.
  // All allocation paths (alloc_lifecycle, zero_lifecycle) enforce this.
  // The in-class initializer is for documentation; memset dominates.
  cpp::Atomic<uint32_t> slot_page{UINT32_MAX};
  uint32_t slot_index = UINT32_MAX;
  uint32_t slot_generation = 0;

  // --- Subsystem pointers (single TEB slot fans out here) ---

  signal_state::ThreadSignalState *signal; // On-stack or pool-allocated.
  bool signal_owned;                       // True if signal state is pool-allocated.
  void *thread_ring;                       // ThreadRing*, lazy (null until I/O).
  void *atexit_data;                       // Reserved (previously gated __cxa_thread_finalize).

  // --- Deferred reclamation (used only after deregistration) ---
  // After deregister, subsystem pointers above are null and these fields
  // are safe to reuse. The lifecycle sits on the retired list until the
  // reclaimer determines no reader can still reference it.
  ThreadLifecycle *retired_next;            // Intrusive retired list link.
  uint64_t retired_epoch;                   // Epoch at time of retirement.
};

// Trivially constructible: all fields zero-initialized is a valid empty state.
static_assert(__is_trivially_constructible(cpp::Atomic<uint32_t>),
              "Atomic<uint32_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<uint8_t>),
              "Atomic<uint8_t> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<DWORD>),
              "Atomic<DWORD> must be trivially constructible");
static_assert(__is_trivially_constructible(cpp::Atomic<HANDLE>),
              "Atomic<HANDLE> must be trivially constructible");

// ---------------------------------------------------------------------------
// Process-wide monotonic task_id counter
// ---------------------------------------------------------------------------

// Starts at 1; 0 is reserved for "no owner" in the robust mutex encoding.
// 30-bit range (~1 billion) is sufficient for any single process lifetime.
inline cpp::Atomic<uint32_t> g_next_task_id{1};

inline uint32_t allocate_task_id() {
  return g_next_task_id.fetch_add(1, cpp::MemoryOrder::RELAXED);
}

// ---------------------------------------------------------------------------
// Lifecycle helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Lifecycle TLS root — one TEB slot for all per-thread state
// ---------------------------------------------------------------------------

// Initialize the lifecycle TLS slot. Called from lifecycle_startup_init() (Phase 4).
void lifecycle_init();

// Get/set the current thread's lifecycle root.
ThreadLifecycle *get_current_lifecycle();
void set_current_lifecycle(ThreadLifecycle *lc);

// TLS index accessor (for subsystems that need direct TEB access).
DWORD get_lifecycle_tls_index();

// ---------------------------------------------------------------------------
// Lifecycle helpers
// ---------------------------------------------------------------------------

// Allocate a ThreadLifecycle from the process-wide SlabPool.
// Returns nullptr on OOM. The returned memory is zero-initialized.
ThreadLifecycle *alloc_lifecycle();

// Free a ThreadLifecycle back to the SlabPool.
// No-op if lc is nullptr. Caller must have already cleaned up
// robust records and subsystem pointers.
void free_lifecycle(ThreadLifecycle *lc);

// Zero-initialize a ThreadLifecycle. Null state is a valid empty lifecycle.
// MUST be called on every freshly-allocated lifecycle — memset alone leaves
// slot_page == 0, which is a valid page index and would cause a bogus
// deregister if the lifecycle were freed without prior registration.
inline void zero_lifecycle(ThreadLifecycle *lc) {
  __builtin_memset(lc, 0, sizeof(ThreadLifecycle));
  // Restore the "not registered" sentinel — memset zeroed the atomic.
  lc->slot_page.store(UINT32_MAX, cpp::MemoryOrder::RELAXED);
  lc->slot_index = UINT32_MAX;
}

// Sentinel value stored in exit_word before thread starts running.
inline constexpr uint32_t EXIT_WORD_LIVE = 0xABCD1234;

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_LIFECYCLE_H
