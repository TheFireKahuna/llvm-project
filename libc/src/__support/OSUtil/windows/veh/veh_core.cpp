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
// All filters are statically registered into `.libcveh$M` via
// LIBC_REGISTER_VEH_FILTER and inserted into the dispatch table by
// register_all_static_veh_filters() during Tier A. After Tier A, the filter
// table lives in sealed Zone 0a memory — no mutation is possible (or needed),
// so the master handler reads it without synchronization.
//
// Init order (called from __libc_bootstrap()):
//   Phase 0a  veh_reentry_guard_startup_init  -- TEB TLS slot for reentry guard
//   Phase 0b  veh_core_startup_init           -- master handler + DLL notify
//   Phase 0d  register_all_static_veh_filters -- .libcveh sweep
//
// Teardown (from __libc_dll_fini() via .libcfin sweep, or exec_ops):
//   veh_core_fini                             -- remove handler + DLL notify
//
// Fork reinit (from libc_fork_reinit()):
//   veh_core_fork_reinit                      -- re-register handler + notify
//                                                (filter table inherited via COW)
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/dlfcn/r_debug.h"
#include "src/__support/OSUtil/windows/veh/fault_guard.h"
#include "src/__support/OSUtil/windows/veh/veh_reentry_guard.h"
#include "src/__support/OSUtil/windows/lazy_init_reset.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/module_block.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/section_registry.h"
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
// Sealed / mutable VEH state accessors
//
// Sealed half lives in PcbZone0 (read-only after Tier A); mutable half in
// PcbZone1. The accessors here keep call sites short and document the
// contract: read sealed via const ref, write sealed only through PcbInit-
// Access (and only during Tier A before pcb_seal_readonly_a()).
// ---------------------------------------------------------------------------

LIBC_INLINE static const VehSealedState &veh_sealed() {
  return g_pcb.zone0.veh_sealed();
}

LIBC_INLINE static VehMutableState &veh_mutable() {
  return g_pcb.veh_mutable;
}

bool insert_static_veh_filter(const VehFilter &filter) {
  // Single-threaded use only — called from the Tier A `.libcveh` sweep
  // before Zone 0 seal. No lock needed: bootstrap is single-threaded.
  auto &sealed = internal::PcbInitAccess::veh_sealed_mut();

  if (sealed.filter_count >= VEH_MAX_FILTERS)
    return false;

  // Find insertion point (sorted by priority, lower = earlier).
  int pos = sealed.filter_count;
  for (int i = 0; i < sealed.filter_count; ++i) {
    if (filter.priority < sealed.filters[i].priority) {
      pos = i;
      break;
    }
  }

  // Shift elements right to make room.
  for (int i = sealed.filter_count; i > pos; --i)
    sealed.filters[i] = sealed.filters[i - 1];

  sealed.filters[pos] = filter;
  ++sealed.filter_count;
  return true;
}

// ---------------------------------------------------------------------------
// Master VEH handler
// ---------------------------------------------------------------------------

NTAPI static LONG master_veh_handler(EXCEPTION_POINTERS *ep) {
  if (!ep || !ep->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;

  const auto &sealed = veh_sealed();

  // ── Fault guard (pre-reentry-guard) ──────────────────────────────
  // Thread-local escape hatch for libc page probes and foreign dtor calls.
  // Checked before the reentry guard because longjmp would skip its RAII
  // destructor and permanently set the reentry flag on this thread.
  // This path is pure TLS read + mask check — no callbacks, no allocations,
  // nothing that can recursively fault.
  FaultGuard *fg = get_fault_guard(sealed.fault_guard_tls_index);
  if (fg) {
    uint32_t bit = exception_code_to_bit(ep->ExceptionRecord->ExceptionCode);
    if (bit & fg->exception_mask) {
      fg->exception_code = ep->ExceptionRecord->ExceptionCode;
      set_fault_guard(sealed.fault_guard_tls_index, fg->prev);
      LIBC_NAMESPACE::longjmp(fg->buf, 1);
    }
  }

  // ── Debugger transparency ─────────────────────────────────────────
  // When a debugger is attached (PEB.BeingDebugged), pass breakpoint and
  // single-step exceptions through to the debugger instead of routing
  // them to POSIX signal dispatch. Without this, attaching lldb/WinDbg
  // to an NT-POSIX process would fight with VEH over these exceptions.
  // This check is cheap (single byte read from PEB, always resident).
  if (NtCurrentPeb()->BeingDebugged) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP)
      return EXCEPTION_CONTINUE_SEARCH;
  }

  // ── Reentry guard ────────────────────────────────────────────────
  // If we're already inside the master handler on this thread (e.g., a
  // fault in a filter callback), bail to the OS crash handler. This
  // produces a clean minidump instead of infinite recursion.
  internal::VehReentryGuard guard(sealed.reentry_tls_index);
  if (guard.is_reentry())
    return EXCEPTION_CONTINUE_SEARCH;

  // Map exception code to bitmask position. Unrecognized codes (language
  // runtime exceptions, C++/CLR, etc.) get 0 --- no filter matches.
  uint32_t bit = exception_code_to_bit(ep->ExceptionRecord->ExceptionCode);
  if (bit == 0)
    return EXCEPTION_CONTINUE_SEARCH;

  // Walk the priority-sorted filter table. The table is in sealed memory
  // (Zone 0, PAGE_READONLY post-Tier-A) — no synchronization needed.
  // Filter handlers are responsible for their own runtime gating
  // (e.g. signal_veh_transport returns CONTINUE_SEARCH when no SEH-class
  // signal handler is installed). Filters that hand control to user code
  // (signal_veh_transport's dispatch_pending invocation) clear the
  // reentry flag themselves before that call — the RAII destructor here
  // does not run on a non-local exit (siglongjmp from a user handler).
  for (int i = 0; i < sealed.filter_count; ++i) {
    if (sealed.filters[i].exception_mask & bit) {
      LONG result = sealed.filters[i].handler(ep);
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
#include "startup/windows/dll_unload_cxa_finalize.h"

NTAPI static void dll_notify_callback(
    ULONG reason, const LDR_DLL_NOTIFICATION_DATA *data, void *) {
  if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
    // DLL load: re-add our master VEH at front-of-chain. The newly loaded
    // DLL may have registered its own VEH that pushed us off front; the
    // add-then-remove sequence keeps us in the chain throughout.

    // Step 1: Add new entry at front.
    void *new_h = ::RtlAddVectoredExceptionHandler(1, master_veh_handler);
    if (!new_h)
      return;

    // Step 2: Swap handles atomically.
    void *old_h = veh_mutable().handler_handle.exchange(
        new_h, cpp::MemoryOrder::ACQ_REL);

    // Step 3: Remove old entry. Between steps 1 and 3, both entries exist.
    if (old_h)
      ::RtlRemoveVectoredExceptionHandler(old_h);

    // Update the GDB/LLDB rendezvous link_map list with the new module.
    // Runs under loader lock (notification invariant) — safe to walk
    // PEB->Ldr and parse the new image's PE headers.
    ::LIBC_NAMESPACE::internal::rdebug_on_dll_load(data->Loaded.DllBase);
    return;
  }

  if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    // Shutdown gate — during ExitProcess → LdrpShutdownProcess, DLLs
    // unmap in reverse-dep order. DLLs unloaded BEFORE c.dll still
    // fire this callback with our libc subsystems fully alive, so
    // running cxa_finalize for them would be well-defined — but any
    // dtor that touches subsystem state is racing with the shutdown
    // walker itself (other threads may be executing arbitrary ntdll
    // code). Skipping reconciliation here matches what glibc does on
    // Linux shutdown: atexit entries registered against a DSO whose
    // unmap is driven by the OS terminator are dropped, not run. The
    // loader reclaims all image memory anyway; dtors that would have
    // touched freed libc state are safer skipped than executed.
    PEB *peb = NtCurrentPeb();
    if (peb->Ldr && peb->Ldr->ShutdownInProgress)
      return;

    // Drop the module from the GDB/LLDB rendezvous list before the
    // cxa_finalize sweep.  User dtors that touch cross-module state
    // observe a list consistent with "this module is gone"; a debugger
    // attached during unload sees the RT_DELETE transition complete
    // before any dtor code runs.
    ::LIBC_NAMESPACE::internal::rdebug_on_dll_unload(data->Unloaded.DllBase);

    // DLL unload: optionally sweep per-image .libcfin / .libclzr via
    // the DLL's __libc_module_block export, then run __cxa_finalize
    // for any DSO that lacks its own _DllMainCRTStartup. FaultGuard
    // catches misbehaving user dtors so a fault under loader lock
    // doesn't freeze every other thread.
    //
    // LOADER LOCK INVARIANT — this callback runs with the loader lock
    // already held by the unmapping thread. User dtors invoked through
    // __cxa_finalize MUST NOT:
    //   - LoadLibrary/FreeLibrary anything (recursive loader lock acq;
    //     the lock is exclusive — instant deadlock on the next thread
    //     that touches the loader, and a self-recursive load that races
    //     a sibling LdrLoadDll deadlocks immediately).
    //   - Spawn threads that synchronise with their entry (CreateThread
    //     completion runs DLL_THREAD_ATTACH on every loaded DLL — needs
    //     the loader lock the parent already holds).
    //   - Wait on any handle that another thread might signal while
    //     holding (or contending) the loader lock.
    // The libc itself takes no LoadLibrary path during fini, so the
    // constraint is on third-party dtors only.
    FaultGuard g;
    if (!fault_guard_enter(&g, FAULT_GUARD_DTOR)) {
      // Per-image sweep: only fires for DLLs that exported
      // __libc_module_block via LIBC_DEFINE_MODULE_BLOCK. c.dll's own
      // unload never reaches this callback (veh_core_fini unregisters
      // the cookie in .libcfin phase 0 before c.dll unmaps), so c.dll
      // does not adopt the macro and this lookup returns null for it.
      // Resolution is image-scoped via LdrGetProcedureAddress; a miss
      // means the DLL is not libc-linked and we skip the sweep.
      if (const ::LIBC_NAMESPACE::internal::LibcModuleBlock *block =
              ::LIBC_NAMESPACE::internal::resolve_module_block(
                  data->Unloaded.DllBase)) {
        // Order mirrors the main-image teardown path:
        //   1. .libcfin in reverse phase order ($P9..$P0)
        //   2. .libclzr reset thunks (forward; intra-phase order
        //      is undefined and irrelevant for resets)
        // cxa_finalize runs after, so C++ static dtors observe the
        // libc subsystems' torn-down state if they registered late.
        for (const auto &entry :
             ::LIBC_NAMESPACE::internal::SectionRegistry<
                 ::LIBC_NAMESPACE::internal::FiniEntry>(block->libcfin_start,
                                                       block->libcfin_end)
                 .reverse()) {
          if (entry.fini)
            entry.fini();
        }
        for (const auto &entry :
             ::LIBC_NAMESPACE::internal::SectionRegistry<
                 ::LIBC_NAMESPACE::internal::LazyInitResetEntry>(
                 block->libclzr_start, block->libclzr_end)) {
          if (entry.reset)
            entry.reset();
        }
      }
      __libc_dll_unload_cxa_finalize(data);
      fault_guard_leave(&g);
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle: init, fini, fork reinit
// ---------------------------------------------------------------------------

// Phase 0b init — runs from __libc_bootstrap() before pcb_startup_init.
// Allocates the fault-guard TLS slot directly into Zone 0 sealed state via
// PcbInitAccess (Zone 0 is unsealed at this point). Registers the master
// VEH and a single combined dll-load/unload notification.
static void veh_core_init_impl() {
  auto &sealed_mut = internal::PcbInitAccess::veh_sealed_mut();

  // Allocate TEB TLS slot for fault guard chain head. Goes straight into
  // sealed memory (still writable — pcb_seal_readonly_a happens later in
  // Tier A after the .libcveh sweep).
  sealed_mut.fault_guard_tls_index = internal::tls_alloc();

  // Register the single master VEH handler at front (priority 1).
  void *h = ::RtlAddVectoredExceptionHandler(1, master_veh_handler);
  veh_mutable().handler_handle.store(h, cpp::MemoryOrder::RELEASE);

  // Single LdrRegisterDllNotification covering BOTH load (re-add VEH at
  // front) and unload (run __cxa_finalize for foreign DSOs). The cookie
  // lives in Zone 0b — set once here while Zone 0b is still writable,
  // then sealed alongside pid/cookies at end of Tier A.
  if (!g_pcb.zone0b.dll_notify_cookie()) {
    PVOID cookie = nullptr;
    if (NT_SUCCESS(::LdrRegisterDllNotification(0, dll_notify_callback,
                                                nullptr, &cookie)))
      internal::PcbInitAccess::set_dll_notify_cookie(cookie);
  }

  // Seed the GDB/LLDB rendezvous link_map list from the current PEB.
  // Tier A is single-threaded so no concurrent DLL load can race this
  // walk; any subsequent LOAD flows through dll_notify_callback which
  // dedups via find_node_by_base.
  ::LIBC_NAMESPACE::internal::rdebug_startup_init();
}

void veh_core_fini() {
  // Unregister the combined dll-load/unload notification under a single
  // Zone 0b unseal window. Order matters: unseal first, then unregister
  // + clear the field as a paired operation. The prior order (unregister
  // before unseal) had a failure mode where AV/EDR hooking of
  // NtProtectVirtualMemory could leave a stale cookie in PCB pointing at
  // kernel state ntdll had already released — a subsequent double fini
  // or debugger read would observe the dangling pointer.
  //
  // If unseal fails we leak the kernel cookie rather than touch ntdll
  // with a cookie we can no longer null out. The process is shutting
  // down anyway.
  if (g_pcb.zone0b.dll_notify_cookie()) {
    if (pcb_unseal_readonly_b()) {
      void *cookie = g_pcb.zone0b.dll_notify_cookie();
      if (cookie) {
        ::LdrUnregisterDllNotification(cookie);
        internal::PcbInitAccess::set_dll_notify_cookie(nullptr);
      }
      // Fail-closed reseal. If we cannot put Zone 0b back to PAGE_READONLY
      // before returning, the host process resumes after FreeLibrary with
      // a window of writable PCB state until the loader unmaps c.dll.
      // That window is small but real, and a host that proceeds through
      // it has degraded protection without any libc-level signal that
      // the contract was broken. Treat reseal failure the same way we
      // treat seal failure during Tier A: terminate. Costs the host
      // process; the alternative is a silent posture downgrade in the
      // last few microseconds before unmap, which is precisely what the
      // seal contract forbids.
      if (!pcb_seal_readonly_b())
        ::NtTerminateProcess(NtCurrentProcess(), 127);
    }
  }

  // Remove the master VEH handler.
  void *h =
      veh_mutable().handler_handle.exchange(nullptr, cpp::MemoryOrder::ACQ_REL);
  if (h)
    ::RtlRemoveVectoredExceptionHandler(h);

  // Tear down the GDB/LLDB rendezvous list.  Runs after the DLL
  // notification is unregistered so no concurrent callback can race
  // the teardown; a debugger that attaches in the narrow window
  // before c.dll unmaps sees an empty, consistent list.
  ::LIBC_NAMESPACE::internal::rdebug_fini();

  // The fault_guard TLS slot lives in sealed Zone 0 memory — we cannot
  // free it (Zone 0 is sealed for life). The PEB TlsBitmap bit is leaked
  // on FreeLibrary, but FreeLibrary→LoadLibrary of c.dll is rejected by
  // the poisoned init gate so this is a one-shot leak at process exit.
}

// Fork-child reinit. Called from libc_fork_reinit() WHILE Zone 0b is
// unsealed (the caller's existing unseal window for pid/cookie writes).
// Updates dll_notify_cookie and re-acquires the kernel VEH handle.
//
// Filter table, TLS indices: inherited via COW from the parent's sealed
// Zone 0 page. The PEB TlsBitmap is COW'd so the indices remain allocated;
// the child's fresh TEB starts with zeroed slots which is the correct
// initial chain head state. No reallocation needed.
static void veh_core_fork_reinit_impl() {
  // Re-register the master handler. The previous handle was bound to the
  // parent's kernel VEH chain and does not survive RtlCloneUserProcess;
  // overwrite the slot directly with the new one.
  void *h = ::RtlAddVectoredExceptionHandler(1, master_veh_handler);
  veh_mutable().handler_handle.store(h, cpp::MemoryOrder::RELEASE);

  // Re-acquire the dll-notify cookie. Zone 0b is already unsealed by the
  // caller (libc_fork_reinit runs the cookie reseed and PID rewrite under
  // the same window).
  PVOID cookie = nullptr;
  if (NT_SUCCESS(::LdrRegisterDllNotification(0, dll_notify_callback,
                                              nullptr, &cookie)))
    internal::PcbInitAccess::set_dll_notify_cookie(cookie);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

// ---------------------------------------------------------------------------
// Explicit startup / fini / fork-reinit entries
// Called by __libc_bootstrap(), __libc_dll_fini(), libc_fork_reinit().
// ---------------------------------------------------------------------------

int LIBC_NAMESPACE::internal::veh_core_startup_init() {
  LIBC_NAMESPACE::windows::veh_core_init_impl();
  return 0;
}

void LIBC_NAMESPACE::internal::veh_core_fork_reinit() {
  LIBC_NAMESPACE::windows::veh_core_fork_reinit_impl();
}

LIBC_REGISTER_FINI(0, veh_core, &::LIBC_NAMESPACE::windows::veh_core_fini)

LIBC_REGISTER_FORK_REINIT(veh_core,
                          ::LIBC_NAMESPACE::internal::kForkPrioVehCore,
                          &::LIBC_NAMESPACE::internal::veh_core_fork_reinit)
