//===-- Explicit ordered subsystem initialization for c.dll ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __libc_dll_init() — called by c.dll's _DllMainCRTStartup during
// DLL_PROCESS_ATTACH. Replaces the former .CRT$XI* section-walked
// function-pointer dispatch with explicit, source-visible function calls.
//
// All init ordering is visible in this single function. Dependencies are
// documented by phase. No function-pointer arrays, no indirect dispatch,
// no section letter fragility.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/stdlib/windows/posix_alloc.h"

extern "C" int __libc_dll_init() {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::internal;

  // =====================================================================
  // Hard floor: Windows 11 23H2 (build 22631) minimum.
  //
  // KUSER_SHARED_DATA.NtBuildNumber is kernel-mapped read-only at
  // 0x7FFE0000 — cannot be shimmed by application manifests.
  // If the build is below the minimum, terminate immediately. No
  // fallbacks, no graceful degradation — the syscall surface is
  // architecturally incompatible below this floor.
  // =====================================================================
  {
    uint32_t build = windows_util::shared_user_data()->NtBuildNumber;
    if (build < nt_version::WIN11_23H2) {
      // STATUS_NOT_SUPPORTED (0xC00000BB) — chosen over
      // STATUS_REVISION_MISMATCH because the problem is "this OS is
      // too old" not "wrong version of a specific protocol".
      ::NtTerminateProcess(NtCurrentProcess(),
                           static_cast<NTSTATUS>(0xC00000BBL));
      __builtin_unreachable();
    }
  }

  // =====================================================================
  // Phase 0: PCB constants + NT capability probe
  //   page_size, security_cookie, module_handle, canary, pid, umask,
  //   nt_build, capabilities, optional syscall dispatch table.
  //   Must be first — everything else depends on page_size.
  // =====================================================================
  if (pcb_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 1: Exception handling infrastructure
  //   VEH reentry TLS slot, then master VEH handler + DLL-load re-front.
  // =====================================================================
  if (veh_reentry_guard_startup_init() != 0)
    return 1;
  if (veh_core_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 2: Process identity
  //   uid/gid/privilege from process token, parent_pid into PCB Zone 0.
  //   Depends on VEH being online (access violations during token queries).
  // =====================================================================
  if (identity_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 3: Allocator + memory subsystem
  //   posix_alloc_init needs page_size. mmap_subsystem_init needs the
  //   allocator. Both must complete before any slab pool or malloc use.
  // =====================================================================
  posix_alloc_init();
  windows::mmap_subsystem_init();

  // =====================================================================
  // Phase 4: Object pools
  //   All slab pool inits. No inter-pool dependencies.
  //   Each reserves VA and allocates TLS slots.
  // =====================================================================
  if (lifecycle_startup_init() != 0)
    return 1;
  if (ofd_pool_startup_init() != 0)
    return 1;
  if (file_pool_startup_init() != 0)
    return 1;
  if (wait_slot_startup_init() != 0)
    return 1;
  if (named_semaphore_startup_init() != 0)
    return 1;
  if (robust_pool_startup_init() != 0)
    return 1;
  if (thread_ring_startup_init() != 0)
    return 1;
  if (thread_storage_startup_init() != 0)
    return 1;
  if (brk_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 5: VEH fault handlers
  //   mmap demand-commit and mlock onfault filters.
  //   Depends on VEH core (Phase 1) and pools (Phase 4).
  // =====================================================================
  if (mem_fault_startup_init() != 0)
    return 1;
  if (mlock_policy_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 6: Fd table
  //   Chunk 0 commit. Depends on pools + VEH fault handlers.
  // =====================================================================
  if (fd_table_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 7: Services
  //   Reactor creates IOCP + drain thread. Signal inits ALPC port
  //   (reactor-driven, no additional thread).
  //   NtCreateThreadEx uses SKIP_THREAD_ATTACH to avoid loader deadlock.
  // =====================================================================
  if (reactor_startup_init() != 0)
    return 1;
  if (signal_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 8: Stdio + console
  //   Bind stdin/stdout/stderr from PEB handles.
  //   Depends on signal being live (SIGHUP delivery for console hangup).
  // =====================================================================
  if (fd_table_std_fds_startup_init() != 0)
    return 1;
  if (vt_pty_startup_init() != 0)
    return 1;
  if (console_startup_init() != 0)
    return 1;

  // =====================================================================
  // Phase 9: Seal PCB Zone 0
  //   Canary validation + PAGE_READONLY on Zone 0. After this point,
  //   writes to page_size/cookie/pid/parent_pid trigger access violations.
  // =====================================================================
  if (!pcb_check_canary())
    return 1;
  // Non-fatal on seal failure: constants are correct, just not
  // hardware-write-protected. Handles edge cases like AV software
  // hooking NtProtectVirtualMemory. [[nodiscard]] acknowledged.
  [[maybe_unused]] bool sealed = pcb_seal_readonly();

  return 0;
}

// ---------------------------------------------------------------------------
// Fork reinit for VA inventory:
//   1. Reset the CAS spinlock protecting the foreign region table. The
//      table contents (base/end/type entries) are valid in the child
//      (address space clone), but the spinlock may have been held by a
//      DLL notification callback in a non-surviving parent thread.
//   2. Re-register the DLL load/unload notification. LdrRegisterDllNotification
//      state is per-process loader data that does not survive NtCreateProcessEx.
//      Without re-registration, dlopen/dlclose in the child silently fails to
//      update g_foreign, allowing MAP_FIXED to clobber newly-loaded images.
// ---------------------------------------------------------------------------
void LIBC_NAMESPACE::internal::va_inventory_fork_reinit() {
  LIBC_NAMESPACE::windows::g_foreign.write_lock.store(
      0, LIBC_NAMESPACE::cpp::MemoryOrder::RELEASE);
  LIBC_NAMESPACE::windows::va_inventory_fork_reinit_dll_notify();
}
