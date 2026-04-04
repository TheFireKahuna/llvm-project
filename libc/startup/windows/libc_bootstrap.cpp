//===-- Tier A libc bootstrap for Windows --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __libc_bootstrap() — Tier A: brings the process into a state where code
// can execute safely. Runs before any Tier B subsystem init, any user
// C initializer, any C++ constructor, any allocation or signal path.
//
// Phases:
//   - OS floor check (NtBuildNumber >= Windows 11 23H2, build 22631).
//   - PCB Zone 0 constants (page_size, security_cookie, module_handle,
//     PID, nt_build, capability bitmask, syscall table).
//   - Master VEH reentry guard + VEH handler installation. After this
//     point faults route through our filter chain.
//   - Process identity (uid/gid/privilege, parent_pid into Zone 0).
//   - Seal PCB Zone 0 PAGE_READONLY.
//
// Invariants:
//   - No allocator calls (Tier B brings up posix_alloc).
//   - No thread registry, futex, fd table, stdio references.
//   - Only standard `__LIBC_EXTERN_DLLIMPORT_ATTR` callees — never anything reachable
//     through `__delayLoadHelper2`. Standard imports are bound by the loader
//     before _DllMainCRTStartup runs, so calls into ntdll, bcryptprimitives
//     (ProcessPrng), and similar resolve directly through the IAT without
//     ever entering the delay-load helper. Adding a `/DELAYLOAD:` for any
//     dependency reachable from Tier A would break this — the helper itself
//     uses SEH and would fault before the master VEH is installed.
//   - Idempotent: safe to call from both c.dll's _DllMainCRTStartup and
//     the EXE entry path. The winner-publishes / loser-waits protocol
//     ensures every caller observes a complete Tier A before returning,
//     so a downstream `__libc_dll_init()` cannot race a still-running
//     `pcb_startup_init()` in another thread.
//
// Tier B (allocator, threads, fd table, stdio, env/argv, ...) runs from
// __libc_init() / __libc_dll_init() after __libc_bootstrap() returns.
//
//===----------------------------------------------------------------------===//

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/nt/shared_user_data.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"
#include "src/__support/macros/config.h"
#include "startup/windows/tier_a_trace.h"

namespace {

// STATUS_NOT_SUPPORTED — the running OS is below the hard floor.
constexpr NTSTATUS STATUS_UNSUPPORTED_OS = static_cast<NTSTATUS>(0xC00000BBL);

// STATUS_DLL_INIT_FAILED — a Tier A phase reported failure. We abort here
// rather than unwinding because the process is not yet in a state where
// any fault-tolerant cleanup could run.
constexpr NTSTATUS STATUS_BOOTSTRAP_FAILED = static_cast<NTSTATUS>(0xC0000142L);

[[noreturn]] void bootstrap_abort(NTSTATUS status) {
  ::NtTerminateProcess(NtCurrentProcess(), status);
  __builtin_unreachable();
}

void run_bootstrap() {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::internal;

  // Load env-gated trace knobs once. Safe at Phase −1: PEB and
  // ProcessParameters are mapped before user code ever runs, and the
  // scan reads only committed memory already in our address space.
  const TierATraceState trace = TierATraceState::load();
  trace.phase("phase -1: OS floor check");

  // ---------------------------------------------------------------------
  // Phase −1: OS floor — Windows 11 23H2 (build 22631).
  //
  // KUSER_SHARED_DATA.NtBuildNumber is kernel-mapped read-only at
  // 0x7FFE0000 — cannot be shimmed by application manifests, cannot
  // fault.
  // ---------------------------------------------------------------------
  {
    uint32_t build = windows_util::shared_user_data()->NtBuildNumber;
    if (build < nt_version::WIN11_23H2)
      bootstrap_abort(STATUS_UNSUPPORTED_OS);
  }

  // ---------------------------------------------------------------------
  // Phase 0a/0b: Master VEH FIRST.
  //
  // The reentry guard claims one TEB inline TLS slot via CAS on
  // PEB->TlsBitmap (no Zone 0 dependency, cannot fault). veh_core then
  // installs the master VEH at front-of-chain via
  // RtlAddVectoredExceptionHandler and registers ONE combined dll-
  // load/unload notification (load: re-add VEH; unload: __cxa_finalize).
  //
  // After this returns, every subsequent Tier A access — PCB writes,
  // ProcessPrng, NtQuerySystemInformation, identity_startup_init's token
  // reads — runs under our master handler. The filter table is empty at
  // this point (sweep happens at Phase 0d), so the handler returns
  // EXCEPTION_CONTINUE_SEARCH for any HW exception until Phase 0d, but
  // the handler itself is in the chain and can be extended with a
  // bootstrap-phase diagnostic if/when desired.
  // ---------------------------------------------------------------------
  // Each phase below pairs its work with pcb_init_state_advance_checked,
  // pinning the caller graph at runtime. A reorder of these blocks (or
  // an out-of-sequence re-invocation) trips the CAS inside
  // advance_checked and terminates the process immediately, rather than
  // letting a skipped phase's invariants silently corrupt a later one.
  //
  // Phase 0a/0b: Master VEH FIRST.
  trace.phase("phase 0a/0b: master VEH");
  if (veh_reentry_guard_startup_init() != 0 ||
      veh_core_startup_init() != 0)
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  pcb_init_state_advance_checked(PcbInitState::None, PcbInitState::TierA_VehUp);

  // Phase 0c: PCB Zone 0 + Zone 0b constants. Now safe — any fault routes
  // through the master VEH installed above.
  trace.phase("phase 0c: PCB Zone 0/0b constants");
  if (pcb_startup_init() != 0)
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  pcb_init_state_advance_checked(PcbInitState::TierA_VehUp,
                                 PcbInitState::TierA_PcbWritten);

  // Phase 0c.5: Memory primitives — `.libcmem` walker.
  //
  // Walks `.libcmem$M` and runs every registered memory-primitive init
  // handler: VaSubstrate seeds its secrets + reserves one arena per
  // class; MappingTable runs `ensure_init()` (L1 root, remap guards,
  // RegionPool directory); future primitives plug in here too. After
  // every Pass 1 handler returns, the walker batch-registers every
  // emitted Receipt into the now-live mapping table as LIBC_INTERNAL,
  // then stamps pre-existing VA (TEB/PEB/stack, loaded modules) via
  // va_inventory_startup_discover().
  //
  // Traps on any handler or registration failure — libc init has no
  // useful partial-recovery posture. See memory_primitives_bootstrap.h
  // for the protocol and unified-memory-bootstrap.md for the design.
  trace.phase("phase 0c.5: memory primitives (.libcmem sweep)");
  memory_primitives_startup_init();
  pcb_init_state_advance_checked(PcbInitState::TierA_PcbWritten,
                                 PcbInitState::TierA_MappingTable);

  // Phase 0d: mlock policy onfault state.
  //
  // Runs BEFORE the .libcveh sweep so the mlock filter's dispatch
  // handler has a constructed OnfaultState to dereference. The
  // register_all_static_veh_filters() call below asserts this ordering
  // via the TierA_MlockPolicy predecessor check — a future reordering
  // that installs the filter first would terminate at that CAS rather
  // than expose a live VEH filter that reads uninitialised state.
  trace.phase("phase 0d.1: mlock policy onfault state");
  if (mlock_policy_startup_init() != 0)
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  pcb_init_state_advance_checked(PcbInitState::TierA_MappingTable,
                                 PcbInitState::TierA_MlockPolicy);

  // Phase 0d.2: Static VEH filter sweep. Walks `.libcveh$M` and
  // populates the sealed filter table in priority order. Filters self-
  // gate against their own subsystem state (signal_veh_transport returns
  // CONTINUE_SEARCH when no SEH-class handler is installed; mem_fault
  // degrades via zero-init mapping table).
  trace.phase("phase 0d.2: .libcveh sweep");
  register_all_static_veh_filters();
  pcb_init_state_advance_checked(PcbInitState::TierA_MlockPolicy,
                                 PcbInitState::TierA_FiltersInstalled);

  // Phase 1: Process identity. Token queries can raise access violations
  // on exotic token layouts; runs under VEH protection with the full
  // filter table already live.
  trace.phase("phase 1: process identity");
  if (identity_startup_init() != 0)
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  pcb_init_state_advance_checked(PcbInitState::TierA_FiltersInstalled,
                                 PcbInitState::TierA_Identity);

  // Phase 2: Seal Zone 0 (lifetime-immutable) and Zone 0b (fork-mutable).
  //
  // Canary validates Zone 0b cookie integrity — if anything strayed
  // into the cookie/complement/canary triple during earlier phases the
  // mismatch is fatal. Seal failures are also fatal: Zone 0 holds the
  // VEH dispatch table consulted on every fault, the syscall table, and
  // capability bitmask; without hardware R/O the only defence against an
  // arbitrary-write primitive is gone. NtProtectVirtualMemory R/W→R/O on
  // owned committed pages is used by every JIT and guard-page consumer
  // on Windows — it does not legitimately fail on the supported floor.
  trace.phase("phase 2: seal Zone 0/0b");
  if (!pcb_check_canary())
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  if (!pcb_seal_readonly_a())
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  if (!pcb_seal_readonly_b())
    bootstrap_abort(STATUS_BOOTSTRAP_FAILED);
  pcb_init_state_advance_checked(PcbInitState::TierA_Identity,
                                 PcbInitState::TierA);

  trace.end_of_tier_a();
}

} // namespace

extern "C" void __libc_bootstrap() {
  // Winner-claims / loser-waits. Both c.dll _DllMainCRTStartup and the EXE
  // entry path may call this concurrently; the winner of the CAS owns
  // Tier A, every other caller spin-waits until the winner publishes
  // pcb_init_state >= TierA via run_bootstrap()'s RELEASE store.
  //
  // Why we cannot return immediately on a losing exchange: the next thing
  // every caller does is invoke `__libc_dll_init()`, which CASs an init
  // gate and then writes PCB Zone 0 fields. If thread B returns from a
  // losing __libc_bootstrap before thread A has finished
  // pcb_startup_init(), thread B can win the dll_init CAS and start Tier B
  // against half-initialised Zone 0 state. The wait below closes that
  // window without serialising the common single-call case (the CAS hits
  // the cache as a single uncontended ACQ_REL on first call).
  using LIBC_NAMESPACE::cpp::MemoryOrder;
  static LIBC_NAMESPACE::cpp::Atomic<int> g_bootstrap_claimed{0};
  int prev = 0;
  if (g_bootstrap_claimed.compare_exchange_strong(prev, 1, MemoryOrder::ACQ_REL,
                                                  MemoryOrder::RELAXED)) {
    run_bootstrap(); // Publishes pcb_init_state == TierA on its way out.
    return;
  }
  // Loser: spin-wait on the winner's RELEASE-published init state.
  // Tier A is short (microseconds — no I/O, no allocation, no kernel
  // wait), so a pause-spin is cheaper than a kernel block.
  while (LIBC_NAMESPACE::pcb_init_state() < LIBC_NAMESPACE::PcbInitState::TierA) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
  }
}
