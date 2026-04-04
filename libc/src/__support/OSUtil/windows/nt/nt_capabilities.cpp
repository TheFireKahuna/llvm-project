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
#include "src/__support/OSUtil/windows/nt/nt_wstring_view.h"
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

NTAPI NTSTATUS
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

NTAPI NTSTATUS
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
  windows::nt_wstring_view ntdll_wsv(u"ntdll.dll");

  PVOID ntdll_handle = nullptr;
  NTSTATUS st =
      ::LdrGetDllHandleByName(ntdll_wsv.unicode_string(), nullptr, &ntdll_handle);
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

  // --- x86 hardware spin-wait capabilities ---
  // Intel WAITPKG: CPUID.7.0:ECX[5] enables UMONITOR/UMWAIT/TPAUSE.
  // AMD MONITORX:  CPUID.80000001:ECX[29] enables MONITORX/MWAITX.
  // Both run unconditionally — neither vendor surfaces its competitor's
  // bit set, so setting at most one is guaranteed. Executing these
  // instructions on non-supporting hardware raises #UD, so a correct
  // probe is load-bearing for spin_wait.h.
#if defined(__x86_64__) || defined(_M_X64)
  {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
    if (ecx & (1u << 5))
      caps_out |= nt_cap::X86_UMWAIT;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001), "c"(0));
    if (ecx & (1u << 29))
      caps_out |= nt_cap::X86_MWAITX;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  {
    // --- FEAT_WFxT (ARMv8.7+): WFET/WFIT ---
    //
    // Windows surfaces no API for this feature. IsProcessorFeaturePresent
    // / KUSER_SHARED_DATA.ProcessorFeatures top out at PF_ARM_V83_LRCPC
    // (index 45) in the current SDK; no PF_ARM_V87_* slot exists. The
    // alternative — MRS ID_AA64ISAR2_EL1 — is #UD from EL0 on Windows
    // (no MRS emulation, unlike Linux's HWCAP_CPUID path). So we probe
    // by attempting WFET inside a vectored exception handler and
    // checking whether STATUS_ILLEGAL_INSTRUCTION fires.
    //
    // WFET takes an absolute CNTVCT_EL0 deadline; deadline=0 is
    // already-expired, so on supporting hardware the instruction
    // returns immediately as a single-cycle hint with no actual sleep.
    // On non-supporting hardware it #UDs and the VEH skips past it
    // (PC += 4, one AArch64 instruction width).
    //
    // Cost: one VEH register/unregister + one WFET on success, or one
    // exception (~1μs typical) on failure. Runs once at Phase 0.
    //
    // Note: practically every currently-shipping Windows-on-ARM SKU
    // (Snapdragon 8cx Gen 3 onwards, Snapdragon X Elite/Plus on Oryon)
    // implements FEAT_WFxT, so this probe almost always succeeds. The
    // WFE fallback in spin_wait.h primarily serves older devkits and
    // pre-8cx-Gen-3 hardware that may still be running 11 24H2.

    static volatile uint32_t wfxt_undef_flag;
    wfxt_undef_flag = 0;

    // Non-capturing lambda → C-style function pointer. State flows
    // through the static flag (probe is single-threaded at Phase 0,
    // so no atomicity concern beyond the volatile).
    auto handler = +[](EXCEPTION_POINTERS *info) noexcept -> LONG {
      // STATUS_ILLEGAL_INSTRUCTION = 0xC000001D. Hardcoded to avoid
      // pulling <ntstatus.h> into this TU.
      if (info->ExceptionRecord->ExceptionCode == 0xC000001Du) {
        // Skip the WFET (4 bytes) and record the trap. Pc here is
        // ContextRecord->Pc on AArch64 (CONTEXT::Pc field, distinct
        // from x86's Rip).
        info->ContextRecord->Pc += 4;
        wfxt_undef_flag = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
      }
      return EXCEPTION_CONTINUE_SEARCH;
    };

    PVOID veh = ::RtlAddVectoredExceptionHandler(/*FirstHandler=*/1u,
                                                  handler);
    if (veh) {
      // WFET expects an absolute CNTVCT_EL0 deadline in Xn. Zero is
      // unconditionally in the past (counter is monotonic from boot),
      // so the instruction completes the timeout check on the first
      // micro-op and retires.
      uint64_t expired_deadline = 0;
      __asm__ volatile(".arch_extension wfxt\n\t"
                       "wfet %0\n\t"
                       ".arch_extension nowfxt"
                       :: "r"(expired_deadline)
                       : "memory");

      ::RtlRemoveVectoredExceptionHandler(veh);

      if (!wfxt_undef_flag)
        caps_out |= nt_cap::ARM_WFXT;
    }
    // If RtlAddVectoredExceptionHandler failed (extreme low-memory at
    // process init), keep ARM_WFXT clear — spin_wait.h falls through
    // to the WFE backend, which is correct on every AArch64 host.
  }

  {
    // --- FEAT_LSE (v8.1 atomics) and FEAT_LRCPC (v8.3) ---
    //
    // These have IsProcessorFeaturePresent flags. Read directly from
    // KUSER_SHARED_DATA.ProcessorFeatures (offset 0x274, BYTE[64])
    // rather than calling the kernel32 wrapper — same pattern as
    // kuser_spin_threshold() in spin_wait.h. The byte is 1 if the
    // feature is enabled by the kernel for userspace, 0 otherwise.
    //
    // PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE = 34
    // PF_ARM_V83_LRCPC_INSTRUCTIONS_AVAILABLE  = 45
    auto pf_present = [](uint32_t pf_index) -> bool {
      return *reinterpret_cast<const volatile uint8_t *>(
          0x7FFE0274ull + pf_index) != 0;
    };
    if (pf_present(34))
      caps_out |= nt_cap::ARM_LSE;
    if (pf_present(45))
      caps_out |= nt_cap::ARM_LRCPC;
  }
#endif
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
