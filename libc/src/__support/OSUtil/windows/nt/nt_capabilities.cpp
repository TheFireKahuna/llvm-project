//===-- NT capability probing and fallback implementations ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"

#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// Fallback: NtAlertThreadByThreadIdEx
//
// Two-step: release the SRW lock, then alert the thread. The atomicity
// guarantee of the real syscall (release + alert in one transition) is
// lost, but correctness is preserved because waiters always re-check
// their predicate after waking from NtWaitForAlertByThreadId.
//===----------------------------------------------------------------------===//

NTSTATUS NTAPI
nt_fallback::alert_thread_by_thread_id_ex(HANDLE ThreadId, PVOID Lock) {
  if (Lock)
    ::RtlReleaseSRWLockExclusive(static_cast<PRTL_SRWLOCK>(Lock));
  return ::NtAlertThreadByThreadId(ThreadId);
}

//===----------------------------------------------------------------------===//
// Fallback: NtAlertMultipleThreadByThreadId
//
// Sequential per-thread alerts. O(n) syscalls vs O(1) batch, but each
// thread is individually woken. Returns the last NTSTATUS (or
// STATUS_SUCCESS if count is zero). ExtendedParameters are ignored
// because all existing call sites pass nullptr/0.
//===----------------------------------------------------------------------===//

NTSTATUS NTAPI
nt_fallback::alert_multiple_by_thread_id(HANDLE *ThreadIds, ULONG Count,
                                         PVOID /*ExtendedParameters*/,
                                         ULONG /*ExtendedParameterCount*/) {
  NTSTATUS last = STATUS_SUCCESS;
  for (ULONG i = 0; i < Count; ++i) {
    NTSTATUS st = ::NtAlertThreadByThreadId(ThreadIds[i]);
    if (!NT_SUCCESS(st))
      last = st;
  }
  return last;
}

//===----------------------------------------------------------------------===//
// probe_nt_capabilities
//
// Called once during Phase 0 (pcb_startup_init). Reads the unshimmed
// build number from KUSER_SHARED_DATA, then resolves optional 24H2+
// exports from the already-loaded ntdll.dll via LdrGetProcedureAddress.
//
// Security:
//   - ntdll.dll handle obtained from the loader's own PEB InMemoryOrder
//     list via LdrGetDllHandleByName — no DLL search path involved.
//   - Function pointers are written into PcbZone0, which is sealed
//     PAGE_READONLY after init completes.
//===----------------------------------------------------------------------===//

void probe_nt_capabilities(uint32_t build, NtOptionalSyscalls &table,
                           uint32_t &caps_out) {
  caps_out = 0;

  // --- Resolve ntdll handle ---
  // ntdll.dll is always the first module loaded — this cannot fail in a
  // correctly-running process.
  WCHAR ntdll_path[] = u"ntdll.dll";
  UNICODE_STRING ntdll_name;
  ntdll_name.Buffer = ntdll_path;
  ntdll_name.Length = sizeof(ntdll_path) - sizeof(WCHAR);
  ntdll_name.MaximumLength = sizeof(ntdll_path);

  PVOID ntdll_handle = nullptr;
  NTSTATUS st =
      ::LdrGetDllHandleByName(&ntdll_name, nullptr, &ntdll_handle);
  if (!NT_SUCCESS(st) || !ntdll_handle)
    return; // Cannot resolve — keep all fallbacks.

  // --- Helper: resolve a single export by name ---
  auto resolve = [&](const char *name) -> PVOID {
    ANSI_STRING proc_name;
    ::RtlInitString(&proc_name, name);
    PVOID addr = nullptr;
    NTSTATUS s =
        ::LdrGetProcedureAddress(ntdll_handle, &proc_name, 0, &addr);
    return NT_SUCCESS(s) ? addr : nullptr;
  };

  // --- Probe NtAlertThreadByThreadIdEx (24H2+) ---
  if (PVOID p = resolve("NtAlertThreadByThreadIdEx")) {
    table.alert_thread_ex =
        reinterpret_cast<NtAlertThreadByThreadIdExFn>(p);
    caps_out |= nt_cap::ALERT_THREAD_EX;
  }

  // --- Probe NtAlertMultipleThreadByThreadId (24H2+) ---
  if (PVOID p = resolve("NtAlertMultipleThreadByThreadId")) {
    table.alert_multiple =
        reinterpret_cast<NtAlertMultipleThreadByThreadIdFn>(p);
    caps_out |= nt_cap::ALERT_MULTIPLE;
  }

  // --- IoRing V4 detection (24H2+) ---
  // IoRing V4 adds IORING_OP_READ_SCATTER / IORING_OP_WRITE_GATHER.
  // Detection is build-number based: V4 was introduced in 26100 (24H2).
  // The IoRing API itself (NtQueryIoRingCapabilities etc.) is always
  // available as a hard import since 23H2 is the floor.
  if (build >= nt_version::WIN11_24H2)
    caps_out |= nt_cap::IORING_V4;
}

//===----------------------------------------------------------------------===//
// Runtime accessors
//
// Defined out-of-line so call sites only need nt_capabilities.h, not the
// full process_control_block.h. The compiler will inline through these
// at higher optimization levels.
//===----------------------------------------------------------------------===//

const NtOptionalSyscalls &nt_optional() {
  return g_pcb.zone0.optional();
}

uint32_t nt_caps() { return g_pcb.zone0.capabilities(); }

} // namespace LIBC_NAMESPACE_DECL
