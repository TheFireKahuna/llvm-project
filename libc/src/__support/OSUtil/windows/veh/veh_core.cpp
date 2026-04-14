//===-- Unified VEH dispatch framework implementation --------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single process-global VEH handler with priority-sorted dispatch table.
//
// Init order (explicit calls from __libc_dll_init()):
//   Phase 1: veh_reentry_guard_startup_init()  -- allocate TLS slot
//            veh_core_startup_init()            -- register master handler + DLL notify
//   Phase 5: subsystem filter registration via register_veh_filter()
//
// Teardown (explicit call from __libc_dll_fini() or exec_ops):
//   veh_core_startup_fini()           -- remove master handler + DLL notify
//
// Fork reinit (explicit call from libc_fork_reinit()):
//   veh_core_fork_reinit()            -- re-register (kernel state is stale)
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/OSUtil/windows/veh/veh_reentry_guard.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/config.h"
#include "src/setjmp/longjmp.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// ---------------------------------------------------------------------------
// Exception code -> bitmask mapping
// ---------------------------------------------------------------------------

/// Map an NTSTATUS exception code to a VehExceptionBit. Returns 0 for
/// unrecognized codes (no filter will match).
static uint32_t exception_code_to_bit(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:     return VEH_ACCESS_VIOLATION;
  case EXCEPTION_GUARD_PAGE:           return VEH_GUARD_PAGE;
  case EXCEPTION_INT_DIVIDE_BY_ZERO:   return VEH_INT_DIVIDE_BY_ZERO;
  case EXCEPTION_INT_OVERFLOW:         return VEH_INT_OVERFLOW;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:   return VEH_FLT_DIVIDE_BY_ZERO;
  case EXCEPTION_FLT_OVERFLOW:         return VEH_FLT_OVERFLOW;
  case EXCEPTION_FLT_UNDERFLOW:        return VEH_FLT_UNDERFLOW;
  case EXCEPTION_FLT_INEXACT_RESULT:   return VEH_FLT_INEXACT;
  case EXCEPTION_FLT_INVALID_OPERATION:return VEH_FLT_INVALID_OP;
  case EXCEPTION_FLT_DENORMAL_OPERAND: return VEH_FLT_DENORMAL;
  case EXCEPTION_FLT_STACK_CHECK:      return VEH_FLT_STACK_CHECK;
  case EXCEPTION_ILLEGAL_INSTRUCTION:  return VEH_ILLEGAL_INSTRUCTION;
  case EXCEPTION_PRIV_INSTRUCTION:     return VEH_PRIV_INSTRUCTION;
  case EXCEPTION_BREAKPOINT:           return VEH_BREAKPOINT;
  case EXCEPTION_SINGLE_STEP:          return VEH_SINGLE_STEP;
  case EXCEPTION_DATATYPE_MISALIGNMENT:return VEH_DATATYPE_MISALIGN;
  case EXCEPTION_IN_PAGE_ERROR:        return VEH_IN_PAGE_ERROR;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:return VEH_ARRAY_BOUNDS;
  case EXCEPTION_STACK_OVERFLOW:       return VEH_STACK_OVERFLOW;
  default:                             return 0;
  }
}

// ---------------------------------------------------------------------------
// Dispatch table
// ---------------------------------------------------------------------------

static VehState &veh_state() { return g_pcb.veh; }

static void lock_filters(VehState &state) {
  for (;;) {
    FutexValueType expected = 0;
    if (state.filter_lock.compare_exchange_weak(expected, 1,
                                                cpp::MemoryOrder::ACQUIRE,
                                                cpp::MemoryOrder::RELAXED))
      return;
    state.filter_lock.wait(1);
  }
}

static void unlock_filters(VehState &state) {
  state.filter_lock.store_and_notify(0);
}

bool register_veh_filter(const VehFilter &filter) {
  auto &state = veh_state();
  lock_filters(state);

  int count = state.filter_count.load(cpp::MemoryOrder::RELAXED);
  if (count >= VEH_MAX_FILTERS) {
    unlock_filters(state);
    return false;
  }

  // Find insertion point (sorted by priority, lower = earlier).
  int pos = count;
  for (int i = 0; i < count; ++i) {
    if (filter.priority < state.filters[i].priority) {
      pos = i;
      break;
    }
  }

  // Shift elements right to make room.
  for (int i = count; i > pos; --i)
    state.filters[i] = state.filters[i - 1];

  state.filters[pos] = filter;

  // Release-store publishes the new entry to lock-free readers.
  state.filter_count.store(count + 1, cpp::MemoryOrder::RELEASE);

  unlock_filters(state);
  return true;
}

bool unregister_veh_filter(LONG (*handler)(EXCEPTION_POINTERS *)) {
  auto &state = veh_state();
  lock_filters(state);

  int count = state.filter_count.load(cpp::MemoryOrder::RELAXED);
  int found = -1;
  for (int i = 0; i < count; ++i) {
    if (state.filters[i].handler == handler) {
      found = i;
      break;
    }
  }

  if (found < 0) {
    unlock_filters(state);
    return false;
  }

  // Shift elements left to fill the gap.
  for (int i = found; i < count - 1; ++i)
    state.filters[i] = state.filters[i + 1];

  // Release-store: readers loading the decremented count see a consistent
  // prefix. A concurrent reader that loaded the old count may call one
  // extra filter (the duplicated tail entry), which is safe --- filters
  // are idempotent for non-matching exceptions.
  state.filter_count.store(count - 1, cpp::MemoryOrder::RELEASE);

  unlock_filters(state);
  return true;
}

// ---------------------------------------------------------------------------
// Master VEH handler
// ---------------------------------------------------------------------------

static LONG NTAPI master_veh_handler(EXCEPTION_POINTERS *ep) {
  if (!ep || !ep->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;

  auto &state = veh_state();

  // ── Fault guard (pre-reentry-guard) ──────────────────────────────
  // Thread-local escape hatch for libc page probes and foreign dtor calls.
  // Checked before the reentry guard because longjmp would skip its RAII
  // destructor and permanently set the reentry flag on this thread.
  // This path is pure TLS read + mask check — no callbacks, no allocations,
  // nothing that can recursively fault.
  FaultGuard *fg = get_fault_guard(state.fault_guard_tls_index);
  if (fg) {
    uint32_t bit = exception_code_to_bit(ep->ExceptionRecord->ExceptionCode);
    if (bit & fg->exception_mask) {
      fg->exception_code = ep->ExceptionRecord->ExceptionCode;
      set_fault_guard(state.fault_guard_tls_index, fg->prev);
      LIBC_NAMESPACE::longjmp(fg->buf, 1);
    }
  }

  // ── Reentry guard ────────────────────────────────────────────────
  // If we're already inside the master handler on this thread (e.g., a
  // fault in a filter callback), bail to the OS crash handler. This
  // produces a clean minidump instead of infinite recursion.
  internal::VehReentryGuard guard(state.reentry_tls_index);
  if (guard.is_reentry())
    return EXCEPTION_CONTINUE_SEARCH;

  // Map exception code to bitmask position. Unrecognized codes (language
  // runtime exceptions, C++/CLR, etc.) get 0 --- no filter matches.
  uint32_t bit = exception_code_to_bit(ep->ExceptionRecord->ExceptionCode);
  if (bit == 0)
    return EXCEPTION_CONTINUE_SEARCH;

  // Walk the priority-sorted filter table. First matching filter that
  // returns non-CONTINUE_SEARCH wins.
  int count = state.filter_count.load(cpp::MemoryOrder::ACQUIRE);
  for (int i = 0; i < count; ++i) {
    if (state.filters[i].exception_mask & bit) {
      LONG result = state.filters[i].handler(ep);
      if (result != EXCEPTION_CONTINUE_SEARCH)
        return result;
    }
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// VEH handle and DLL-load notification
// ---------------------------------------------------------------------------

/// DLL-load callback. Uses add-then-remove ordering to maintain front-of-chain
/// position without ever being absent from the chain.
///
/// Between the add and remove, the same function pointer is in the chain twice.
/// RtlAddVectoredExceptionHandler creates a new list node per call regardless
/// of function pointer identity, so duplicate registration is safe. The
/// reentry guard prevents double dispatch during the overlap window.
static void NTAPI dll_load_callback(
    ULONG reason, const LDR_DLL_NOTIFICATION_DATA *, void *) {
  if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED)
    return;

  auto &state = veh_state();

  // Step 1: Add new entry at front.
  void *new_h = ::RtlAddVectoredExceptionHandler(1, master_veh_handler);
  if (!new_h)
    return;

  // Step 2: Swap handles atomically.
  void *old_h =
      state.handler_handle.exchange(new_h, cpp::MemoryOrder::ACQ_REL);

  // Step 3: Remove old entry. Between steps 1 and 3, both entries exist.
  if (old_h)
    ::RtlRemoveVectoredExceptionHandler(old_h);
}

// ---------------------------------------------------------------------------
// Lifecycle: init, fini, fork reinit
// ---------------------------------------------------------------------------

static void veh_core_init_impl() {
  auto &state = veh_state();

  // Allocate TEB TLS slot for fault guard chain head.
  state.fault_guard_tls_index = internal::tls_alloc();

  // Register the single master VEH handler at front (priority 1).
  void *h = ::RtlAddVectoredExceptionHandler(1, master_veh_handler);
  state.handler_handle.store(h, cpp::MemoryOrder::RELEASE);

  // Register DLL notification to maintain front position across DLL loads.
  if (!state.dll_notify_cookie) {
    PVOID cookie = nullptr;
    if (NT_SUCCESS(::LdrRegisterDllNotification(0, dll_load_callback,
                                                nullptr, &cookie)))
      state.dll_notify_cookie = cookie;
  }
}

static void veh_core_fini_impl() {
  auto &state = veh_state();

  // Unregister DLL notification first --- no more re-registrations.
  if (state.dll_notify_cookie) {
    ::LdrUnregisterDllNotification(state.dll_notify_cookie);
    state.dll_notify_cookie = nullptr;
  }

  // Remove the master VEH handler.
  void *h =
      state.handler_handle.exchange(nullptr, cpp::MemoryOrder::ACQ_REL);
  if (h)
    ::RtlRemoveVectoredExceptionHandler(h);

  // Free the fault guard TLS slot.
  if (state.fault_guard_tls_index != internal::TLS_OUT_OF_INDEXES) {
    internal::tls_free(state.fault_guard_tls_index);
    state.fault_guard_tls_index = internal::TLS_OUT_OF_INDEXES;
  }
}

static void veh_core_fork_reinit_impl() {
  auto &state = veh_state();

  // Kernel VEH and DLL notification state is stale after NtCreateProcessEx.
  // The filter table in process memory survives fork (function pointers
  // remain valid --- same module at same base). Re-register the master
  // handler and DLL notification.
  state.filter_lock.reset_for_fork(0);
  state.handler_handle.store(nullptr, cpp::MemoryOrder::RELAXED);
  state.dll_notify_cookie = nullptr;

  // Free the stale fault guard TLS bitmap bit from the parent. The PEB
  // bitmap is COW'd across fork, so the parent's bit is still set in the
  // child. Free it before veh_core_init_impl() allocates a fresh slot.
  // (reentry_tls_index is NOT freed — it keeps its parent value and the
  // fresh child TEB slot starts null, which is the correct initial state.)
  if (state.fault_guard_tls_index != internal::TLS_OUT_OF_INDEXES)
    internal::tls_free(state.fault_guard_tls_index);

  veh_core_init_impl();
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

// ---------------------------------------------------------------------------
// Explicit startup / fini / fork-reinit entries
// Called by __libc_dll_init(), __libc_dll_fini(), libc_fork_reinit().
// ---------------------------------------------------------------------------

int LIBC_NAMESPACE::internal::veh_core_startup_init() {
  LIBC_NAMESPACE::windows::veh_core_init_impl();
  return 0;
}

void LIBC_NAMESPACE::internal::veh_core_startup_fini() {
  LIBC_NAMESPACE::windows::veh_core_fini_impl();
}

void LIBC_NAMESPACE::internal::veh_core_fork_reinit() {
  LIBC_NAMESPACE::windows::veh_core_fork_reinit_impl();
}
