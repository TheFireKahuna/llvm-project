//===-- Process mitigation policy init (Phase 0.5) -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// mitigation_policy_startup_init(): applies Windows process mitigation
// policies at Phase 0.5 of __libc_dll_init(), after PCB constants are live
// (Phase 0) but before VEH registration (Phase 1).
//
// This function locks down the process against exploit techniques before any
// subsystem infrastructure (allocator, fd table, signal, reactor) exists.
//
// Fatal policies (Win11 baseline — __builtin_trap on failure):
//   - DEP permanent (via ProcessExecuteFlags, not the mitigation envelope)
//   - ASLR force-relocate + bottom-up + high-entropy
//   - Strict handle checks (permanent)
//   - Extension point disable
//   - CFG strict + export suppression (only if image PE has GUARD_CF)
//   - Image load restrictions (no remote, no low-integrity)
//
// Non-fatal policies (hardware/environment dependent):
//   - CET shadow stack (only if CPU + OS support CET-U)
//   - Side-channel isolation (VMs may not support)
//
// Not applied (by design):
//   - ACG (ProcessDynamicCodePolicy): incompatible with mmap(PROT_EXEC)
//   - SEHOP: x86-32 only, irrelevant for x64 table-based unwind
//   - System call disable: may break error reporting
//   - Signature policy: too restrictive for general POSIX userland
//   - Child process policy: breaks fork/exec/posix_spawn
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/OSUtil/windows/nt/nt_context_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_string_api.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/macros/config.h"

namespace {

using namespace LIBC_NAMESPACE;

// IMAGE_DLL_CHARACTERISTICS_GUARD_CF — set by lld only when -guard:cf was
// passed. This is the definitive indicator that the image has a valid CFG
// function table and should be placed under strict CFG policy.
constexpr USHORT kImageGuardCf = 0x4000;

// Apply a single mitigation policy via the class-52 envelope.
LIBC_INLINE NTSTATUS
set_mitigation_policy(PROCESS_MITIGATION_POLICY_INFORMATION &info) {
  return ::NtSetInformationProcess(NtCurrentProcess(), ProcessMitigationPolicy,
                                   &info, sizeof(info));
}

// Returns true if the CPU and OS support CET user-mode shadow stacks.
// Reads KUSER_SHARED_DATA directly — zero-cost, no syscall.
LIBC_INLINE bool cpu_supports_cet() {
#if defined(__x86_64__)
  const auto *ksd = windows_util::shared_user_data();
  return (ksd->XState.EnabledUserVisibleSupervisorFeatures &
          XSTATE_MASK_CET_U) != 0;
#else
  return false;
#endif
}

// Returns true if the current EXE image was linked with CFG enforcement.
// Reads the PE DllCharacteristics directly — the __guard_flags symbol from
// crt_cfg.obj is unreliable because lld may not have overridden it if
// -guard:cf was not passed, leaving the static initializer value (which
// falsely claims instrumented + table present).
LIBC_INLINE bool image_has_cfg() {
  auto *base = NtCurrentPeb()->ImageBaseAddress;
  auto *nt_hdr = static_cast<IMAGE_NT_HEADERS64 *>(::RtlImageNtHeader(base));
  if (!nt_hdr)
    return false;
  return (nt_hdr->OptionalHeader.DllCharacteristics & kImageGuardCf) != 0;
}

} // namespace

int LIBC_NAMESPACE::internal::mitigation_policy_startup_init() {
  PROCESS_MITIGATION_POLICY_INFORMATION info;
  NTSTATUS st;

  // --- Fatal policies: Win11 baseline, trap on failure ---

  // DEP permanent: make NX non-revocable. Uses ProcessExecuteFlags (class 34),
  // not the class-52 mitigation envelope — DEP predates the mitigation policy
  // infrastructure and is controlled via MEM_EXECUTE_OPTION_* flags.
  {
    ULONG exec_flags = MEM_EXECUTE_OPTION_DISABLE |
                       MEM_EXECUTE_OPTION_DISABLE_THUNK_EMULATION |
                       MEM_EXECUTE_OPTION_PERMANENT;
    st = ::NtSetInformationProcess(NtCurrentProcess(), ProcessExecuteFlags,
                                   &exec_flags, sizeof(exec_flags));
    // STATUS_ACCESS_DENIED means DEP is already permanent — that's fine.
    if (!NT_SUCCESS(st) && st != STATUS_ACCESS_DENIED)
      __builtin_trap();
  }

  // ASLR: force-relocate all images (even those without DYNAMIC_BASE),
  // enable bottom-up randomization and high-entropy VA.
  // Note: DisallowStrippedImages is intentionally omitted — it would block
  // loading any DLL with stripped relocs (some third-party drivers, AV
  // injectors, debug tools ship without relocs). Force-relocate already
  // does the right thing for images that have relocs.
  info = {};
  info.Policy = ProcessASLRPolicyClass;
  info.ASLRPolicy.EnableForceRelocateImages = 1;
  info.ASLRPolicy.EnableBottomUpRandomization = 1;
  info.ASLRPolicy.EnableHighEntropy = 1;
  st = set_mitigation_policy(info);
  if (!NT_SUCCESS(st))
    __builtin_trap();

  // Strict handle checks: raise STATUS_INVALID_HANDLE on bad handle ops.
  // Catches use-after-close and double-close bugs early. Permanent.
  //
  // Ordering note: this runs before VEH (Phase 1). From this point on, any
  // invalid handle use raises an exception with no VEH handler installed —
  // the process terminates immediately. This is intentional: invalid handles
  // during early init indicate a fatal bug.
  info = {};
  info.Policy = ProcessStrictHandleCheckPolicyClass;
  info.StrictHandleCheckPolicy.RaiseExceptionOnInvalidHandleReference = 1;
  info.StrictHandleCheckPolicy.HandleExceptionsPermanentlyEnabled = 1;
  st = set_mitigation_policy(info);
  if (!NT_SUCCESS(st))
    __builtin_trap();

  // Extension point disable: blocks legacy DLL injection via AppInit_DLLs,
  // IMEs, shell extensions, Winsock Layered Service Providers, etc.
  info = {};
  info.Policy = ProcessExtensionPointDisablePolicyClass;
  info.ExtensionPointDisablePolicy.DisableExtensionPoints = 1;
  st = set_mitigation_policy(info);
  if (!NT_SUCCESS(st))
    __builtin_trap();

  // CFG strict mode + export suppression: only if the PE image was actually
  // linked with CFG (DllCharacteristics & IMAGE_GUARD_CF). The loader
  // already enabled basic CFG from the PE header; strict mode tightens the
  // valid-target set and export suppression blocks return-to-export gadgets.
  if (image_has_cfg()) {
    info = {};
    info.Policy = ProcessControlFlowGuardPolicyClass;
    info.ControlFlowGuardPolicy.EnableControlFlowGuard = 1;
    info.ControlFlowGuardPolicy.StrictMode = 1;
    info.ControlFlowGuardPolicy.EnableExportSuppression = 1;
    st = set_mitigation_policy(info);
    if (!NT_SUCCESS(st))
      __builtin_trap();
  }

  // Image load restrictions: block DLLs from network shares and
  // low-integrity locations. Prevents remote DLL injection and
  // untrusted-path DLL planting.
  // Note: PreferSystem32Images is intentionally omitted — NTPOSIX
  // loads c.dll/c++.dll/unwind.dll from the application directory,
  // not System32, and PreferSystem32 could misdirect those loads.
  info = {};
  info.Policy = ProcessImageLoadPolicyClass;
  info.ImageLoadPolicy.NoRemoteImages = 1;
  info.ImageLoadPolicy.NoLowMandatoryLabelImages = 1;
  st = set_mitigation_policy(info);
  if (!NT_SUCCESS(st))
    __builtin_trap();

  // --- Non-fatal policies: hardware/environment dependent ---

  // Side-channel isolation: SMT branch target isolation (IBPB on context
  // switch, free on Zen 3+/Golden Cove+ with eIBRS) and page combine
  // disable (prevents cross-process side channel via CoW fault timing
  // on deduplicated pages). Both are zero-cost on modern hardware.
  //
  // IsolateSecurityDomain (bit 1) is intentionally omitted — it hints
  // the scheduler to avoid co-scheduling with other security domains on
  // sibling SMT threads, which harms throughput for cooperating NTPOSIX
  // processes (shell pipelines, parallel builds) with no benefit when
  // all processes share the same trust level.
  //
  // VMs and older hardware may not support all bits — non-fatal.
  info = {};
  info.Policy = ProcessSideChannelIsolationPolicyClass;
  info.SideChannelIsolationPolicy.SmtBranchTargetIsolation = 1;
  info.SideChannelIsolationPolicy.DisablePageCombine = 1;
  (void)set_mitigation_policy(info);

  // CET shadow stack: full enforcement if hardware supports it.
  // Requires Ice Lake+ (Intel) or Zen 3+ (AMD). Non-fatal — the CET-compat
  // PE mark (from the linker) still provides OS-level enforcement on
  // supporting hardware even if this runtime call fails.
  if (cpu_supports_cet()) {
    info = {};
    info.Policy = ProcessUserShadowStackPolicyClass;
    info.UserShadowStackPolicy.EnableUserShadowStack = 1;
    info.UserShadowStackPolicy.EnableUserShadowStackStrictMode = 1;
    info.UserShadowStackPolicy.BlockNonCetBinariesNonEhcont = 1;
    info.UserShadowStackPolicy.CetDynamicApisOutOfProcOnly = 1;
    (void)set_mitigation_policy(info);
  }

  return 0;
}
