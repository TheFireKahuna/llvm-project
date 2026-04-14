//===-- NT version detection and optional syscall dispatch ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Version constants, capability flags, and a sealed function-pointer dispatch
// table for NT syscalls that are not available on all supported builds.
//
// Design:
//   1. Hard floor: KUSER_SHARED_DATA.NtBuildNumber >= WIN11_23H2.
//      Enforced at the top of __libc_dll_init() before any subsystem init.
//      KUSER_SHARED_DATA is kernel-mapped read-only at 0x7FFE0000 — cannot
//      be shimmed by application compatibility manifests (unlike PEB).
//
//   2. Capability flags: a uint32_t bitmask in PcbZone0. Each bit gates one
//      optional feature (24H2+). Probed once in Phase 0, then sealed
//      PAGE_READONLY with the rest of Zone 0.
//
//   3. Sealed dispatch table: NtOptionalSyscalls holds function pointers
//      for syscalls that may not exist on 23H2. Each pointer is initialized
//      to a correct fallback implementation. At Phase 0, if the real export
//      exists in ntdll.dll, the pointer is overwritten. After init, the
//      entire table is in the sealed PAGE_READONLY page — an attacker
//      cannot redirect them without first calling NtProtectVirtualMemory
//      (which ACG/CFG can gate).
//
//      Call sites do a single indirect call — no null check, no branch.
//      On 24H2+: calls the real ntdll function.
//      On 23H2:  calls the fallback. Same signature, same ABI.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CAPABILITIES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CAPABILITIES_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

//===----------------------------------------------------------------------===//
// Build number constants
//===----------------------------------------------------------------------===//

namespace nt_version {
inline constexpr uint32_t WIN11_RTM  = 22000; // 21H2
inline constexpr uint32_t WIN11_22H2 = 22621;
inline constexpr uint32_t WIN11_23H2 = 22631; // Hard minimum for this libc
inline constexpr uint32_t WIN11_24H2 = 26100;
} // namespace nt_version

//===----------------------------------------------------------------------===//
// Capability bits — each gates one optional 24H2+ feature
//===----------------------------------------------------------------------===//

namespace nt_cap {
inline constexpr uint32_t ALERT_THREAD_EX = 1u << 0; // NtAlertThreadByThreadIdEx
inline constexpr uint32_t ALERT_MULTIPLE  = 1u << 1; // NtAlertMultipleThreadByThreadId
inline constexpr uint32_t IORING_V4       = 1u << 2; // IoRing scatter/gather ops
} // namespace nt_cap

//===----------------------------------------------------------------------===//
// Optional syscall type aliases
//===----------------------------------------------------------------------===//

// NtAlertThreadByThreadIdEx (24H2+) — atomically releases an SRW lock and
// alerts the thread. Lock is optional; nullptr is identical to plain alert.
using NtAlertThreadByThreadIdExFn = NTSTATUS(NTAPI *)(HANDLE ThreadId,
                                                      PVOID Lock);

// NtAlertMultipleThreadByThreadId (24H2+) — batch wake multiple threads
// parked in NtWaitForAlertByThreadId in a single syscall.
using NtAlertMultipleThreadByThreadIdFn = NTSTATUS(NTAPI *)(
    HANDLE *ThreadIds, ULONG Count, PVOID ExtendedParameters,
    ULONG ExtendedParameterCount);

//===----------------------------------------------------------------------===//
// NtOptionalSyscalls — sealed dispatch table
//
// Stored in PcbZone0. Pre-initialized to fallback implementations.
// Overwritten with real ntdll exports if available, then sealed read-only.
// Call sites use a single indirect call — zero branches, zero null checks.
//===----------------------------------------------------------------------===//

struct NtOptionalSyscalls {
  NtAlertThreadByThreadIdExFn alert_thread_ex;
  NtAlertMultipleThreadByThreadIdFn alert_multiple;
};

//===----------------------------------------------------------------------===//
// Fallback implementations
//
// These provide correct (but potentially slower) behavior using syscalls
// that are guaranteed to exist on the 23H2 floor. They are the initial
// values of the dispatch table pointers.
//===----------------------------------------------------------------------===//

namespace nt_fallback {

// Fallback: release lock manually, then alert. Non-atomic (two syscalls
// instead of one), but correct — waiters always re-check their predicate
// after wake, so the window between lock release and alert is harmless.
NTSTATUS NTAPI alert_thread_by_thread_id_ex(HANDLE ThreadId, PVOID Lock);

// Fallback: sequential per-thread alerts. O(n) syscalls instead of O(1),
// but correct — each thread is individually woken.
NTSTATUS NTAPI alert_multiple_by_thread_id(HANDLE *ThreadIds, ULONG Count,
                                           PVOID ExtendedParameters,
                                           ULONG ExtendedParameterCount);

} // namespace nt_fallback

//===----------------------------------------------------------------------===//
// Probe function — called once in Phase 0
//===----------------------------------------------------------------------===//

// Resolves optional syscalls from ntdll.dll via LdrGetProcedureAddress.
// `build` is the unshimmed NT build number (caller reads it once from
// KUSER_SHARED_DATA). Populates `table` with real exports where available
// (leaving fallbacks for missing ones). Sets capability bits in `caps_out`.
void probe_nt_capabilities(uint32_t build, NtOptionalSyscalls &table,
                           uint32_t &caps_out);

//===----------------------------------------------------------------------===//
// Runtime accessor — zero-cost read from the sealed PCB page
//===----------------------------------------------------------------------===//

// Returns the sealed NtOptionalSyscalls table from PcbZone0.
// Declared here so call sites only need this header, not the full PCB.
// Defined in nt_capabilities.cpp (reads g_pcb.zone0.optional()).
const NtOptionalSyscalls &nt_optional();

// Returns the capability bitmask from PcbZone0.
uint32_t nt_caps();

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CAPABILITIES_H
