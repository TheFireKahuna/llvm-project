//===-- Child process tracking table implementation -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/process/windows/child_table.h"
#include "hdr/errno_macros.h"
#include "src/__support/CPP/new.h"
#include "src/__support/CPP/utility.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/threads/windows/spin_wait.h"

// Full definition of ChildStateBlock + signal delivery + SA flag queries.
#include "src/__support/OSUtil/windows/signal/signal_internal.h"

// For W_EXITCODE / wait status encoding.
#include "llvm-libc-macros/sys-wait-macros.h"

// For SIGSEGV / SIGILL / SIGFPE / etc. when mapping NTSTATUS exit codes.
#include "hdr/signal_macros.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace process {

// Pool is workload-bounded (growable). The previous file-local static
// `child_current_slab` was a process-wide cache (not thread_local) — every
// thread shared one slab pointer, racing on alloc_slow updates. Migrated
// to pool-managed TLS so each thread has its own cached slab and dead
// threads' slabs reach the Crystalline retire path via FLS abandon.
static internal::SlabPool child_pool;

namespace {

HANDLE create_unsignaled_notification_event() {
  HANDLE event = nullptr;
  OBJECT_ATTRIBUTES oa = {};
  oa.Length = sizeof(oa);
  NTSTATUS st = NtCreateEvent(&event, EVENT_MODIFY_STATE | SYNCHRONIZE, &oa,
                              NotificationEvent, FALSE);
  return NT_SUCCESS(st) ? event : nullptr;
}

void filetime_to_timeval(LARGE_INTEGER ft, struct timeval &tv) {
  LONGLONG total_us = ft.QuadPart / 10;
  tv.tv_sec = static_cast<decltype(tv.tv_sec)>(total_us / 1000000);
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>(total_us % 1000000);
}

// Snapshot the child's cumulative user/kernel CPU time in microseconds for
// SIGCHLD si_utime / si_stime. Returns (0, 0) if the query fails (handle
// revoked early, child terminated unexpectedly) — POSIX permits these
// fields to be best-effort.
void query_child_cpu_times_us(HANDLE process_handle, long long &utime_us,
                              long long &stime_us) {
  utime_us = 0;
  stime_us = 0;
  if (!process_handle)
    return;
  KERNEL_USER_TIMES times;
  NTSTATUS st = ::NtQueryInformationProcess(process_handle, ProcessTimes,
                                            &times, sizeof(times), nullptr);
  if (NT_SUCCESS(st)) {
    // FILETIME is 100-ns ticks; /10 → microseconds.
    utime_us = times.UserTime.QuadPart / 10;
    stime_us = times.KernelTime.QuadPart / 10;
  }
}

void capture_child_rusage(HANDLE process_handle, struct rusage &usage) {
  __builtin_memset(&usage, 0, sizeof(usage));

  KERNEL_USER_TIMES times;
  NTSTATUS st = ::NtQueryInformationProcess(process_handle, ProcessTimes, &times,
                                            sizeof(times), nullptr);
  if (NT_SUCCESS(st)) {
    filetime_to_timeval(times.UserTime, usage.ru_utime);
    filetime_to_timeval(times.KernelTime, usage.ru_stime);
  }

  VM_COUNTERS vmc;
  st = ::NtQueryInformationProcess(process_handle, ProcessVmCounters, &vmc,
                                   sizeof(vmc), nullptr);
  if (NT_SUCCESS(st)) {
    usage.ru_maxrss = static_cast<long>(vmc.PeakWorkingSetSize / 1024);
    usage.ru_minflt = static_cast<long>(vmc.PageFaultCount);
  }

  IO_COUNTERS ioc;
  st = ::NtQueryInformationProcess(process_handle, ProcessIoCounters, &ioc,
                                   sizeof(ioc), nullptr);
  if (NT_SUCCESS(st)) {
    usage.ru_inblock = static_cast<long>(ioc.ReadOperationCount);
    usage.ru_oublock = static_cast<long>(ioc.WriteOperationCount);
  }
}

ChildEntry *alloc_entry() {
  void *slot = child_pool.tls_alloc();
  if (!slot)
    return nullptr;
  return ::new (slot) ChildEntry{};
}

void free_entry(ChildEntry *entry) {
  entry->~ChildEntry();
  internal::SlabPool::free(entry);
}

void release_state_resources(ChildEntry *entry) {
  if (entry->state_change_event) {
    ::NtClose(entry->state_change_event);
    entry->state_change_event = nullptr;
  }
  entry->state_view = windows::SectionView();
  entry->state_section = windows::SectionHandle();
}

PgidBucket *find_or_create_pgid_bucket(pid_t pgid);
void link_to_group(ChildEntry *entry);
void unlink_from_group(ChildEntry *entry);

// Map a Windows unhandled-exception exit code (an NTSTATUS that the kernel
// planted in the process's ExitStatus when the top-level exception filter
// didn't handle a structured exception) to the POSIX signal number a Linux
// kernel would have delivered. Returns 0 if the exit code is not a
// recognised exception — in that case the code is a normal _Exit value.
int ntstatus_to_signal(DWORD exit_code) {
  switch (exit_code) {
  // Memory faults.
  case 0xC0000005: // STATUS_ACCESS_VIOLATION
  case 0xC00000FD: // STATUS_STACK_OVERFLOW
  case 0xC000008C: // STATUS_ARRAY_BOUNDS_EXCEEDED
    return SIGSEGV;
  case 0xC0000006: // STATUS_IN_PAGE_ERROR
  case 0x80000002: // STATUS_DATATYPE_MISALIGNMENT
    return SIGBUS;

  // Illegal instructions / CPU faults.
  case 0xC000001D: // STATUS_ILLEGAL_INSTRUCTION
  case 0xC0000096: // STATUS_PRIVILEGED_INSTRUCTION
  case 0xC0000025: // STATUS_NONCONTINUABLE_EXCEPTION
  case 0xC0000026: // STATUS_INVALID_DISPOSITION
    return SIGILL;

  // Arithmetic faults.
  case 0xC000008D: // STATUS_FLOAT_DENORMAL_OPERAND
  case 0xC000008E: // STATUS_FLOAT_DIVIDE_BY_ZERO
  case 0xC000008F: // STATUS_FLOAT_INEXACT_RESULT
  case 0xC0000090: // STATUS_FLOAT_INVALID_OPERATION
  case 0xC0000091: // STATUS_FLOAT_OVERFLOW
  case 0xC0000092: // STATUS_FLOAT_STACK_CHECK
  case 0xC0000093: // STATUS_FLOAT_UNDERFLOW
  case 0xC0000094: // STATUS_INTEGER_DIVIDE_BY_ZERO
  case 0xC0000095: // STATUS_INTEGER_OVERFLOW
    return SIGFPE;

  // Debug events.
  case 0x80000003: // STATUS_BREAKPOINT
  case 0x80000004: // STATUS_SINGLE_STEP
    return SIGTRAP;

  // Abort-like terminations.
  case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (also CFG / __fastfail)
  case 0x40000015: // STATUS_FATAL_APP_EXIT
    return SIGABRT;

  // Console Ctrl-C that propagated as an uncaught exception.
  case 0xC000013A: // STATUS_CONTROL_C_EXIT
    return SIGINT;

  default:
    return 0;
  }
}

// Classify a Windows exit code as signal-terminated or normally-exited.
// Returns the signal number (>0) if signaled, 0 otherwise.
//
// Two independent producers of signal death on Windows:
//   1. Our kill() and execute_default_action call TerminateProcess(h,
//      128+signum). Codes 129..128+NSIG-1 decode back to the signal.
//   2. An unhandled structured exception leaves the kernel's NTSTATUS as
//      the ExitStatus (e.g. 0xC0000005 for a fault).  ntstatus_to_signal
//      maps those to the matching POSIX signal.
int exit_code_to_signal(DWORD exit_code) {
  if (exit_code > 128 && exit_code < 128 + NSIG)
    return static_cast<int>(exit_code - 128);
  return ntstatus_to_signal(exit_code);
}

// Encode a Windows exit code as a POSIX wait status.
int encode_wait_status(DWORD exit_code) {
  if (int sig = exit_code_to_signal(exit_code))
    return sig; // WIFSIGNALED, signal in low 7 bits
  return W_EXITCODE(static_cast<int>(exit_code & 0xFF), 0);
}

// Check if SA_NOCLDWAIT is set on the current SIGCHLD disposition.
bool is_nocldwait_set() {
  auto &handler = g_pcb.signal_handler;
  handler.lock.lock();
  bool result =
      (handler.handlers[SIGCHLD].sa_flags & SA_NOCLDWAIT) != 0;
  handler.lock.unlock();
  return result;
}

// Check if SA_NOCLDSTOP is set on the current SIGCHLD disposition.
bool is_nocldstop_set() {
  auto &handler = g_pcb.signal_handler;
  handler.lock.lock();
  bool result =
      (handler.handlers[SIGCHLD].sa_flags & SA_NOCLDSTOP) != 0;
  handler.lock.unlock();
  return result;
}

// Reactor callback for state-change notification. Fired on the drain thread
// when a child signals its state_change_event (stop or continue).
// Context is ChildEntry* — safe because the reactor's unwatch() guarantee
// ensures no callback executes after deregistration, and the drain thread
// is serial (no concurrent state + exit callbacks).
void child_state_reactor_cb(void *context, NTSTATUS /*status*/,
                            ULONG_PTR /*information*/) {
  auto *entry = static_cast<ChildEntry *>(context);

  auto &table = g_pcb.child_table;
  int claimed_state = signal_state::CHILD_STATE_RUNNING;
  bool should_notify = false;

  table.lock.lock();

  if (!entry->exited.load() && entry->state_block()) {
    // Protocol: clear event, exchange state, re-arm reactor watch.
    // NtClearEvent before rearm ensures we don't immediately re-fire
    // on the stale signal. If the child signals between clear and rearm,
    // NtAssociateWaitCompletionPacket detects already_signaled and the
    // WCP fires immediately — correct behavior.
    if (entry->state_change_event)
      NtClearEvent(entry->state_change_event);

    claimed_state = entry->state_block()->state.exchange(
        signal_state::CHILD_STATE_RUNNING, cpp::MemoryOrder::ACQ_REL);

    if (claimed_state != signal_state::CHILD_STATE_RUNNING) {
      entry->child_state.store(claimed_state, cpp::MemoryOrder::RELEASE);
      should_notify = true;
    }

    // Re-arm for the next state change.
    if (entry->state_token.valid())
      reactor::rearm(entry->state_token);
  }

  table.lock.unlock();

  if (should_notify) {
    NtSetEvent(table.any_child_event, nullptr);

    // SA_NOCLDSTOP suppresses SIGCHLD for stop/continue events.
    if (!is_nocldstop_set()) {
      int code = (claimed_state == signal_state::CHILD_STATE_CONTINUED)
                     ? CLD_CONTINUED
                     : CLD_STOPPED;
      int status = (claimed_state > 0) ? claimed_state : SIGCONT;
      long long utime_us = 0, stime_us = 0;
      query_child_cpu_times_us(entry->process_handle, utime_us, stime_us);
      signal_state::deliver_sigchld(code, static_cast<int>(entry->pid),
                                    status, utime_us, stime_us);
    }
  }
}

// Reactor callback — fires on the drain thread when a child process exits.
void child_exit_reactor_cb(void *context, NTSTATUS /*status*/,
                           ULONG_PTR /*information*/) {
  auto *entry = static_cast<ChildEntry *>(context);

  PROCESS_BASIC_INFORMATION pbi = {};
  NTSTATUS st = NtQueryInformationProcess(entry->process_handle,
                                          ProcessBasicInformation,
                                          &pbi, sizeof(pbi), nullptr);
  DWORD exit_code = 0;
  if (NT_SUCCESS(st) && pbi.ExitStatus != STATUS_PENDING)
    exit_code = static_cast<DWORD>(pbi.ExitStatus);

  capture_child_rusage(entry->process_handle, entry->exit_rusage);

  // Snapshot CPU times in microseconds for SIGCHLD si_utime/si_stime.
  // Done from the already-captured rusage so we don't requery; the
  // FILETIME→timeval conversion above preserves μs precision (ns/10).
  long long sigchld_utime_us =
      static_cast<long long>(entry->exit_rusage.ru_utime.tv_sec) * 1000000 +
      entry->exit_rusage.ru_utime.tv_usec;
  long long sigchld_stime_us =
      static_cast<long long>(entry->exit_rusage.ru_stime.tv_sec) * 1000000 +
      entry->exit_rusage.ru_stime.tv_usec;

  if (is_nocldwait_set()) {
    // SA_NOCLDWAIT: auto-reap the child. No zombie state, waitpid sees
    // ECHILD. We hold exclusive lock to unlink from the active list.
    auto &table = g_pcb.child_table;
    table.lock.lock();

    ChildReapTokens tokens = unlink_child_for_reap(entry);

    // Unwatch state-change reactor registration. Safe: drain thread is
    // serial, so no state callback is executing concurrently.
    if (tokens.state_token.valid())
      reactor::unwatch(tokens.state_token);

    // Detach this exit watch from within its own callback. Uses detach()
    // instead of unwatch() to avoid deadlock on the dispatching spin-wait
    // (we ARE the currently-dispatching callback).
    if (tokens.exit_token.valid())
      reactor::detach(tokens.exit_token);

    // Release state-change NT handles (event, section, mapped view).
    release_state_resources(entry);

    // Entry is unlinked — no other thread can find it via the active
    // list. Drop the lock before the spin drain + handle close to avoid
    // blocking all child table operations on the drain latency.
    DWORD child_pid = entry->pid;
    table.lock.unlock();

    // Wake any threads parked on exit_futex, then drain wait_refs.
    // All waiters that incremented wait_refs have already dropped the
    // lock and are in exit_futex.wait() or past it. The
    // store_and_notify_all(1) wakes them; the futex fast-path (CAS-64
    // value mismatch) returns in ~1 instruction.
    entry->exit_futex.store_and_notify_all(1);
    // Address-monitor park (UMWAIT C0.2 / MWAITX) on the wait_refs cache
    // line — wakes on the last waiter's fetch_sub write, no cycles burned
    // while it's still nonzero.
    for (;;) {
      uint32_t refs = entry->wait_refs.load(cpp::MemoryOrder::ACQUIRE);
      if (refs == 0)
        break;
      spin_wait::spin_on_raw(
          reinterpret_cast<uint32_t *>(&entry->wait_refs.val), refs, 4096);
    }

    // Close process handle.
    if (entry->process_handle) {
      ::NtClose(entry->process_handle);
      entry->process_handle = nullptr;
    }

    // Return to pool under lock.
    table.lock.lock();
    free_entry(entry);
    table.lock.unlock();

    int sig = exit_code_to_signal(exit_code);
    int code = sig ? CLD_KILLED : CLD_EXITED;
    int status = sig ? sig : static_cast<int>(exit_code & 0xFF);
    signal_state::deliver_sigchld(code, static_cast<int>(child_pid), status,
                                  sigchld_utime_us, sigchld_stime_us);
    return;
  }

  // Normal path: mark exited and wake waitpid.
  entry->wait_status.store(encode_wait_status(exit_code));
  entry->exited.store(true);

  // Wake per-child futex waiters (P_PID fast path in waitid_core).
  // store_and_notify_all(1) → NtAlertMultipleThreadByThreadId batch wake.
  entry->exit_futex.store_and_notify_all(1);

  auto &table = g_pcb.child_table;
  NtSetEvent(table.any_child_event, nullptr);

  int sig = exit_code_to_signal(exit_code);
  int code = sig ? CLD_KILLED : CLD_EXITED;
  int status = sig ? sig : static_cast<int>(exit_code & 0xFF);
  signal_state::deliver_sigchld(code, static_cast<int>(entry->pid), status,
                                sigchld_utime_us, sigchld_stime_us);
}

} // anonymous namespace

bool ChildEntry::has_execd() const {
  auto *block = state_block();
  if (!block)
    return true; // no shared memory — assume exec'd (posix_spawn path)
  return block->exec_count.load(cpp::MemoryOrder::ACQUIRE) > 0;
}

void init_child_table() {
  // RawMutex and active are zero-initialized by PCB demand-zero.
  child_pool.init(sizeof(ChildEntry), alignof(ChildEntry));
  // Pool-managed TLS so the slab abandons via FLS at thread exit.
  child_pool.init_tls(internal::kTlsCleanupPhaseAllocator);
  g_pcb.child_table.any_child_event = create_unsignaled_notification_event();

  // Allocate the pgid hash index externally (1 page, demand-zero).
  // PgidBucket is 16 bytes × 64 = 1024 bytes — fits in one 4 KB page.
  void *idx = internal::page_reserve(PGID_BUCKET_COUNT * sizeof(PgidBucket));
  if (idx &&
      !internal::page_commit(idx, PGID_BUCKET_COUNT * sizeof(PgidBucket))) {
    internal::page_free(idx);
    idx = nullptr;
  }
  g_pcb.child_table.pgid_index = static_cast<PgidBucket *>(idx);
}

int track_child(HANDLE process_handle, DWORD pid, pid_t pgid,
                HANDLE state_event, windows::SectionHandle state_section) {
  auto &table = g_pcb.child_table;
  table.lock.lock();

  ChildEntry *entry = alloc_entry();
  if (!entry) {
    table.lock.unlock();
    return ENOMEM;
  }

  entry->process_handle = process_handle;
  entry->pid = pid;
  entry->pgid = pgid;
  entry->wait_status.store(-1, cpp::MemoryOrder::RELAXED);
  entry->exited.store(false, cpp::MemoryOrder::RELAXED);
  entry->exit_futex.reset(0);
  entry->wait_refs.store(0, cpp::MemoryOrder::RELAXED);
  __builtin_memset(&entry->exit_rusage, 0, sizeof(entry->exit_rusage));
  entry->child_state.store(signal_state::CHILD_STATE_RUNNING,
                           cpp::MemoryOrder::RELAXED);

  // Initialize group chain pointers before linking.
  entry->group_next = nullptr;
  entry->group_prev = nullptr;

  // State-change notification from child (stop/continue).
  entry->state_change_event = state_event;
  entry->state_token = reactor::INVALID_TOKEN;

  if (state_section) {
    // Map the shared section to read child state from callbacks.
    entry->state_view = windows::SectionView::map_anywhere(
        state_section, PAGE_READWRITE,
        sizeof(signal_state::ChildStateBlock));
    // Transfer section ownership to the entry.
    entry->state_section = cpp::move(state_section);
  }

  // Insert at head of active list.
  entry->prev = nullptr;
  entry->next = table.active;
  if (table.active)
    table.active->prev = entry;
  table.active = entry;

  // Link into the per-group chain.
  link_to_group(entry);

  // Arm reactor watch for exit notification (one-shot — process handle
  // signals once on exit).
  entry->exit_token =
      reactor::watch(process_handle, child_exit_reactor_cb, entry);
  if (!entry->exit_token.valid()) {
    // Without exit notification, waitpid is broken. Roll back.
    if (entry->prev)
      entry->prev->next = entry->next;
    else
      table.active = entry->next;
    if (entry->next)
      entry->next->prev = entry->prev;
    release_state_resources(entry);
    free_entry(entry);
    table.lock.unlock();
    return EAGAIN;
  }

  // Arm reactor watch for state-change notification. Context is the
  // entry pointer — safe because unwatch() guarantees no callback after
  // deregistration, and the drain thread is serial.
  if (state_event && entry->state_block()) {
    entry->state_token =
        reactor::watch(state_event, child_state_reactor_cb, entry);
    // state_token failure is non-fatal: degraded (no stop/continue
    // notification), but exit tracking and waitpid still work.
  }

  // A very short-lived child can exit before the reactor arms the process
  // wait. If the handle is already signaled here, publish the exit state
  // immediately so waitpid does not block forever waiting for an edge that
  // has already happened.
  LARGE_INTEGER zero_timeout = {};
  if (::NtWaitForSingleObject(process_handle, FALSE, &zero_timeout) ==
      STATUS_SUCCESS) {
    if (entry->exit_token.valid()) {
      reactor::unwatch(entry->exit_token);
      entry->exit_token = reactor::INVALID_TOKEN;
    }

    PROCESS_BASIC_INFORMATION pbi = {};
    NTSTATUS st = ::NtQueryInformationProcess(process_handle,
                                              ProcessBasicInformation, &pbi,
                                              sizeof(pbi), nullptr);
    DWORD exit_code = 0;
    if (NT_SUCCESS(st) && pbi.ExitStatus != STATUS_PENDING)
      exit_code = static_cast<DWORD>(pbi.ExitStatus);

    capture_child_rusage(process_handle, entry->exit_rusage);
    entry->wait_status.store(encode_wait_status(exit_code));
    entry->exited.store(true);
    entry->exit_futex.store_and_notify_all(1);
    ::NtSetEvent(table.any_child_event, nullptr);
  }

  table.lock.unlock();
  return 0;
}

ChildEntry *find_child(pid_t pid) {
  for (ChildEntry *e = g_pcb.child_table.active; e; e = e->next) {
    if (static_cast<pid_t>(e->pid) == pid)
      return e;
  }
  return nullptr;
}

ChildReapTokens unlink_child_for_reap(ChildEntry *entry) {
  // Unlink from active list (caller holds lock).
  if (entry->prev)
    entry->prev->next = entry->next;
  else
    g_pcb.child_table.active = entry->next;
  if (entry->next)
    entry->next->prev = entry->prev;

  // Unlink from per-group chain.
  unlink_from_group(entry);

  // Copy and clear reactor tokens. The entry remains valid (not freed)
  // but is no longer findable via the active list or group chain.
  ChildReapTokens tokens{entry->exit_token, entry->state_token};
  entry->exit_token = reactor::INVALID_TOKEN;
  entry->state_token = reactor::INVALID_TOKEN;
  return tokens;
}

void finalize_child_reap(ChildEntry *entry, ChildReapTokens tokens) {
  // Unwatch reactor registrations. This blocks until any in-flight
  // callback completes (spin-waits on the dispatching flag). Must be
  // called WITHOUT table lock to avoid deadlock: a state callback on
  // the drain thread may be holding dispatching=true while trying to
  // acquire table.lock.
  if (tokens.state_token.valid())
    reactor::unwatch(tokens.state_token);
  if (tokens.exit_token.valid())
    reactor::unwatch(tokens.exit_token);

  // Release state-change resources (event, section view, section handle).
  release_state_resources(entry);

  // Drain concurrent waiters before closing the handle. The child is
  // exited so exit_futex value is 1 — any thread still in wait() hits
  // the CAS-64 fast-path (value mismatch) and returns in ~1 instruction.
  // The spin here is bounded to the waiter's fetch_sub latency (~1 ns),
  // so we park the CPU in UMWAIT C0.2 / MWAITX on the wait_refs cache
  // line instead of burning cycles in PAUSE.
  for (;;) {
    uint32_t refs = entry->wait_refs.load(cpp::MemoryOrder::ACQUIRE);
    if (refs == 0)
      break;
    spin_wait::spin_on_raw(
        reinterpret_cast<uint32_t *>(&entry->wait_refs.val), refs, 4096);
  }

  // Close the process handle.
  if (entry->process_handle) {
    ::NtClose(entry->process_handle);
    entry->process_handle = nullptr;
  }

  // Destroy entry and return to pool.
  g_pcb.child_table.lock.lock();
  free_entry(entry);
  g_pcb.child_table.lock.unlock();
}

// ---------------------------------------------------------------------------
// Process group index helpers
// ---------------------------------------------------------------------------

namespace {

PgidBucket *find_pgid_bucket(pid_t pgid) {
  if (!g_pcb.child_table.pgid_index)
    return nullptr;
  unsigned hash = static_cast<unsigned>(pgid) & PGID_BUCKET_MASK;
  for (int i = 0; i < PGID_BUCKET_COUNT; ++i) {
    unsigned idx = (hash + i) & PGID_BUCKET_MASK;
    PgidBucket &b = g_pcb.child_table.pgid_index[idx];
    if (b.pgid == pgid && b.count > 0)
      return &b;
    if (b.pgid == 0 && b.count == 0)
      return nullptr; // empty slot — pgid not in table
  }
  return nullptr;
}

PgidBucket *find_or_create_pgid_bucket(pid_t pgid) {
  if (!g_pcb.child_table.pgid_index)
    return nullptr;
  unsigned hash = static_cast<unsigned>(pgid) & PGID_BUCKET_MASK;
  PgidBucket *first_empty = nullptr;
  for (int i = 0; i < PGID_BUCKET_COUNT; ++i) {
    unsigned idx = (hash + i) & PGID_BUCKET_MASK;
    PgidBucket &b = g_pcb.child_table.pgid_index[idx];
    if (b.pgid == pgid && b.count > 0)
      return &b;
    if (b.count == 0 && !first_empty)
      first_empty = &b;
  }
  if (!first_empty)
    return nullptr; // table full — extremely unlikely with 64 buckets
  first_empty->pgid = pgid;
  first_empty->head = nullptr;
  first_empty->count = 0;
  return first_empty;
}

void link_to_group(ChildEntry *entry) {
  PgidBucket *bucket = find_or_create_pgid_bucket(entry->pgid);
  if (!bucket)
    return; // degrade gracefully — entry is still on active list

  // Insert at head of group chain.
  entry->group_prev = nullptr;
  entry->group_next = bucket->head;
  if (bucket->head)
    bucket->head->group_prev = entry;
  bucket->head = entry;
  ++bucket->count;
}

void unlink_from_group(ChildEntry *entry) {
  PgidBucket *bucket = find_pgid_bucket(entry->pgid);
  if (!bucket)
    return;

  // Unlink from doubly-linked group chain.
  if (entry->group_prev)
    entry->group_prev->group_next = entry->group_next;
  else
    bucket->head = entry->group_next;
  if (entry->group_next)
    entry->group_next->group_prev = entry->group_prev;

  entry->group_prev = nullptr;
  entry->group_next = nullptr;

  --bucket->count;
  if (bucket->count == 0) {
    // Tombstone: clear the slot for reuse.
    bucket->pgid = 0;
    bucket->head = nullptr;
  }
}

} // anonymous namespace

int change_child_pgid(ChildEntry *entry, pid_t new_pgid) {
  // Validate that the target group exists or this is creating a new group
  // (new_pgid == entry's own pid).
  if (new_pgid != static_cast<pid_t>(entry->pid)) {
    PgidBucket *target = find_pgid_bucket(new_pgid);
    if (!target)
      return EPERM; // target group doesn't exist
  }

  unlink_from_group(entry);
  entry->pgid = new_pgid;
  link_to_group(entry);
  return 0;
}

bool has_process_group(pid_t pgid) { return find_pgid_bucket(pgid) != nullptr; }

int collect_group_pids(pid_t pgid, pid_t *out, int max_out) {
  PgidBucket *bucket = find_pgid_bucket(pgid);
  if (!bucket)
    return 0;

  int n = 0;
  for (ChildEntry *e = bucket->head; e && n < max_out; e = e->group_next)
    out[n++] = static_cast<pid_t>(e->pid);
  return n;
}

// ---------------------------------------------------------------------------
// Process-global process group ID — stored in PCB Zone 1
// ---------------------------------------------------------------------------

pid_t get_self_pgid() {
  if (!g_pcb.identity.pgid_initialized.load(cpp::MemoryOrder::ACQUIRE)) {
    // Initialize from PEB's ProcessGroupId — the closest NT analog.
    auto *params = NtCurrentPeb()->ProcessParameters;
    pid_t initial = params
                        ? static_cast<pid_t>(params->ProcessGroupId)
                        : static_cast<pid_t>(NtCurrentProcessId());
    g_pcb.identity.pgid.store(initial, cpp::MemoryOrder::RELAXED);
    g_pcb.identity.pgid_initialized.store(1, cpp::MemoryOrder::RELEASE);
  }
  return g_pcb.identity.pgid.load(cpp::MemoryOrder::RELAXED);
}

void set_self_pgid(pid_t pgid) {
  g_pcb.identity.pgid.store(pgid, cpp::MemoryOrder::RELAXED);
  g_pcb.identity.pgid_initialized.store(1, cpp::MemoryOrder::RELEASE);
}

// Fork reinit: reset lock, clear all entries. Child doesn't inherit
// grandchild tracking — reactor slots were already invalidated by
// reactor::fork_reinit(). Stale tokens are harmless.
void fork_reinit_child_table() {
  g_pcb.child_table.lock.reset_for_fork();
  g_pcb.child_table.active = nullptr;
  // Pool-managed TLS: SlabPool::fork_reinit handles slab re-claim under
  // the new TID. The TEB TLS slot survives fork (CoW); no explicit clear.
  child_pool.fork_reinit();
  if (g_pcb.child_table.pgid_index) {
    __builtin_memset(g_pcb.child_table.pgid_index, 0,
                     PGID_BUCKET_COUNT * sizeof(PgidBucket));
  }
  // The parent's any_child_event was created with internal_oa()
  // (non-inheritable), so the handle value is invalid in the child.
  // Just null it and create a fresh one.
  g_pcb.child_table.any_child_event = nullptr;
  g_pcb.child_table.any_child_event = create_unsignaled_notification_event();
  // Re-initialize self pgid from PEB (child inherits parent's group).
  g_pcb.identity.pgid_initialized.store(0, cpp::MemoryOrder::RELEASE);
}

} // namespace process
} // namespace LIBC_NAMESPACE_DECL

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
// Child table lock reset, before fd_table.
void LIBC_NAMESPACE::internal::child_table_fork_reinit() {
  LIBC_NAMESPACE::process::fork_reinit_child_table();
}

LIBC_REGISTER_FORK_REINIT(child_table,
                          ::LIBC_NAMESPACE::internal::kForkPrioChildTable,
                          &::LIBC_NAMESPACE::internal::child_table_fork_reinit)
