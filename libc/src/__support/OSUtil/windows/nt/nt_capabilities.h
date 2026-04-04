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

// CPU hardware capabilities detected once via CPUID at Phase 0. Bits 8+
// are reserved for CPU caps so 0..7 stay available for future NT caps.
inline constexpr uint32_t X86_UMWAIT      = 1u << 8; // Intel WAITPKG (UMONITOR/UMWAIT/TPAUSE)
inline constexpr uint32_t X86_MWAITX      = 1u << 9; // AMD MONITORX/MWAITX
inline constexpr uint32_t ARM_WFXT        = 1u << 8;   // FEAT_WFxT (v8.7): WFET/WFIT
inline constexpr uint32_t ARM_LSE         = 1u << 9;   // FEAT_LSE (v8.1): atomics
inline constexpr uint32_t ARM_LRCPC       = 1u << 10; // FEAT_LRCPC (v8.3): LDAPR
} // namespace nt_cap

//===----------------------------------------------------------------------===//
// Extended parameter for NtAlertMultipleThreadByThreadId (Windows 11 24H2+).
// Type=0 = PsAlertMultipleExtendedParameterAutoBoostContext: the payload is
// an opaque 64-bit token associated with the synchronization/ownership
// handoff context. Passing the sync object address engages the NT kernel's
// AutoBoost (Ab) subsystem — priority boost for woken waiters and wait-chain
// tracking. Only the *last* extended parameter's payload takes effect; a
// single entry (count=1) is the norm.
struct PS_ALERT_THREAD_EXTENDED_PARAMETER {
  // Low 8 bits: type (0 = AutoBoostContext). High 56 bits: reserved, zero.
  ULONGLONG TypeAndReserved;
  // AutoBoostContext token — typically the sync object's address.
  union {
    ULONGLONG ULong64;
    PVOID Pointer;
  };
};
static_assert(sizeof(PS_ALERT_THREAD_EXTENDED_PARAMETER) == 16,
              "PS_ALERT_THREAD_EXTENDED_PARAMETER must be 16 bytes");

//===----------------------------------------------------------------------===//
// Fallback implementations
//
// These provide correct (but potentially slower) behavior using syscalls
// that are guaranteed to exist on the 23H2 floor. They are the initial
// values of the dispatch table pointers, and their declarations double as
// the source-of-truth for the dispatch pointer types below — `decltype` on
// an NTAPI-declared prototype carries the MS-ABI attribute through to the
// alias without tripping `-Wgcc-compat` (unlike attributing a type alias
// directly, which GCC/Clang both forbid on a function type).
//===----------------------------------------------------------------------===//

namespace nt_fallback {

// Fallback: release lock manually, then alert. Non-atomic (two syscalls
// instead of one), but correct — waiters always re-check their predicate
// after wake, so the window between lock release and alert is harmless.
NTAPI NTSTATUS alert_thread_by_thread_id_ex(HANDLE ThreadId, PVOID Lock);

// Fallback: sequential per-thread alerts. O(n) syscalls instead of O(1),
// but correct — each thread is individually woken.
NTAPI NTSTATUS alert_multiple_by_thread_id(HANDLE *ThreadIds, ULONG Count,
                                           PVOID ExtendedParameters,
                                           ULONG ExtendedParameterCount);

} // namespace nt_fallback

//===----------------------------------------------------------------------===//
// Optional syscall pointer aliases
//
// Derived from the fallback prototypes via `decltype` so the MS-ABI
// attribute rides in on the declaration side (legal) rather than being
// pinned to a type alias (rejected by GCC, warned by Clang).
//===----------------------------------------------------------------------===//

using NtAlertThreadByThreadIdExFn =
    decltype(&nt_fallback::alert_thread_by_thread_id_ex);
using NtAlertMultipleThreadByThreadIdFn =
    decltype(&nt_fallback::alert_multiple_by_thread_id);

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

//===----------------------------------------------------------------------===//
// Zero-overhead inline capability read
//
// Hot-path spin loops (spin_wait.h) branch on UMWAIT vs MWAITX vs RELAX on
// every entry. An out-of-line call into nt_caps() is cheap but still adds
// a call/ret pair (~5 cycles) — and worse, the naive alternative of a
// function-local-static-guarded cache emits an acquire load + test + branch
// on every call. Neither is acceptable in spin primitives.
//
// The trick: PcbZone0's layout is pinned by static_asserts, so we can
// forward-declare ProcessControlBlock and read `capabilities_` directly
// at a fixed byte offset. The sealed PAGE_READONLY page is still the
// single source of truth — this is just an offset-based accessor that
// avoids pulling process_control_block.h (which would create an include
// cycle through thread_lifecycle.h → futex_utils.h → spin_wait.h).
//
// The matching static_assert in process_control_block.h validates the
// offset at compile time; any PcbZone0 layout change that disturbs
// capabilities_'s offset will trip the assert.
//===----------------------------------------------------------------------===//

struct ProcessControlBlock;
extern ProcessControlBlock g_pcb;

namespace internal_nt_caps {
// Offset of PcbZone0::capabilities_ within g_pcb. Validated in
// process_control_block.h.
//
// Layout (post Zone-0 split):
//   page_size(4) + alloc_granularity(4) + min_address(8) + max_address(8) +
//   module_handle(8) + dso_handle(8) = 40
//   session_id(4) + reserved(4) = 48
//   nt_build(4) = 52
//   capabilities ← here
inline constexpr unsigned kPcbCapabilitiesOffset = 52;
} // namespace internal_nt_caps

[[nodiscard]] LIBC_INLINE uint32_t nt_caps_fast() {
  return *reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const char *>(&g_pcb) +
      internal_nt_caps::kPcbCapabilitiesOffset);
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_CAPABILITIES_H
