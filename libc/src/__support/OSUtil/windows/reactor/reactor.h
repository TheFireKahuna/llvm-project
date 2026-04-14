//===-- Process-wide IOCP reactor --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Foundational event backbone for NT-POSIX. A single I/O Completion Port
// (IOCP) and a pool of drain threads that multiplex all asynchronous kernel
// events the runtime needs to react to:
//
//   - ALPC signal delivery (cross-process kill/sigqueue)
//   - Process handle waits (waitpid)
//   - Timer completions (alarm/setitimer)
//   - Epoll socket/FIFO/socketpair events
//   - Any waitable NT object
//
// Design principles:
//   - One IOCP, N drain threads -- the NT-idiomatic reactor pattern with
//     concurrent dispatch. IOCP naturally load-balances completions across
//     waiting threads. A slow callback on one thread doesn't block dispatch
//     on the others.
//   - Per-slot CAS exclusion: callbacks for the same registration are
//     serialized via compare_exchange on the dispatching flag. Callbacks
//     for different registrations execute concurrently.
//   - Two registration paths for reactor-internal events:
//       watch()      -- WaitCompletionPacket bridge for any waitable handle
//       watch_alpc() -- native ALPC completion port association
//   - Epoll shares the IOCP directly: AFD_NOTIFY and WCP registrations
//     point at reactor::iocp_handle(). Drain threads route non-reactor
//     completions to per-instance pending queues via an installed router.
//   - ABA-safe dispatch via generation-tagged completion keys. Stale
//     completions from unwatched slots are silently discarded.
//   - Guaranteed-delivery reserve object for manual posts.
//   - Strong unwatch contract: after unwatch() returns, no callback is
//     executing and none will fire. Safe to free the callback's context.
//   - Fork-safe: clean reinit protocol invalidates all registrations.
//
// Completion key discrimination:
//   Reactor keys set bit 48 (non-canonical in user-mode x86-64), making
//   them trivially distinguishable from real pointers (epoll registration
//   pointers, etc.) that share the same IOCP.
//
//   Key layout: [generation:15][tag:1][pointer:48]
//     - Bit 48: always set (reactor discriminant)
//     - Bits [63:49]: 15-bit truncated generation for ABA detection
//     - Bits [47:0]: ReactorSlot pointer (user-mode, bit 47 always 0)
//
// Routing model:
//   Drain pool threads are the primary IOCP consumers. For reactor-keyed
//   completions they dispatch via tagged-pointer slot lookup. For non-
//   reactor completions (epoll events) they invoke an installed router
//   callback which pushes them to per-epoll-instance pending queues.
//   epoll_wait also does a non-blocking IOCP flush before blocking on
//   its per-instance ready_event, ensuring freshly-arrived completions
//   are picked up immediately.
//
// This is the NT kernel's own event model (IOCP + WCP) promoted to a
// process-wide backbone, not an abstraction over it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_REACTOR_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_REACTOR_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace reactor {

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

// Callback invoked on a reactor drain thread when a completion fires.
//
//   context     -- opaque pointer provided at registration time
//   status      -- NTSTATUS from the completion (STATUS_SUCCESS for most events)
//   information -- completion-specific payload (0 for WCP/ALPC)
//
// Callbacks for different registrations may execute concurrently on
// separate drain threads. Callbacks for the same registration are
// serialized via CAS on the dispatching flag. Callbacks must be short
// and non-blocking -- pend work and return.
using ReactorCallback = void (*)(void *context, NTSTATUS status,
                                 ULONG_PTR information);

// Forward declaration -- defined in reactor.cpp. Slots are SlabPool-allocated.
struct ReactorSlot;

// Opaque token identifying a reactor registration.
// Holds a direct pointer to the SlabPool-allocated slot and the generation
// counter that was current at registration time. The generation is used to
// detect stale completions after unwatch/rewatch cycles.
struct ReactorToken {
  ReactorSlot *slot;
  uint32_t generation;

  bool valid() const { return slot != nullptr; }
};

inline constexpr ReactorToken INVALID_TOKEN = {nullptr, 0};

//===----------------------------------------------------------------------===//
// Lifecycle
//===----------------------------------------------------------------------===//

// Initialize the process-wide reactor: create IOCP, reserve object, start
// drain pool. Called by reactor_startup_init() (Phase 7, after fd_table,
// before signals). Returns 0 on success, -1 on failure.
int init();

// Shutdown: stop drain pool, cancel all watches, close IOCP.
// Called by reactor_startup_fini() (after signal fini).
void fini();

// Fork child reinit: close inherited handles, create new IOCP + drain
// pool. All prior registrations are invalidated -- subsystems must
// re-register in their own fork_reinit callbacks (which run later).
// Called by reactor_fork_reinit() (between fd_table and signal_fork_reinit()).
void fork_reinit();

//===----------------------------------------------------------------------===//
// Watch Registration
//===----------------------------------------------------------------------===//

// Watch a waitable NT handle via WaitCompletionPacket bridge.
//
// When the handle becomes signaled, the reactor invokes
// cb(context, status, information) on the drain thread. The watch is
// one-shot: it fires once, then becomes dormant. Call rearm() to watch
// again after consuming the signaled condition.
//
// Works with any NT object that supports NtWaitForSingleObject: events,
// timers (NtCreateTimer2), process handles, thread handles, mutants.
//
// Returns a valid token on success, INVALID_TOKEN on failure.
ReactorToken watch(HANDLE waitable, ReactorCallback cb, void *context);

// Watch an ALPC port via native completion port association.
//
// Uses NtAlpcSetInformation(AlpcAssociateCompletionPortInformation) to
// bind the port directly to the reactor's IOCP. The kernel posts a
// completion each time a message arrives -- persistent, no re-arming.
//
// The callback should drain ALL pending messages via non-blocking
// NtAlpcSendWaitReceivePort (timeout=0) in a loop. Multiple messages
// may queue between IOCP completions.
//
// Returns a valid token on success, INVALID_TOKEN on failure.
ReactorToken watch_alpc(HANDLE alpc_port, ReactorCallback cb, void *context);

// Watch a job object via IOCP completion port association.
//
// Uses NtSetInformationJobObject(JobObjectAssociateCompletionPortInformation)
// to bind the job directly to the reactor's IOCP. The kernel posts a
// completion for each job event (process exit, limit violation, etc.).
// Persistent -- no re-arming needed.
//
// The callback receives the message type in the `information` parameter
// (JOB_OBJECT_MSG_* constants from nt_job_types.h).
//
// Returns a valid token on success, INVALID_TOKEN on failure.
ReactorToken watch_job(HANDLE job, ReactorCallback cb, void *context);

// Deregister a watch. After return:
//   1. No future callbacks will fire for this token.
//   2. No callback is currently executing for this token.
// The caller may safely free resources referenced by the callback context.
//
// For WCP watches: cancels and closes the WaitCompletionPacket.
// For ALPC watches: the IOCP association persists until the port is closed.
//   The slot is freed, but stale completions are silently discarded via
//   generation mismatch.
void unwatch(ReactorToken token);

// Detach a watch from within its own callback. Performs the same cleanup
// as unwatch() (cancel WCP, bump generation, mark slot free) but does NOT
// spin-wait on the dispatching flag -- the caller IS the currently-
// dispatching callback, so no concurrent dispatch exists.
//
// Must only be called from the reactor drain thread, during dispatch of
// the given token's callback. The slot is not immediately returned to the
// pool; the dispatch loop completes the deferred free after clearing the
// dispatching flag.
void detach(ReactorToken token);

// Re-arm a dormant WCP watch. Call after the callback has consumed the
// signaled condition (e.g., read the timer, reaped the process exit code).
// If the handle is already signaled, the callback fires immediately.
//
// No-op for ALPC watches (persistent by nature).
// Returns 0 on success, -1 on failure (invalid token, wrong type).
int rearm(ReactorToken token);

//===----------------------------------------------------------------------===//
// Completion Key Discrimination
//===----------------------------------------------------------------------===//

// Bit 48 tag: set in all reactor completion keys. User-mode pointers on
// x86-64 have bits [63:48] = 0 (canonical form), so this bit is never
// set in a real pointer. Guaranteed zero false positives.
inline constexpr uintptr_t REACTOR_KEY_TAG = 1ULL << 48;

// Test whether a completion key belongs to the reactor.
// Subsystems draining the shared IOCP (e.g., epoll_wait flush) use this
// to discriminate reactor events from their own.
inline bool is_reactor_key(PVOID key) {
  return (reinterpret_cast<uintptr_t>(key) & REACTOR_KEY_TAG) != 0;
}

//===----------------------------------------------------------------------===//
// Completion Routing
//===----------------------------------------------------------------------===//

// Router callback for non-reactor completions. Installed by epoll (or
// any future subsystem that shares the reactor's IOCP). The drain thread
// invokes this for every completion whose key is NOT a reactor key.
//
// Parameters mirror FILE_IO_COMPLETION_INFORMATION fields:
//   key         -- CompletionKey (e.g., EpollRegistration pointer)
//   apc_context -- ApcContext
//   status      -- IoStatusBlock.Status
//   information -- IoStatusBlock.Information
using CompletionRouter = void (*)(PVOID key, PVOID apc_context,
                                  NTSTATUS status, ULONG_PTR information);

// Install the external completion router. At most one router at a time.
// Called by epoll during its subsystem init to claim non-reactor
// completions on the shared IOCP.
void set_completion_router(CompletionRouter router);

//===----------------------------------------------------------------------===//
// Inline Dispatch
//===----------------------------------------------------------------------===//

// Dispatch a reactor-keyed completion inline from the calling thread.
// Used by epoll_wait's non-blocking IOCP flush: when it dequeues a
// reactor completion, it dispatches it immediately rather than letting
// it wait for the drain pool. Same CAS-guarded dispatch logic as the
// drain threads.
void dispatch_inline(PVOID key, NTSTATUS status, ULONG_PTR information);

//===----------------------------------------------------------------------===//
// IOCP Access
//===----------------------------------------------------------------------===//

// Returns the reactor's IOCP handle. Available between init() and fini().
// The shared IOCP is used by both the reactor (for watch/watch_alpc
// registrations) and epoll (for AFD_NOTIFY and WCP registrations).
// Returns nullptr before init() or after fini().
HANDLE iocp_handle();

// Returns the reactor's reserve object handle. Subsystems that need
// guaranteed-delivery posts (e.g., epoll synthetic completions for
// regular files) can use this with NtSetIoCompletionEx.
// The reserve is reusable but NOT concurrently safe -- callers must
// serialize calls to NtSetIoCompletionEx with this handle.
HANDLE reserve_handle();

//===----------------------------------------------------------------------===//
// Drain Thread Health & Fencing
//===----------------------------------------------------------------------===//

// Check whether the drain pool is making progress. Returns true if any
// drain thread has completed at least one iteration since the last call
// to this function (or since init). Callers (e.g., signal delivery,
// waitpid) can use this to detect a fully stuck pool. The check is
// lock-free: a single atomic load + thread-local compare.
bool drain_thread_healthy();

// Return the current drain-thread heartbeat counter. Each drain loop
// iteration increments this. Used by subsystems that need to fence
// against in-flight completions (e.g., epoll close).
uint64_t current_heartbeat();

// RCU-style fence: wait for every drain thread to complete at least one
// full cycle after the current point. Snapshots each thread's per-thread
// epoch, posts N wakeups, then waits (hardware spin + futex park) for
// all epochs to advance.
//
// After this returns, any completion that was queued on the IOCP before
// the call has been dequeued and dispatched. The per-thread epoch
// guarantee is airtight — unlike global heartbeat, which only shows
// that *some* thread cycled, this proves *every* thread has cycled.
//
// Bounded: if any drain thread doesn't advance within ~10ms (e.g.,
// stuck in a callback), returns without guarantee. Callers should
// combine this with other safety mechanisms (tombstoning, activity
// counters).
void fence_drain_cycle();

} // namespace reactor
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_REACTOR_REACTOR_H
