//===-- PCB + AppProperties early init ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// pcb_startup_init(): the first subsystem init called by __libc_dll_init().
// Populates the Process Control Block (PCB) Zone 0 read-only constants
// (page size, allocation granularity, VA bounds, module handle, security
// cookie, zone canary) and the default umask. Then backfills AppProperties
// for cross-platform compat.
//
// After all subsystem inits complete, __libc_dll_init() seals Zone 0 as
// PAGE_READONLY.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "config/windows/app.h"
#include "src/__support/OSUtil/windows/alloc/legacy/va_substrate.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/macros/config.h"

// c.dll's DSO handle — set by _DllMainCRTStartup before __libc_dll_init().
extern "C" void *__dso_handle;

namespace LIBC_NAMESPACE_DECL {

__LIBC_SELECTANY_ATTR AppProperties app = {
    0,            // page_size — set by pcb_startup_init()
    0,            // alloc_granularity — set by pcb_startup_init()
    nullptr,      // module_handle — set by pcb_startup_init()
    internal::TLS_OUT_OF_INDEXES, // tls_atexit_index (unused)
    nullptr,      // dll_notify_cookie
    0,            // argc
    nullptr,      // argv
    nullptr,      // env_ptr
};

} // namespace LIBC_NAMESPACE_DECL

// Phase 0 startup init. Populates PCB Zone 0 and AppProperties.
// Called by __libc_dll_init() before all other subsystem inits.
int LIBC_NAMESPACE::internal::pcb_startup_init() {
  using namespace LIBC_NAMESPACE;

  // --- Zone 0: read-only constants (via PcbInitAccess write gate) ---

  using InitAccess = internal::PcbInitAccess;

  // System info: page size, allocation granularity, VA range bounds.
  SYSTEM_BASIC_INFORMATION sbi;
  if (NT_SUCCESS(::NtQuerySystemInformation(SystemBasicInformation, &sbi,
                                            sizeof(sbi), nullptr))) {
    InitAccess::set_page_size(static_cast<uint32_t>(sbi.PageSize));
    InitAccess::set_alloc_granularity(
        static_cast<uint32_t>(sbi.AllocationGranularity));
    InitAccess::set_min_address(
        reinterpret_cast<void *>(sbi.MinimumUserModeAddress));
    InitAccess::set_max_address(
        reinterpret_cast<void *>(sbi.MaximumUserModeAddress));
  } else {
    // Fallback: x64 defaults. Only reached if ntdll is catastrophically broken.
    InitAccess::set_page_size(4096);
    InitAccess::set_alloc_granularity(65536);
    InitAccess::set_min_address(
        reinterpret_cast<void *>(static_cast<uintptr_t>(0x10000)));
    InitAccess::set_max_address(
        reinterpret_cast<void *>(static_cast<uintptr_t>(0x7FFFFFFEFFFFULL)));
  }

  // Module identity. These are two distinct concepts that were previously
  // conflated:
  //   - module_handle: the PE image base of whatever module statically
  //     contains this libc code. Used by section_registry walkers to read
  //     IMAGE_SECTION_HEADER characteristics, so it MUST be a real PE base.
  //   - dso_handle: the opaque per-DSO identifier for __cxa_atexit /
  //     __cxa_finalize. Per the Itanium C++ ABI its value is unspecified;
  //     glibc convention is &__dso_handle for the main executable.
  //
  // For c.dll these happen to coincide (_DllMainCRTStartup assigns
  // __dso_handle = hinstDLL, which on Windows equals the DLL's image
  // base). For hermetically-linked EXEs they do NOT: do_start.cpp
  // initialises __dso_handle to &__dso_handle (a .data address), which
  // is POSIX-correct but would crash verify_section_readonly()'s DOS
  // signature check. Recover the real image base by asking the loader
  // which module contains a PC inside this very function.
  {
    PVOID image_base = nullptr;
    ::RtlPcToFileHeader(reinterpret_cast<PVOID>(&pcb_startup_init),
                        &image_base);
    InitAccess::set_module_handle(image_base);
  }
  InitAccess::set_dso_handle(__dso_handle);

  // Security cookie: CSPRNG via ProcessPrng (bcryptprimitives.dll).
  // Always succeeds on Windows 10 20H2+ — no error path needed.
  uintptr_t cookie;
  ::ProcessPrng(reinterpret_cast<unsigned char *>(&cookie), sizeof(cookie));
  InitAccess::set_security_cookie(cookie);
  InitAccess::set_security_cookie_complement(~cookie);

  // Process identity: PID from TEB (single instruction, no syscall).
  // parent_pid is set by identity_startup_init() (Phase 2) which queries
  // NtQueryInformationProcess(ProcessBasicInformation).
  InitAccess::set_pid(static_cast<pid_t>(NtCurrentProcessId()));

  // Session id: read once from PEB+0x2C0 (populated by the kernel at
  // process creation). Sealed with Zone 0 so session-scoped NT object
  // paths ("\Sessions\<id>\BaseNamedObjects\...") cannot be redirected
  // by a corrupted PEB.
  InitAccess::set_session_id(
      static_cast<uint32_t>(NtCurrentPeb()->SessionId));

  // Zone canary: placed between read-only constants and mutable state.
  InitAccess::init_canary();

  // --- NT capability probe ---
  // Read the unshimmed build number once from KUSER_SHARED_DATA, then
  // pre-initialize the optional syscall table with fallbacks and probe
  // for real ntdll exports. The table lives in Zone 0 and will be
  // sealed PAGE_READONLY with the rest of the page.
  {
    // KUSER_SHARED_DATA already exposes the real build number here.
    // Do not mask it down: modern Windows 11 builds exceed 14 bits.
    uint32_t build = windows_util::shared_user_data()->NtBuildNumber;
    InitAccess::set_nt_build(build);

    NtOptionalSyscalls &opt = InitAccess::optional_mut();
    opt.alert_thread_ex = nt_fallback::alert_thread_by_thread_id_ex;
    opt.alert_multiple = nt_fallback::alert_multiple_by_thread_id;

    uint32_t caps = 0;
    probe_nt_capabilities(build, opt, caps);
    InitAccess::set_capabilities(caps);
  }

  // Substrate pre_init() used to fire from here. It now runs from the
  // VaSubstrate `.libcmem` handler at Tier A Phase 0c.5, where the
  // walker also batch-registers every seed arena as LIBC_INTERNAL into
  // the mapping table and stamps pre-existing VA via
  // va_inventory_startup_discover. See memory_primitives_bootstrap.h.

  // --- Zone 1: mutable defaults ---

  // Default umask 022 (octal). Must be set before any file operations
  // in later __libc_dll_init() phases. The PCB is zero-initialized by the
  // PE loader, so without this, umask would be 0 (no masking).
  // Relaxed store is safe: CRT init is single-threaded (loader lock held).
  g_pcb.identity.umask.store(0022, cpp::MemoryOrder::RELAXED);

  // --- AppProperties backfill (cross-platform compatibility) ---
  // app.module_handle gets the EXE's base address (not c.dll's) because
  // cross-platform code expects the main executable's HINSTANCE.
  app.page_size = g_pcb.zone0.page_size();
  app.alloc_granularity = g_pcb.zone0.alloc_granularity();
  app.module_handle =
      static_cast<HMODULE>(NtCurrentPeb()->ImageBaseAddress);
  app.tls_atexit_index = internal::TLS_OUT_OF_INDEXES;

  return 0;
}
