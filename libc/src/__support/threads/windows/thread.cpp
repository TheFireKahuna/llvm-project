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
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/robust_list_cleanup.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"

// Signal state for mask inheritance across pthread_create. On Linux the kernel
// copies the parent's signal mask into the child during clone(), but Windows
// CreateThread has no signal concept — we must do it in userspace. This
// couples threading to the signal subsystem, which is unavoidable on Windows.
#include "src/__support/OSUtil/windows/signal/signal.h"

// NUMA policy inheritance. Linux clone() inherits the parent's mempolicy;
// Windows thread_local reinitializes to MPOL_DEFAULT. We snapshot the
// parent's policy in StartArgs and apply it in thread_entry().
#include "src/__support/OSUtil/windows/memory/numa_policy.h"

// Process identity — impersonation token inheritance for POSIX setuid.
// Linux clone() inherits the parent's credentials; Windows CreateThread
// inherits the process token, not any thread's impersonation token.
#include "src/__support/OSUtil/windows/process/process_identity.h"
#include "src/__support/OSUtil/windows/resource/rlimit_data_guard.h"
#include "src/__support/OSUtil/windows/process_control_block.h"

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

struct alignas(STACK_ALIGNMENT) ThreadStartStorage {
  StartArgs start_args;
  ThreadAttributes attrib;
  signal_state::ThreadSignalState sig_state;
};

namespace {

constexpr size_t kThreadStorageSlotAlign = alignof(ThreadStartStorage);
constexpr size_t kThreadStorageSlotSize =
    (sizeof(ThreadStartStorage) + kThreadStorageSlotAlign - 1) &
    ~(kThreadStorageSlotAlign - 1);

static_assert(kThreadStorageSlotSize >= sizeof(void *),
              "ThreadStartStorage slab slot must be pointer-sized");

internal::SlabPool thread_storage_pool;

LIBC_INLINE void init_thread_storage_pool() {
  thread_storage_pool.init(kThreadStorageSlotSize, kThreadStorageSlotAlign);
  thread_storage_pool.init_tls();
}

} // namespace

LIBC_INLINE bool round_up_to_page(size_t value, size_t &rounded) {
  if (value > SIZE_MAX - (EXEC_PAGESIZE - 1))
    return false;
  rounded = (value + EXEC_PAGESIZE - 1) & ~(size_t(EXEC_PAGESIZE) - 1);
  return true;
}

LIBC_INLINE ErrorOr<ThreadStartStorage *> alloc_thread_storage() {
  init_thread_storage_pool();
  void *storage = thread_storage_pool.tls_alloc();
  if (!storage)
    return Error{ENOMEM};
  return static_cast<ThreadStartStorage *>(storage);
}

LIBC_INLINE void free_thread_storage(void *storage) {
  internal::SlabPool::free(storage);
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
[[gnu::always_inline]] LIBC_INLINE void
cleanup_thread_resources(ThreadAttributes *attrib, ThreadLifecycle *lc) {
  void *attrib_storage = lc ? lc->attrib_storage : nullptr;

  cleanup_tls(attrib->tls, attrib->tls_size);

  if (lc) {
    HANDLE h = lc->thread_handle.exchange(nullptr, cpp::MemoryOrder::ACQ_REL);
    if (h)
      ::NtClose(h);
  }

  if (attrib_storage)
    free_thread_storage(attrib_storage);
}

// Free the ThreadLifecycle allocation. Called by the joiner (joinable) or
// by a deferred path after the thread has fully exited (detached).
LIBC_INLINE void free_thread_lifecycle(ThreadLifecycle *lc) {
  if (!lc)
    return;
  registry_deregister_and_free(lc);
}

// Thread entry implementation — called by the guarded wrapper below.
static DWORD WINAPI thread_entry_impl(void *arg) {
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

  // Register the stable per-thread signal state with TLS and inherit the
  // parent's signal mask.
  signal_state::register_thread_state(start_args->sig_state);
  start_args->sig_state->blocked_signals = start_args->inherited_sigmask;
  start_args->sig_state->sched_policy = start_args->inherited_sched_policy;

  // Wire the signal state into the lifecycle root.
  lc->signal = start_args->sig_state;

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
  lc->cancel_word.fetch_or(signal_state::CANCEL_STATE_BIT,
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

  uint32_t joinable_state = uint32_t(DetachState::JOINABLE);
  if (!lc->detach_state.compare_exchange_strong(
          joinable_state, uint32_t(DetachState::EXITING))) {
    cleanup_thread_resources(attrib, lc);
    // Now signal completion. The lifecycle survives — either nobody is
    // waiting (pure detach) or a late detacher will free it.
    lc->exit_word.store_and_notify_all(0);
    NtTerminateThread(NtCurrentThread(), 0);
  }

  // Signal completion via futex (matching Linux clear_tid behavior).
  // Safe here: the joiner owns cleanup for joinable threads.
  lc->exit_word.store_and_notify_all(0);

  NtTerminateThread(NtCurrentThread(), 0);
  __builtin_unreachable(); // NtTerminateThread never returns
}

// SEH-guarded thread entry — registered as the NtCreateThreadEx callback.
// The .seh_handler directive registers __llvm_libc_thread_fault_handler as the
// frame's language-specific handler. RtlDispatchException calls it during the
// stack walk for any unhandled exception, providing a safety net for
// demand-commit and remap-guard faults if VEH is displaced. Uses the same
// EXCEPTION_ROUTINE interface as __gxx_personality_seh0 — fully transparent
// to libunwind.
extern "C" LONG NTAPI __llvm_libc_thread_fault_handler(
    EXCEPTION_RECORD *, void *, CONTEXT *, DISPATCHER_CONTEXT *);

#if defined(__x86_64__)
extern "C" [[gnu::naked]]
DWORD WINAPI thread_entry_guarded(void * /*arg*/) {
  asm volatile(R"(
      .seh_proc thread_entry_guarded
      .seh_handler __llvm_libc_thread_fault_handler, @except
      push %%rbp
      .seh_pushreg %%rbp
      mov %%rsp, %%rbp
      .seh_setframe %%rbp, 0
      sub $32, %%rsp
      .seh_stackalloc 32
      .seh_endprologue
      call %P[impl]
      add $32, %%rsp
      pop %%rbp
      retq
      .seh_endproc
  )" :: [impl] "X"(thread_entry_impl));
}
#elif defined(__aarch64__)
extern "C" [[gnu::naked]]
DWORD WINAPI thread_entry_guarded(void * /*arg*/) {
  asm volatile(R"(
      .seh_proc thread_entry_guarded
      .seh_handler __llvm_libc_thread_fault_handler, @except
      pacibsp
      .seh_pac_sign_lr
      stp x29, x30, [sp, #-16]!
      .seh_save_fplr_x 16
      mov x29, sp
      .seh_set_fp
      .seh_endprologue
      bl %[impl]
      .seh_startepilogue
      ldp x29, x30, [sp], #16
      .seh_save_fplr_x 16
      autibsp
      .seh_pac_sign_lr
      .seh_endepilogue
      ret
      .seh_endproc
  )" :: [impl] "S"(thread_entry_impl));
}
#endif

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

  auto storage_alloc = alloc_thread_storage();
  if (!storage_alloc) {
    free_thread_lifecycle(lc);
    return ENOMEM;
  }
  auto *storage = storage_alloc.value();
  auto *start_args = &storage->start_args;
  attrib = &storage->attrib;
  auto *sig_state = &storage->sig_state;

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
  lc->attrib_storage = storage;
  lc->attrib = attrib;

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

  // Create the thread using NtCreateThreadEx (no kernel32 dependency).
  // thread_entry_guarded wraps thread_entry_impl with a .seh_handler
  // for demand-commit/remap-guard fault recovery if VEH is displaced.
  HANDLE handle;
  NTSTATUS status = NtCreateThreadEx(
      &handle,                                    // ThreadHandle
      THREAD_QUERY_LIMITED_INFORMATION |          // Access required for thread naming
          THREAD_TERMINATE |                      // Access required for thread_exit
          THREAD_SET_CONTEXT |                    // Signal delivery (APC)
          THREAD_GET_CONTEXT |                    // Registry signal/context users
          THREAD_SET_INFORMATION |                // NtCreateThreadStateChange (SIGSTOP)
          SYNCHRONIZE,                            // Join/wait operations
      nullptr,                                    // ObjectAttributes
      NtCurrentProcess(),                         // ProcessHandle
      reinterpret_cast<PVOID>(thread_entry_guarded), // StartRoutine
      start_args,                                 // Argument
      0,                                          // CreateFlags (0 = run immediately)
      0,                                          // ZeroBits
      0,                                          // StackSize (use default)
      stack_reserve_size,                         // MaximumStackSize (reserve)
      reinterpret_cast<PS_ATTRIBUTE_LIST *>(&attr_list)); // AttributeList

  if (!NT_SUCCESS(status)) {
    cleanup_tls(tls.tls_index, tls.size);
    free_thread_storage(storage);
    free_thread_lifecycle(lc);
    return windows_util::ntstatus_to_errno(status);
  }

  DWORD child_tid = static_cast<DWORD>(
      reinterpret_cast<ULONG_PTR>(child_client_id.UniqueThread));
  lc->tid = static_cast<int>(child_tid);
  attrib->tid = lc->tid;
  populate_kernel_stack_metadata(attrib, lc, static_cast<TEB *>(child_teb));

  lc->thread_handle.store(handle, cpp::MemoryOrder::RELEASE);

  // Register the lifecycle from the parent thread BEFORE closing the
  // creation handle. This eliminates the race where pthread_cancel is
  // called before the child thread has had a chance to self-register,
  // which would cause ESRCH.
  // The handle is adopted (not duplicated) since NtCreateThreadEx just
  // created it. registry_register stores the SlotRef fields in lc.
  registry_register(lc, handle, child_tid, /*duplicate_handle=*/false);

  return 0;
}

int Thread::join(ThreadReturnValue &retval) {
  wait();

  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);

  if (attrib->style == ThreadStyle::POSIX)
    retval.posix_retval = lc->retval.posix_retval;
  else
    retval.stdc_retval = lc->retval.stdc_retval;

  cleanup_thread_resources(attrib, lc);
  free_thread_lifecycle(lc);
  return 0;
}

int Thread::detach() {
  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);

  uint32_t joinable_state = uint32_t(DetachState::JOINABLE);
  if (lc->detach_state.compare_exchange_strong(
          joinable_state, uint32_t(DetachState::DETACHED))) {
    return int(DetachType::SIMPLE);
  }

  // Thread is exiting, wait for it and clean up
  wait();
  cleanup_thread_resources(attrib, lc);
  free_thread_lifecycle(lc);
  return int(DetachType::CLEANUP);
}

// Wait for thread termination. On Windows the thread object itself is the
// reliable one-shot completion primitive, so join/detach use the owned thread
// handle rather than the futex-style exit word.
void Thread::wait() {
  auto *lc = static_cast<ThreadLifecycle *>(attrib->platform_data);
  HANDLE th = lc->thread_handle.load(cpp::MemoryOrder::ACQUIRE);
  if (th) {
    ::NtWaitForSingleObject(th, /*Alertable=*/FALSE,
                            /*Timeout=*/nullptr);
    return;
  }

  // Fallback: detached threads that closed the join handle still publish
  // completion in exit_word.
  while (lc->exit_word.load() != 0)
    lc->exit_word.wait(EXIT_WORD_LIVE, cpp::nullopt, true);
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
  THREAD_NAME_INFORMATION tni;
  tni.ThreadName.Buffer = wide_name;
  tni.ThreadName.Length = static_cast<USHORT>(len * sizeof(WCHAR));
  tni.ThreadName.MaximumLength = tni.ThreadName.Length + sizeof(WCHAR);

  NTSTATUS status = NtSetInformationThread(
      handle, ThreadNameInformation, &tni, sizeof(tni));

  if (opened)
    NtClose(handle);

  return NT_SUCCESS(status) ? 0 : EINVAL;
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

  // Query thread name using NtQueryInformationThread with ThreadNameInformation
  // First query to get the size, then allocate and query again
  THREAD_NAME_INFORMATION tni;
  ULONG return_length = 0;
  NTSTATUS status = NtQueryInformationThread(
      handle, ThreadNameInformation, &tni, sizeof(tni), &return_length);

  if (opened)
    NtClose(handle);

  if (!NT_SUCCESS(status) || tni.ThreadName.Buffer == nullptr)
    return EINVAL;

  // Convert UTF-16 to UTF-8
  char utf8_name[NAME_SIZE_MAX];
  int name_len = tni.ThreadName.Length / sizeof(WCHAR);
  int len = windows::wide_to_utf8_n(
      tni.ThreadName.Buffer, name_len, utf8_name, NAME_SIZE_MAX);

  // Free the string allocated by NtQueryInformationThread using RtlFreeHeap
  HANDLE heap = reinterpret_cast<HANDLE>(NtCurrentPeb()->ProcessHeap);
  RtlFreeHeap(heap, 0, tni.ThreadName.Buffer);

  if (len <= 0)
    return EINVAL;

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
    lc->cancel_word.fetch_or(signal_state::CANCEL_STATE_BIT,
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

  uint32_t joinable_state = uint32_t(DetachState::JOINABLE);
  if (!lc->detach_state.compare_exchange_strong(
          joinable_state, uint32_t(DetachState::EXITING))) {
    cleanup_thread_resources(attrib, lc);
    lc->exit_word.store_and_notify_all(0);
    // Run libc TLS cleanup (lifecycle_cleanup, slot_cleanup, etc.) BEFORE
    // NtTerminateThread. NtTerminateThread triggers LdrShutdownThread which
    // fires the same TLS callbacks under the loader lock. By running them
    // here first and clearing the TEB TLS values, the LdrShutdownThread
    // pass becomes a no-op — avoiding loader-lock serialization of heavy
    // cleanup work (__cxa_thread_finalize, registry ops, IoRing teardown).
    internal::tls_cleanup_run_all();
    NtTerminateThread(NtCurrentThread(), 0);
    __builtin_unreachable();
  }

  // Signal completion via futex (matching Linux clear_tid behavior).
  // Safe here: the joiner owns cleanup for joinable threads.
  lc->exit_word.store_and_notify_all(0);

  // Run libc TLS cleanup outside the loader lock — same rationale as above.
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
  LIBC_NAMESPACE::init_thread_storage_pool();
  return 0;
}

// Fork child reinit for the thread storage pool.
//
// Thread trimming: ThreadStartStorage objects are transient trampolines
// allocated during pthread_create() and freed immediately after the new
// thread copies out its start_args and signal state.  No long-lived
// references exist — by the time any thread is running user code, its
// ThreadStartStorage has already been returned to the pool.
//
// After fork, only the forking thread survives.  Any in-flight
// pthread_create() operations from non-forking threads are abandoned:
//   - If the storage was allocated but the thread was not yet created,
//     the storage is an unreturned slot in a dead-thread's slab.
//     SlabPool::fork_reinit() will abandon or release that slab.
//   - If the thread was created and running, the storage was already
//     freed (returned to the pool) before the thread entered user code.
//
// No explicit per-slot cleanup is needed — the pool reset handles
// all memory reclamation for dead threads.
void LIBC_NAMESPACE::internal::thread_storage_fork_reinit() {
  // Reset the pool: reclaim dead-thread slabs, reset internal locks.
  // The forking thread's TLS slab pointer (managed internally by
  // SlabPool via init_tls()) is preserved — the TEB TLS slot value
  // is inherited from the parent and points to a valid slab owned
  // by the forking thread's TID.
  LIBC_NAMESPACE::thread_storage_pool.fork_reinit();
}
