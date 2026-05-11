//===--- Implementation of a Windows thread class ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/thread.h"
#include "config/app.h"
#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/CPP/stringstream.h"
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain_registry.h"
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/tls/teb_fixup.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/cancel_support.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/robust_list_cleanup.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#include "src/__support/time/abs_timeout.h"

#include "hdr/time_macros.h"

// Signal state for mask inheritance across pthread_create. On Linux the kernel
// copies the parent's signal mask into the child during clone(), but Windows
// CreateThread has no signal concept — we must do it in userspace. This
// couples threading to the signal subsystem, which is unavoidable on Windows.
#include "src/__support/OSUtil/windows/signal/signal.h"

// NUMA policy inheritance. Linux clone() inherits the parent's mempolicy;
// Windows thread_local reinitializes to MPOL_DEFAULT. We snapshot the
// parent's policy in StartArgs and apply it in thread_entry().
#include "src/__support/OSUtil/windows/memory/legacy/numa_policy.h"

// Process identity — impersonation token inheritance for POSIX setuid.
// Linux clone() inherits the parent's credentials; Windows CreateThread
// inherits the process token, not any thread's impersonation token.
#include "src/__support/OSUtil/windows/process/process_identity.h"
#include "src/__support/OSUtil/windows/resource/rlimit_data_guard.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

// __cxa_finalize runs process-level atexit handlers. Forward-declared here
// for the last-thread exit path (matching src/stdlib/exit.cpp).
extern "C" void __cxa_finalize(void *);

namespace LIBC_NAMESPACE_DECL {

static constexpr size_t NAME_SIZE_MAX = 256;

// POSIX §2.4.3: "An implicit call to exit() is made when [...] the last
// thread in the process calls pthread_exit(), or when [...] all threads
// have terminated."  The exit status is 0.
//
// After thread-local destructors and TSS dtors have run, check if this is
// the last live thread. If so, run process atexit handlers and terminate
// the process — matching the behavior of glibc and musl.
static void exit_process_if_last_thread() {
  if (registry_live_count() <= 1) {
    __cxa_finalize(nullptr);
    NtTerminateProcess(reinterpret_cast<HANDLE>(-1), 0);
    __builtin_unreachable();
  }
}

// Thread start arguments — stored in a small stable sidecar allocation.
// Transient: only used during thread bootstrap.
struct alignas(STACK_ALIGNMENT) StartArgs {
  ThreadAttributes *thread_attrib;
  ThreadRunner runner;
  void *arg;
  ThreadLifecycle *lifecycle;                 // Separately-allocated root.
  signal_state::ThreadSignalState *sig_state; // Stable per-thread signal state.
  sigset_t inherited_sigmask; // Parent's signal mask, per POSIX inheritance
  int8_t inherited_sched_policy; // Parent's POSIX scheduling policy
  windows::NumaPolicy inherited_numa_policy;  // Parent's NUMA mempolicy
  HANDLE inherited_imp_token; // Parent's impersonation token (setuid)
};

// ThreadAttributes and signal_state::ThreadSignalState now live INLINE
// in ThreadLifecycle (`&lc->attrib`, `&lc->sig_state`). The only piece
// that still needs separate transient storage is `StartArgs`, which is
// consumed once by the child during bringup and then released.
namespace {

constexpr size_t kStartArgsSlotAlign = alignof(StartArgs);
constexpr size_t kStartArgsSlotSize =
    (sizeof(StartArgs) + kStartArgsSlotAlign - 1) &
    ~(kStartArgsSlotAlign - 1);

static_assert(kStartArgsSlotSize >= sizeof(void *),
              "StartArgs slab slot must be pointer-sized");

internal::SlabPool start_args_pool;

LIBC_INLINE void init_start_args_pool() {
  start_args_pool.init(kStartArgsSlotSize, kStartArgsSlotAlign);
  // Phase Allocator: start_args slabs back transient pthread_create
  // bootstrap state. Their TLS abandon callback must outlive lifecycle
  // teardown (which retires nodes through the slab) — Allocator < Lifecycle.
  start_args_pool.init_tls(internal::kTlsCleanupPhaseAllocator);
}

} // namespace

LIBC_INLINE bool round_up_to_page(size_t value, size_t &rounded) {
  if (value > SIZE_MAX - (EXEC_PAGESIZE - 1))
    return false;
  rounded = (value + EXEC_PAGESIZE - 1) & ~(size_t(EXEC_PAGESIZE) - 1);
  return true;
}

LIBC_INLINE ErrorOr<StartArgs *> alloc_start_args() {
  init_start_args_pool();
  void *storage = start_args_pool.tls_alloc();
  if (!storage)
    return Error{ENOMEM};
  return static_cast<StartArgs *>(storage);
}

LIBC_INLINE void free_start_args(StartArgs *args) {
  internal::SlabPool::free(args);
}

// Windows owns the real kernel stack reservation. We keep pthread-visible
// stack metadata in ThreadAttributes/ThreadLifecycle and derive the low
// address from the thread's TEB once the kernel stack exists.
LIBC_INLINE void populate_kernel_stack_metadata(ThreadAttributes *attrib,
                                                ThreadLifecycle *lc,
                                                const TEB *teb) {
  if (!attrib || !lc || !teb || attrib->stack)
    return;

  uintptr_t stack_top = reinterpret_cast<uintptr_t>(teb->NtTib.StackBase);
  if (stack_top < attrib->stacksize)
    return;

  void *stack_base = reinterpret_cast<void *>(stack_top - attrib->stacksize);
  attrib->stack = stack_base;
  lc->stack_base = stack_base;
  lc->stack_size = attrib->stacksize;
  lc->guard_size = attrib->guardsize;
}

// This must always be inlined as we may be freeing the calling thread's stack.
//
// Deliberately a no-op for HANDLE close (C4): the thread handle is now
// owned by the lifecycle and closed by free_thread_registry_node's
// Lifecycle FreeFn, after Crystalline confirms no reservation pins lc.
// The previous exchange-and-close path could close the handle while a
// cross-thread borrower (cancel APC, suspend/resume, signal delivery)
// still held a reference — kernel handle reuse on the closed value
// could route the operation to an unrelated object. Closing in the
// FreeFn is the structural fix.
//
// cleanup_tls was a documented no-op on Windows (PE/COFF TLS is loader-
// managed via .CRT$XLC); removed (M6).
[[gnu::always_inline]] LIBC_INLINE void
cleanup_thread_resources(ThreadAttributes *attrib, ThreadLifecycle *lc) {
  (void)attrib;
  (void)lc;
}

// Free the ThreadLifecycle allocation. Called by the joiner (joinable)
// or by the deferred path after the thread has fully exited (detached).
// Crystalline's retire batches the lifecycle for reclaim once no
// reservation pins it; the slab slot returns when FreeFn fires AND the
// thread handle is closed at the same time.
LIBC_INLINE void free_thread_lifecycle(ThreadLifecycle *lc) {
  if (!lc)
    return;
  registry_deregister_and_retire(lc);
}

// NtCreateThreadEx start routine. Faults propagate to the master VEH;
// no frame-level SEH backstop — VEH filters (mem_fault_filter et al.)
// are the single dispatch path.
WINAPI static DWORD thread_entry_impl(void *arg) {
  auto *start_args = static_cast<StartArgs *>(arg);
  auto *attrib = start_args->thread_attrib;
  auto *lc = start_args->lifecycle;

  self.attrib = attrib;
  self.attrib->atexit_callback_mgr = internal::get_thread_atexit_callback_mgr();

  // Set the lifecycle as the TEB root for this thread.
  set_current_lifecycle(lc);

  // The kernel owns the real stack reservation. Fill in the pthread-visible
  // low stack address once the child thread is live.
  populate_kernel_stack_metadata(attrib, lc, NtCurrentTeb());

  // Reserve LIBC_STACK_GUARANTEE_BYTES for the SEH dispatcher + master VEH
  // + signal transport on stack-overflow. Without this the user's SIGSEGV
  // handler (or SA_ONSTACK alt-stack switch) cannot reliably run after a
  // guard-page hit. Pure TEB store, idempotent across re-entry. Done
  // BEFORE register_thread_state so the guarantee is in place before any
  // sigaction / signal delivery in the new thread.
  windows::apply_libc_stack_guarantee();

  // Pre-claim Crystalline-W slot indices on every registered domain so
  // a future fault-context `protect()` (e.g. a VEH filter that calls
  // `va_tracker::resolve()`) is allocation-free. Without warmup, the
  // first protect() on a fault path could demand-commit slot-pool VA,
  // which itself could re-enter the master VEH handler. Idempotent
  // and cheap — domains not yet registered at this point are simply
  // skipped (the calling thread will lazy-claim on first use).
  ::LIBC_NAMESPACE::concurrent::registry_warm_thread_all();

  // Register the stable per-thread signal state with TLS and inherit the
  // parent's signal mask.
  signal_state::register_thread_state(start_args->sig_state);
  start_args->sig_state->blocked_signals = start_args->inherited_sigmask;
  start_args->sig_state->sched_policy = start_args->inherited_sched_policy;

  // lc->signal was pre-wired by the parent in Thread::run to close the
  // cross-thread delivery race; no re-assignment needed here.

  // Inherit parent's NUMA mempolicy. Overwrites the thread_local default
  // (MPOL_DEFAULT) before any mmap call in the child, matching Linux
  // clone() behavior where child tasks inherit the parent's mempolicy.
  windows::get_thread_numa_policy() = start_args->inherited_numa_policy;

  // Inherit parent's impersonation token for POSIX identity semantics.
  // On Linux clone() copies credentials; Windows CreateThread inherits the
  // process token, so we must apply impersonation explicitly.
  if (start_args->inherited_imp_token)
    windows_identity::apply_impersonation(NtCurrentThread(),
                                          start_args->inherited_imp_token);

  ThreadReturnValue retval;
  if (attrib->style == ThreadStyle::POSIX) {
    retval.posix_retval = start_args->runner.posix_runner(start_args->arg);
  } else {
    retval.stdc_retval = start_args->runner.stdc_runner(start_args->arg);
  }

  // start_args has been fully consumed (runner is the last field
  // touched). Release the transient slab slot back to the pool now,
  // before the rest of the exit sequence — keeps the slab footprint
  // proportional to live thread-create-in-flight count rather than
  // total-threads-ever.
  free_start_args(start_args);
  start_args = nullptr;

  // Store return value in both locations — lifecycle is authoritative for
  // join, attrib is kept for compatibility.
  attrib->retval = retval;
  if (attrib->style == ThreadStyle::POSIX)
    lc->retval.posix_retval = retval.posix_retval;
  else
    lc->retval.stdc_retval = retval.stdc_retval;

  // Disable cancellation before atexit callbacks. An atexit destructor
  // could call a cancellation point (write, close, etc.) — if cancel
  // is pending, that would re-enter the exit path. Matches glibc.
  lc->cancel_state.fetch_or(signal_state::CANCEL_STATE_BIT,
                           cpp::MemoryOrder::RELAXED);

  // Call the thread's atexit callbacks
  internal::call_atexit_callbacks(attrib);

  // If this is the last thread, act as exit(0) per POSIX.
  exit_process_if_last_thread();

  // Eagerly publish OWNER_DIED for robust mutexes before the final thread
  // termination handoff. Relying on the loader/TLS detach callback makes this
  // transition scheduler-sensitive, which can let timed robust waiters observe
  // ETIMEDOUT instead of EOWNERDEAD when a thread simply returns.
  robust_mutex::robust_list_cleanup(lc);

  // Atomic exit-state transition. JOINABLE → EXITING means joiner will
  // claim cleanup. CAS failure means state is DETACHED (we own cleanup
  // ourselves) or JOINING (a joiner already claimed; we just notify).
  // The CAS outcome itself isn't branched on here — retire ownership is
  // resolved later via the resulting detach_state — so we discard the
  // bool result. The CAS's in/out `expected` parameter is also unread
  // afterwards (lifecycle_cleanup re-loads detach_state when it runs):
  //   - CAS succeeded → state is now EXITING; joiner / late-detacher
  //     will CAS EXITING → JOINING and run retire.
  //   - CAS failed with observed DETACHED → nobody will join;
  //     lifecycle_cleanup runs the retire.
  //   - CAS failed with observed JOINING → a joiner already claimed;
  //     they run the retire.
  // In every case the lifecycle's HANDLE is closed by the FreeFn (C4)
  // — not by this thread. cleanup_thread_resources is a no-op now (M6).
  uint32_t expected = uint32_t(DetachState::JOINABLE);
  (void)lc->detach_state.compare_exchange_strong(
      expected, uint32_t(DetachState::EXITING),
      cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE);
  cleanup_thread_resources(attrib, lc);

  // RELEASE-store with notify_all on exit_word so any cooperative
  // observer (test harness, fallback wait path) sees completion.
  // Synchronization is also implicit through the kernel-handle wait
  // returned by NtWaitForSingleObject.
  lc->exit_word.store_and_notify_all(0);

  // Run libc TLS cleanup explicitly OUTSIDE the loader lock — mirrors the
  // thread_exit path below. Releases the per-thread scratch arena (via
  // MEM_RESET — the VA is retained to avoid a lock-free Crystalline
  // registry-walk race against peers, but physical pages are reclaimed),
  // returns the wait_slot to its pool, deregisters signal state,
  // abandons FLS-owned slabs, and runs __cxa_thread_finalize. Without
  // this, naturally-returning workers leak ~tens of KB of per-thread
  // state because NtTerminateThread skips loader notification and the
  // .CRT$XLC backup never fires.
  internal::tls_cleanup_run_all();
  NtTerminateThread(NtCurrentThread(), 0);
  __builtin_unreachable();
}

int Thread::run(ThreadStyle style, ThreadRunner runner, void *arg, void *stack,
                size_t stacksize, size_t guardsize, bool detached) {
  constexpr size_t WINDOWS_DEFAULT_STACKSIZE = 1 << 21; // 2 MB
  if (stacksize == 0)
    stacksize = WINDOWS_DEFAULT_STACKSIZE;

  if (!round_up_to_page(stacksize, stacksize) ||
      !round_up_to_page(guardsize, guardsize))
    return EINVAL;

  size_t stack_reserve_size = 0;
  if (__builtin_add_overflow(stacksize, guardsize, &stack_reserve_size))
    return EINVAL;
  if (!windows::allows_public_rlimit_data_growth(stack_reserve_size))
    return ENOMEM;

  // Validate stack alignment
  if (stack) {
    uintptr_t stackaddr = reinterpret_cast<uintptr_t>(stack);
    if ((stackaddr % STACK_ALIGNMENT != 0) ||
        ((stackaddr + stacksize) % STACK_ALIGNMENT != 0))
      return EINVAL;
  }

  // Allocate ThreadLifecycle separately from the stack. This is the key
  // correctness change: exit_word, detach_state, and retval now live in
  // stable memory that survives stack deallocation.
  auto *lc = alloc_lifecycle();
  if (!lc)
    return ENOMEM;

  // ThreadAttributes and ThreadSignalState live INLINE in ThreadLifecycle.
  // The only transient allocation needed is `StartArgs`, consumed once
  // by the child during bringup and freed by free_start_args after.
  auto start_args_alloc = alloc_start_args();
  if (!start_args_alloc) {
    // lc was freshly allocated (no registration yet) — return it
    // directly to the slab pool without going through Crystalline.
    free_lifecycle(lc);
    return ENOMEM;
  }
  auto *start_args = start_args_alloc.value();
  attrib = &lc->attrib;
  auto *sig_state = &lc->sig_state;

  lc->task_id = allocate_task_id();
  lc->exit_word.store(EXIT_WORD_LIVE);
  lc->detach_state.store(
      uint32_t(detached ? DetachState::DETACHED : DetachState::JOINABLE),
      cpp::MemoryOrder::RELAXED);
  lc->thread_handle.store(nullptr, cpp::MemoryOrder::RELAXED);

  lc->stack_base = stack;
  lc->stack_size = stacksize;
  lc->guard_size = guardsize;
  lc->owned_stack = false;

  // H2 fix: pre-allocate the registry bucket entry BEFORE NtCreateThreadEx.
  // The post-create registry_register's most likely failure mode is slab
  // exhaustion in alloc_bucket_entry; doing it here keeps OOM recoverable
  // (clean ENOMEM return; no thread launched, no cleanup needed). On
  // success registry_register_with_entry consumes the pre-allocated entry.
  BucketEntry *pre_entry = registry_alloc_bucket_entry_for(lc);
  if (!pre_entry) {
    free_start_args(start_args);
    free_lifecycle(lc);
    return ENOMEM;
  }

  TLSDescriptor tls;
  init_tls(tls);

  attrib->style = style;
  attrib->detach_state =
      uint32_t(detached ? DetachState::DETACHED : DetachState::JOINABLE);
  attrib->stack = stack;
  attrib->stacksize = stacksize;
  attrib->guardsize = guardsize;
  attrib->owned_stack = false;
  attrib->tls = tls.tls_index;
  attrib->tls_size = tls.size;

  attrib->platform_data = lc;

  start_args->thread_attrib = attrib;
  start_args->runner = runner;
  start_args->arg = arg;
  start_args->lifecycle = lc;
  start_args->sig_state = sig_state;

  // Publish signal state from the parent so cross-thread pthread_kill /
  // pthread_cancel can deliver before the child completes its own startup.
  // Mirrors the registry_register pre-publication below. sig_state lives
  // in zero-initialized slab storage, so readers observing it before the
  // child overwrites blocked_signals/sched_policy see a defined empty
  // state; signals landing in the gap are pended (thread or process-wide
  // via the APC fallback) and delivered once the child dispatches APCs.
  // RELEASE so cross-thread senders that ACQUIRE-load `signal` observe
  // both the pointer and the zero-initialized signal state behind it.
  lc->signal.store(sig_state, cpp::MemoryOrder::RELEASE);

  // Snapshot parent's signal mask and scheduling policy for POSIX inheritance.
  auto *parent_sig = signal_state::get_thread_state();
  start_args->inherited_sigmask =
      parent_sig ? parent_sig->blocked_signals : sigset_t{};
  start_args->inherited_sched_policy =
      parent_sig ? parent_sig->sched_policy.load(cpp::MemoryOrder::ACQUIRE) : 0;

  // Snapshot parent's NUMA mempolicy for inheritance.
  start_args->inherited_numa_policy = windows::get_thread_numa_policy();

  // Snapshot parent's impersonation token for POSIX identity inheritance.
  // On Linux the kernel copies credentials via clone(); on Windows we must
  // explicitly apply the impersonation token to the new thread.
  start_args->inherited_imp_token =
      g_pcb.identity.impersonation_token.load(cpp::MemoryOrder::ACQUIRE);

  CLIENT_ID child_client_id = {};
  PVOID child_teb = nullptr;
  struct {
    SIZE_T TotalLength;
    PS_ATTRIBUTE Attributes[2];
  } attr_list = {};
  attr_list.Attributes[0].Attribute = PS_ATTRIBUTE_CLIENT_ID;
  attr_list.Attributes[0].Size = sizeof(child_client_id);
  attr_list.Attributes[0].ValuePtr = &child_client_id;
  attr_list.Attributes[0].ReturnLength = nullptr;
  attr_list.Attributes[1].Attribute = PS_ATTRIBUTE_TEB_ADDRESS;
  attr_list.Attributes[1].Size = sizeof(child_teb);
  attr_list.Attributes[1].ValuePtr = &child_teb;
  attr_list.Attributes[1].ReturnLength = nullptr;
  attr_list.TotalLength = sizeof(SIZE_T) + 2 * sizeof(PS_ATTRIBUTE);

  // Create the thread SUSPENDED — atomic create+suspend in one
  // syscall. The child cannot reach thread_entry_impl until we
  // NtResumeThread it below, which closes the H2 rollback race
  // structurally: a registration failure terminates a thread that has
  // run zero user code, so there is no possibility of the child
  // having self-registered, allocated TLS, or written to lc.
  //
  // Faults in the thread propagate to the master VEH; no frame-level
  // SEH.
  HANDLE handle;
  NTSTATUS status = NtCreateThreadEx(
      &handle,                                    // ThreadHandle
      THREAD_QUERY_LIMITED_INFORMATION |          // Access required for thread naming
          THREAD_TERMINATE |                      // Access required for thread_exit
          THREAD_SET_CONTEXT |                    // Signal delivery (APC)
          THREAD_GET_CONTEXT |                    // Registry signal/context users
          THREAD_SET_INFORMATION |                // NtCreateThreadStateChange (SIGSTOP)
          THREAD_SUSPEND_RESUME |                 // Required for the post-create
                                                  // NtResumeThread that lifts
                                                  // CREATE_SUSPENDED, and for
                                                  // SIGSTOP/SIGCONT cooperative
                                                  // suspend if used elsewhere
          SYNCHRONIZE,                            // Join/wait operations
      nullptr,                                    // ObjectAttributes
      NtCurrentProcess(),                         // ProcessHandle
      reinterpret_cast<PVOID>(thread_entry_impl), // StartRoutine
      start_args,                                 // Argument
      THREAD_CREATE_FLAGS_CREATE_SUSPENDED,       // start suspended
      0,                                          // ZeroBits
      0,                                          // StackSize (use default)
      stack_reserve_size,                         // MaximumStackSize (reserve)
      reinterpret_cast<PS_ATTRIBUTE_LIST *>(&attr_list)); // AttributeList

  if (!NT_SUCCESS(status)) {
    free_start_args(start_args);
    // Pre-allocated bucket entry was never published — return it
    // directly to the slab.
    registry_release_unused_entry(pre_entry);
    // lc had its task_id allocated but was never published to the
    // registry — free directly to the slab without Crystalline.
    free_lifecycle(lc);
    return windows_util::ntstatus_to_errno(status);
  }

  DWORD child_tid = static_cast<DWORD>(
      reinterpret_cast<ULONG_PTR>(child_client_id.UniqueThread));
  lc->tid = static_cast<int>(child_tid);
  attrib->tid = lc->tid;
  populate_kernel_stack_metadata(attrib, lc, static_cast<TEB *>(child_teb));

  // Register the lifecycle while the child is still suspended. The C1
  // sentinel CAS-claim is uncontended here (the child cannot run yet)
  // but the same code path keeps idempotency for any later child-side
  // self-register. registry_register_with_entry adopts `handle`
  // (duplicate_handle=false); on failure it leaves lc->thread_handle
  // null and returns ownership of `handle` to us.
  if (!registry_register_with_entry(lc, pre_entry, handle, child_tid,
                                     /*duplicate_handle=*/false)) {
    // Structural rollback: the child has run NO user-mode code (still
    // suspended), so termination is race-free — there is no chance of
    // the child observing lc. Terminate + close + free; no Crystalline
    // retire needed because lc was never iter-published.
    ::NtTerminateThread(handle, 0);
    ::NtClose(handle);
    free_start_args(start_args);
    free_lifecycle(lc);
    return EAGAIN;
  }

  // Registration published; release the child to run thread_entry_impl.
  status = ::NtResumeThread(handle, nullptr);
  if (!NT_SUCCESS(status)) {
    // Resume failed — terminate the suspended thread, deregister our
    // already-registered lifecycle, and report. lc IS in the registry
    // at this point so we go through Crystalline retire.
    ::NtTerminateThread(handle, 0);
    registry_deregister_and_retire(lc);
    free_start_args(start_args);
    return windows_util::ntstatus_to_errno(status);
  }

  return 0;
}

// Atomically claim cleanup ownership of `lc` for joining. Transitions
// JOINABLE → JOINING (thread still running) or EXITING → JOINING
// (thread already exited). Returns 0 on success, EINVAL on a state
// that disallows join (DETACHED / already-JOINING / corrupt).
//
// C2 fix: only the CAS winner runs the retire path, so concurrent
// pthread_join calls cannot double-retire lc and corrupt the
// Crystalline batch chain.
LIBC_INLINE int claim_join_ownership(ThreadLifecycle *lc) {
  uint32_t expected = uint32_t(DetachState::JOINABLE);
  for (;;) {
    if (lc->detach_state.compare_exchange_weak(
            expected, uint32_t(DetachState::JOINING),
            cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE)) {
      return 0;
    }
    // CAS failed. expected now holds the observed state.
    if (expected == uint32_t(DetachState::JOINABLE))
      continue; // weak CAS spurious failure
    if (expected == uint32_t(DetachState::EXITING)) {
      // Thread already exited; try EXITING → JOINING.
      uint32_t exiting = uint32_t(DetachState::EXITING);
      if (lc->detach_state.compare_exchange_strong(
              exiting, uint32_t(DetachState::JOINING),
              cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE)) {
        return 0;
      }
      // Lost to another joiner / late detacher; check what they
      // produced. expected was overwritten by compare_exchange_strong.
      expected = exiting;
      // Loop and re-check; if state is now JOINING, we lose.
      continue;
    }
    // DETACHED, JOINING, or unrecognized: refuse.
    return EINVAL;
  }
}

// Wait for thread termination, optionally bounded by an absolute deadline.
//
// `timeout == nullopt`        — block indefinitely (pthread_join).
// `timeout->is_zero()`        — single-shot poll (pthread_tryjoin_np).
// otherwise                   — wait until the absolute deadline elapses
//                                (pthread_timedjoin_np / clockjoin_np).
//
// Returns:
//   0           — thread terminated; caller may drain retval/lifecycle.
//   ETIMEDOUT   — deadline elapsed (or zero-poll found thread alive).
//   EINVAL      — handle-less futex path observed an unexpected error.
//
// Alertable kernel-handle wait keeps APC delivery (cancel APCs, stop
// signals) live across the call. On STATUS_USER_APC / STATUS_ALERTED we
// invoke cancel::check — pending+enabled cancel forces an unwind and
// never returns; deferred / no cancel returns and we re-enter the wait
// against the same absolute deadline (so APC storms cannot extend it).
//
// The kernel handle is signalled by thread termination regardless of
// whether user-mode TLS callbacks fired (covers the external-
// NtTerminateThread case). C4 makes the handle's lifetime co-extensive
// with `lc`, so a joiner holding `lc` always sees a non-null handle if
// the thread was started through Thread::run.
[[gnu::no_sanitize_address]]
LIBC_INLINE static int
wait_until(ThreadLifecycle *lc,
           cpp::optional<internal::AbsTimeout> timeout) {
  HANDLE th = lc->thread_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (th) {
    for (;;) {
      LARGE_INTEGER nt_storage;
      LARGE_INTEGER *nt_to = nullptr;
      if (timeout) {
        // Recompute on every loop iteration. Realtime resolves to an
        // absolute FILETIME (cached the first time through, but the
        // cost is two arithmetic ops); monotonic deadlines must be
        // re-derived against the current QueryUnbiasedInterruptTime
        // because relative LARGE_INTEGER ticks down by itself.
        constexpr long long EPOCH_DIFF_HNS = 116444736000000000LL;
        const timespec &ts = timeout->get_timespec();
        long long target_hns =
            static_cast<long long>(ts.tv_sec) * 10000000LL + ts.tv_nsec / 100;
        if (timeout->is_realtime()) {
          nt_storage.QuadPart = target_hns + EPOCH_DIFF_HNS;
        } else {
          ULONGLONG now_hns;
          ::RtlQueryUnbiasedInterruptTime(&now_hns);
          long long relative_hns =
              target_hns - static_cast<long long>(now_hns);
          // Already past the deadline → request an immediate poll.
          // QuadPart=0 means "do not wait" in NT semantics.
          nt_storage.QuadPart = (relative_hns <= 0) ? 0 : -relative_hns;
        }
        nt_to = &nt_storage;
      }

      NTSTATUS st =
          ::NtWaitForSingleObject(th, /*Alertable=*/TRUE, nt_to);
      if (st == STATUS_USER_APC || st == STATUS_ALERTED) {
        cancel::check();
        continue;
      }
      if (st == STATUS_TIMEOUT)
        return ETIMEDOUT;
      // Thread terminated (or wait failed unrecoverably — treat as
      // "done" so the joiner can drain whatever lifecycle state was
      // set during exit; the kernel-handle wait failing for reasons
      // other than termination is itself an exceptional condition).
      return 0;
    }
  }

  // Handle-less path: lifecycle was registered without an owned thread
  // handle (test scaffolding / foreign-thread paths that didn't go
  // through Thread::run). Fall back to the cooperative exit_word futex,
  // which the dying thread RELEASE-stores to 0 before NtTerminateThread.
  // The futex wait is itself cancellable.
  while (lc->exit_word.load() != 0) {
    long ret = lc->exit_word.wait(EXIT_WORD_LIVE, timeout,
                                   /*is_shared=*/true);
    if (ret == -ETIMEDOUT)
      return ETIMEDOUT;
    if (ret < 0 && ret != -EINTR)
      return EINVAL;
  }
  return 0;
}

// C5 + H6 fix: pthread_join is now a real POSIX cancellation point;
// external termination no longer wedges the joiner.
void Thread::wait() {
  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);
  (void)wait_until(lc, cpp::nullopt);
}

int Thread::join(ThreadReturnValue &retval) {
  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);

  // Atomically claim cleanup ownership. EINVAL on double-join,
  // detached-join, or other invalid state — closes the C2 race.
  int rc = claim_join_ownership(lc);
  if (rc != 0)
    return rc;

  wait();

  if (attrib->style == ThreadStyle::POSIX)
    retval.posix_retval = lc->retval.posix_retval;
  else
    retval.stdc_retval = lc->retval.stdc_retval;

  cleanup_thread_resources(attrib, lc);
  // Hand lc to Crystalline. The handle (lc->thread_handle) is closed
  // by free_thread_registry_node's Lifecycle FreeFn — see C4.
  free_thread_lifecycle(lc);
  return 0;
}

// Drain a successfully-waited lifecycle: claim cleanup ownership,
// pull retval out, retire the lifecycle. Shared by try_join and
// timed_join — the wait succeeded (kernel handle signalled or
// exit_word == 0), so the only way claim_join_ownership fails is a
// concurrent join/detach on the same pthread_t (UB per POSIX), which
// we still convert to EINVAL rather than corrupt the state machine.
LIBC_INLINE static int complete_join(ThreadAttributes *attrib,
                                     ThreadLifecycle *lc,
                                     ThreadReturnValue &retval) {
  int rc = claim_join_ownership(lc);
  if (rc != 0)
    return rc;

  if (attrib->style == ThreadStyle::POSIX)
    retval.posix_retval = lc->retval.posix_retval;
  else
    retval.stdc_retval = lc->retval.stdc_retval;

  cleanup_thread_resources(attrib, lc);
  free_thread_lifecycle(lc);
  return 0;
}

int Thread::try_join(ThreadReturnValue &retval) {
  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);

  // Pre-flight: refuse the obviously-invalid states without touching
  // the kernel. DETACHED / JOINING are POSIX-defined errors here.
  uint32_t st = lc->detach_state.load(cpp::MemoryOrder::ACQUIRE);
  if (st == uint32_t(DetachState::DETACHED) ||
      st == uint32_t(DetachState::JOINING))
    return EINVAL;

  // Zero-timeout absolute deadline → kernel returns STATUS_TIMEOUT
  // immediately if the thread hasn't terminated. Any timespec with
  // both fields zero parses as a valid AbsTimeout.
  timespec zero{};
  auto deadline = internal::AbsTimeout::from_timespec(zero,
                                                     /*realtime=*/false);
  // from_timespec only fails on negative values — zero is always valid.
  int wait_rc = wait_until(lc, deadline ? cpp::optional<internal::AbsTimeout>(
                                              *deadline)
                                        : cpp::nullopt);
  if (wait_rc == ETIMEDOUT)
    return EBUSY; // Glibc semantics: tryjoin reports EBUSY, not ETIMEDOUT.
  if (wait_rc != 0)
    return wait_rc;
  return complete_join(attrib, lc, retval);
}

int Thread::timed_join(ThreadReturnValue &retval, clockid_t clockid,
                       const struct timespec *abstime) {
  if (!abstime)
    return EINVAL;

  // Match glibc exactly: the entire pthread_*_clock* family
  // (pthread_cond_clockwait, pthread_mutex_clocklock, pthread_rwlock_clock*,
  // sem_clockwait, pthread_clockjoin_np) accepts only CLOCK_REALTIME and
  // CLOCK_MONOTONIC. Other clocks — BOOTTIME, CPUTIME, COARSE — return
  // EINVAL even though some have well-defined semantics, so portable code
  // can't accidentally rely on a permissive libc.
  bool realtime;
  if (clockid == CLOCK_REALTIME)
    realtime = true;
  else if (clockid == CLOCK_MONOTONIC)
    realtime = false;
  else
    return EINVAL;

  auto deadline = internal::AbsTimeout::from_timespec(*abstime, realtime);
  if (!deadline) {
    // Invalid timespec (negative tv_nsec / out-of-range) → EINVAL.
    // Already-expired deadline (negative tv_sec) → ETIMEDOUT, after
    // first checking whether the thread has already terminated so a
    // racing exit before the call still yields a successful join.
    if (deadline.error() == internal::AbsTimeout::Error::Invalid)
      return EINVAL;
    // BeforeEpoch — fall through to a zero-timeout poll: a thread that
    // already exited will join successfully; a live thread reports
    // ETIMEDOUT.
    timespec zero{};
    auto poll = internal::AbsTimeout::from_timespec(zero,
                                                   /*realtime=*/false);
    auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);
    uint32_t st = lc->detach_state.load(cpp::MemoryOrder::ACQUIRE);
    if (st == uint32_t(DetachState::DETACHED) ||
        st == uint32_t(DetachState::JOINING))
      return EINVAL;
    int wait_rc = wait_until(lc, *poll);
    if (wait_rc == ETIMEDOUT)
      return ETIMEDOUT;
    if (wait_rc != 0)
      return wait_rc;
    return complete_join(attrib, lc, retval);
  }

  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);
  uint32_t st = lc->detach_state.load(cpp::MemoryOrder::ACQUIRE);
  if (st == uint32_t(DetachState::DETACHED) ||
      st == uint32_t(DetachState::JOINING))
    return EINVAL;

  int wait_rc = wait_until(lc, *deadline);
  if (wait_rc == ETIMEDOUT)
    return ETIMEDOUT;
  if (wait_rc != 0)
    return wait_rc;
  return complete_join(attrib, lc, retval);
}

int Thread::detach() {
  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);

  // Fast path: the running thread is JOINABLE; flip to DETACHED so the
  // dying thread's lifecycle_cleanup runs the retire itself.
  uint32_t expected = uint32_t(DetachState::JOINABLE);
  if (lc->detach_state.compare_exchange_strong(
          expected, uint32_t(DetachState::DETACHED),
          cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE)) {
    return int(DetachType::SIMPLE);
  }

  // Slow path: thread already EXITING. Atomically claim cleanup as a
  // late detacher (state EXITING → JOINING). Same retire ownership
  // semantics as Thread::join — the JOINING claimant runs cleanup.
  if (expected == uint32_t(DetachState::EXITING)) {
    uint32_t exiting = uint32_t(DetachState::EXITING);
    if (lc->detach_state.compare_exchange_strong(
            exiting, uint32_t(DetachState::JOINING),
            cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE)) {
      wait();
      cleanup_thread_resources(attrib, lc);
      free_thread_lifecycle(lc);
      return int(DetachType::CLEANUP);
    }
  }

  // DETACHED / JOINING / unrecognized — double-detach or detach-after-
  // join. Refuse without further side effect; pthread_detach reports
  // success per its public API but the underlying state is unchanged.
  return int(DetachType::SIMPLE);
}

bool Thread::operator==(const Thread &thread) const {
  return attrib->tid == thread.attrib->tid;
}

// Structure for NtSetInformationThread/NtQueryInformationThread thread name
struct THREAD_NAME_INFORMATION {
  UNICODE_STRING ThreadName;
};

int Thread::set_name(const cpp::string_view &name) {
  if (name.size() >= NAME_SIZE_MAX)
    return ERANGE;

  // Convert UTF-8 name to UTF-16 for NtSetInformationThread
  WCHAR wide_name[NAME_SIZE_MAX];
  int len =
      windows::utf8_to_wide_n(name.data(), name.size(), wide_name, NAME_SIZE_MAX - 1);
  if (len < 0)
    return EINVAL;
  wide_name[len] = u'\0';

  HANDLE handle;
  bool opened = false;
  if (*this == self) {
    handle = NtCurrentThread();
  } else {
    CLIENT_ID cid{nullptr,
                  reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(attrib->tid))};
    if (!NT_SUCCESS(NtOpenThread(&handle, THREAD_SET_LIMITED_INFORMATION,
                                 nullptr, &cid)))
      return ESRCH;
    opened = true;
  }

  // Use NtSetInformationThread with ThreadNameInformation (class 38)
  windows::nt_wstring_view wsv(wide_name, len);
  THREAD_NAME_INFORMATION tni;
  tni.ThreadName = *wsv.unicode_string();

  NTSTATUS status = NtSetInformationThread(
      handle, ThreadNameInformation, &tni, sizeof(tni));

  if (opened)
    NtClose(handle);

  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);
  return 0;
}

int Thread::get_name(cpp::StringStream &name) const {
  if (name.bufsize() < NAME_SIZE_MAX)
    return ERANGE;

  HANDLE handle;
  bool opened = false;
  if (*this == self) {
    handle = NtCurrentThread();
  } else {
    CLIENT_ID cid{nullptr,
                  reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(attrib->tid))};
    if (!NT_SUCCESS(NtOpenThread(&handle, THREAD_QUERY_LIMITED_INFORMATION,
                                 nullptr, &cid)))
      return ESRCH;
    opened = true;
  }

  // NtQueryInformationThread(ThreadNameInformation) writes a UNICODE_STRING
  // header at offset 0 of the caller buffer, followed by the WCHAR payload.
  // The returned Buffer pointer aliases into the same buffer — no heap
  // allocation is performed by the kernel and there is nothing to free.
  // STATUS_INFO_LENGTH_MISMATCH / STATUS_BUFFER_TOO_SMALL indicates the
  // caller buffer was too small; ReturnLength holds the required size.
  alignas(UNICODE_STRING) unsigned char
      buf[sizeof(UNICODE_STRING) + NAME_SIZE_MAX * sizeof(WCHAR)];
  ULONG return_length = 0;
  NTSTATUS status = NtQueryInformationThread(
      handle, ThreadNameInformation, buf, sizeof(buf), &return_length);

  if (opened)
    NtClose(handle);

  if (status == STATUS_INFO_LENGTH_MISMATCH ||
      status == STATUS_BUFFER_TOO_SMALL)
    return ERANGE;
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_errno(status);

  const auto *tni = reinterpret_cast<const THREAD_NAME_INFORMATION *>(buf);
  int name_len = static_cast<int>(tni->ThreadName.Length / sizeof(WCHAR));
  char utf8_name[NAME_SIZE_MAX];
  int len = windows::wide_to_utf8_n(tni->ThreadName.Buffer, name_len, utf8_name,
                                    NAME_SIZE_MAX - 1);
  if (len < 0)
    return EINVAL;
  utf8_name[len] = '\0';

  name << utf8_name << cpp::StringStream::ENDS;
  return 0;
}

[[noreturn]] void thread_exit(ThreadReturnValue retval, ThreadStyle style) {
  auto *attrib = self.attrib;
  ThreadLifecycle *lc = nullptr;

  if (attrib)
    lc = static_cast<ThreadLifecycle *>(attrib->platform_data);

  if (lc) {
    // Disable cancellation before atexit callbacks (same rationale as
    // thread_entry — prevent re-entry from cancellation points in dtors).
    lc->cancel_state.fetch_or(signal_state::CANCEL_STATE_BIT,
                             cpp::MemoryOrder::RELAXED);
  }

  // Call per-thread atexit callbacks and TSS destructors.
  // call_atexit_callbacks handles attrib==nullptr (skips atexit_callback_mgr,
  // still runs TSS dtors).
  internal::call_atexit_callbacks(attrib);

  // If this is the last thread, act as exit(0) per POSIX.
  exit_process_if_last_thread();

  // Main thread has no attrib/lifecycle — it cannot be joined, so just
  // terminate the thread. TLS cleanup will deregister from the registry.
  if (!attrib || !lc) {
    internal::tls_cleanup_run_all();
    NtTerminateThread(NtCurrentThread(), 0);
    __builtin_unreachable();
  }

  // Store return value
  attrib->retval = retval;
  if (style == ThreadStyle::POSIX)
    lc->retval.posix_retval = retval.posix_retval;
  else
    lc->retval.stdc_retval = retval.stdc_retval;

  // Same atomic exit-state transition as thread_entry_impl. CAS-claim
  // EXITING from JOINABLE; on failure the state is DETACHED or JOINING
  // and the lifecycle's retire is owned by lifecycle_cleanup or by a
  // joiner respectively. cleanup_thread_resources is a no-op (M6); the
  // HANDLE is closed by the lifecycle FreeFn (C4).
  uint32_t expected = uint32_t(DetachState::JOINABLE);
  (void)lc->detach_state.compare_exchange_strong(
      expected, uint32_t(DetachState::EXITING),
      cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE);
  cleanup_thread_resources(attrib, lc);
  lc->exit_word.store_and_notify_all(0);
  // Run libc TLS cleanup explicitly OUTSIDE the loader lock — see M1.
  internal::tls_cleanup_run_all();
  NtTerminateThread(NtCurrentThread(), 0);
  __builtin_unreachable();
}

void init_tls(TLSDescriptor &tls) {
  // PE/COFF TLS is loader-managed. .CRT$XLC triggers __cxa_thread_finalize
  // unconditionally for all threads via lifecycle_cleanup.
  tls.size = 0;
  tls.tls_index = 0;
  tls.tp = 0;
}

void cleanup_tls(uintptr_t tls_addr, uintptr_t tls_size) {
  // TLS cleanup is handled by .CRT$XLC callback when the thread exits.
  (void)tls_addr;
  (void)tls_size;
}

bool set_thread_ptr(uintptr_t val) {
  // On Windows with TLS, this is a no-op.
  (void)val;
  return true;
}

} // namespace LIBC_NAMESPACE_DECL

int LIBC_NAMESPACE::internal::thread_storage_startup_init() {
  LIBC_NAMESPACE::init_start_args_pool();
  return 0;
}

// Fork child reinit for the StartArgs slab pool.
//
// StartArgs objects are transient trampolines allocated during
// pthread_create and freed by thread_entry_impl immediately after the
// runner returns. No long-lived references exist.
//
// After fork, only the forking thread survives. Any in-flight
// pthread_create operations on non-forking threads are abandoned:
//   - If StartArgs was allocated but the thread was not yet created,
//     the slot lives in a dead-thread's slab; SlabPool::fork_reinit
//     abandons or releases that slab.
//   - If the thread was created and ran past free_start_args, the slot
//     was returned to the pool before the thread entered user code.
//
// No explicit per-slot cleanup is needed.
void LIBC_NAMESPACE::internal::thread_storage_fork_reinit() {
  LIBC_NAMESPACE::start_args_pool.fork_reinit();
}

namespace LIBC_NAMESPACE_DECL {
namespace internal {
// Free the TEB TLS slot reserved by init_tls(). Pool VA (slab chunks) is
// owned by c.dll's image and reclaimed when the DLL unmaps; no explicit
// pool teardown is needed.
static void thread_storage_fini() { start_args_pool.fini_tls(); }
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(4, thread_storage,
                   &::LIBC_NAMESPACE::internal::thread_storage_fini)

LIBC_REGISTER_FORK_REINIT(thread_storage,
                          ::LIBC_NAMESPACE::internal::kForkPrioThreadStorage,
                          &::LIBC_NAMESPACE::internal::thread_storage_fork_reinit)
