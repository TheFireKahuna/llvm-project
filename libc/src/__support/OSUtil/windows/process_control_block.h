//===-- Process Control Block (PCB) for NTPOSIX libc -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ProcessControlBlock: single PE-section-resident struct consolidating all
// small, fixed, process-lifetime mutable state. Replaces ~40 scattered BSS
// globals and placement-new singletons with one demand-zero PE section.
//
// Design principles:
//   - Inline if spec-bounded (NSIG, RLIMIT_COUNT, hardware pkey count).
//   - Pointer if workload-bounded (fd table, child entries, atexit array).
//   - Zero-init is valid for every field (PE loader demand-zero suffices).
//   - Trivially destructible (no dtors; handles closed by subsystem fini).
//   - All locks in one place (fork reinit touches one struct).
//
// Memory layout:
//   Page 0 (PcbZone0)  — sealed PAGE_READONLY at end of Tier A and NEVER
//                        unsealed. Holds lifetime-immutable constants
//                        (page_size, module_handle, NT capabilities, ...)
//                        and the sealed VEH dispatch state (filter table,
//                        TLS indices). Fields are private — only
//                        PcbInitAccess (pcb_init_access.h) can write them.
//
//   Page 1 (PcbZone0b) — sealed PAGE_READONLY at end of Tier A; unsealed
//                        only inside libc_fork_reinit() and the matching
//                        veh_core fini path. Holds fork-mutable PCB state
//                        (pid, parent_pid, /GS-style PCB cookie, dll
//                        notification cookie). Same private+friend pattern.
//
//   Pages 2+ (Zone 1)  — mutable runtime state.
//
// Write discipline:
//   Zone 0  (immutable forever): compile-time via private fields + friend
//                                PcbInitAccess; runtime via PAGE_READONLY
//                                seal after Tier A. Never unsealed.
//   Zone 0b (fork-mutable):      same compile-time gate; PAGE_READONLY most
//                                of the time, briefly unsealed during fork
//                                reinit and veh_core teardown to rewrite
//                                pid / cookies / dll-notify cookie.
//   Zone 1: subsystem code writes through accessor headers (security.h,
//           page_size.h, etc.) — leaf code never includes this header.
//           cpp::Atomic<> on mutable fields prevents accidental plain stores.
//
// PE section:
//   Placed in ".pcb" via #pragma section + __LIBC_SECTION_ATTR. The NT
//   loader maps it with VirtualSize > SizeOfRawData, providing demand-zero
//   pages on first touch. No runtime page_reserve or NtCreateSection needed.
//
// See docs/pcb-design.md for the full design rationale.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/pid_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/io/environment_state.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/ipc/alpc_bus_state.h"
#include "src/__support/OSUtil/windows/ipc/ipc_process_state.h"
#include "src/__support/OSUtil/windows/memory/brk_process_state.h"
#include "src/__support/OSUtil/windows/memory/mapping_table_process_state.h"
#include "src/__support/OSUtil/windows/nls_state.h"
#include "src/__support/OSUtil/windows/process/console_process_state.h"
#include "src/__support/OSUtil/windows/process/child_table_state.h"
#include "src/__support/OSUtil/windows/process/console_tty_state.h"
#include "src/__support/OSUtil/windows/process/exec_process_state.h"
#include "src/__support/OSUtil/windows/process/process_identity_state.h"
#include "src/__support/OSUtil/windows/process/startup_process_state.h"
#include "src/__support/OSUtil/windows/reactor/reactor_state.h"
#include "src/__support/OSUtil/windows/resource/rlimit_process_state.h"
#include "src/__support/OSUtil/windows/security/pkey_process_state.h"
#include "src/__support/OSUtil/windows/signal/process_signal_state.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/OSUtil/windows/time/itimer_state.h"
#include "src/__support/OSUtil/windows/tls/tls_cleanup_state.h"
#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry_state.h"

// Minimal NT types — only the base scalar/handle types, not the full API set.
#include "src/__support/OSUtil/windows/nt/nt_types.h"

namespace LIBC_NAMESPACE_DECL {

namespace internal {
class PcbInitAccess; // Defined in pcb_init_access.h — the sole write gate.
} // namespace internal

// =========================================================================
// PCB-local structs
//
// These are small PCB-owned compositions built from canonical subsystem
// state types. They exist to group related PCB members, not to mirror
// some other definition.
// =========================================================================

// Canonical PCB-resident storage for the process's initial thread.
// Held as opaque-bytes-with-alignas rather than typed members so the
// PCB itself stays a standard-layout struct (Crystalline-managed
// ThreadLifecycle inherits a Crystalline header, which makes its
// type non-standard-layout and would otherwise infect every PCB
// `__builtin_offsetof` static_assert when ThreadLifecycle were
// embedded directly).
//
// signal_state.cpp owns construction:
//   - `init_signal_state()` runs `zero_lifecycle` over `lifecycle`
//     and `zero_thread_state` over `signal`, then wires the
//     lifecycle's notify_word and the back-pointers. Single-threaded
//     at startup, so plain byte init is safe.
//
// External readers always go through signal_state.cpp's accessors
// (`main_thread_lifecycle()`, `main_thread_state()`) which
// reinterpret_cast through the well-typed handle below.
struct alignas(64) MainThreadState {
  alignas(signal_state::ThreadSignalState)
      unsigned char signal_storage[sizeof(signal_state::ThreadSignalState)];
  alignas(ThreadLifecycle)
      unsigned char lifecycle_storage[sizeof(ThreadLifecycle)];
};

// =========================================================================
// Security
// =========================================================================

// XOR salt for the zone canary between read-only and mutable regions.
// The canary (security_cookie ^ PCB_CANARY_MAGIC) detects accidental
// linear buffer overflows or stale-cookie reuse across fork.
//
// Security posture: post-Tier-A, both the cookie and the canary live in
// Zone 0b which is sealed PAGE_READONLY — writable only inside the
// libc_fork_reinit() unseal window. An arbitrary-write attacker therefore
// cannot mutate either operand from user code; the canary is hardware-
// integrity-checked. The salt is link-time constant so disclosure of the
// cookie still allows recomputing the canary; this remains a stale-state /
// linear-overflow detector, not a confidentiality boundary.
inline constexpr uintptr_t PCB_CANARY_MAGIC = 0x50434250'4F534958ULL; // "PCBPOSIX"

// =========================================================================
// ProcessControlBlock
// =========================================================================

// =========================================================================
// PcbZone0: read-only-after-init page
//
// All data fields are private. Only PcbInitAccess (friend) can write them.
// External code reads through the public const accessors. After CRT init,
// page 0 is sealed PAGE_READONLY — writes become hardware faults.
//
// This gives two layers of protection:
//   1. Compile-time: private fields → only friend PcbInitAccess can write.
//   2. Runtime: PAGE_READONLY seal → stray writes trigger access violation.
// =========================================================================

struct PcbZone0 {
  friend class internal::PcbInitAccess;
  friend struct PcbZone0LayoutCheck;

public:
  // --- Read accessors (const, inlined, zero-cost) ---

  [[nodiscard]] LIBC_INLINE uint32_t page_size() const { return page_size_; }
  [[nodiscard]] LIBC_INLINE uint32_t alloc_granularity() const {
    return alloc_granularity_;
  }
  [[nodiscard]] LIBC_INLINE void *min_address() const { return min_address_; }
  [[nodiscard]] LIBC_INLINE void *max_address() const { return max_address_; }
  [[nodiscard]] LIBC_INLINE void *module_handle() const {
    return module_handle_;
  }
  [[nodiscard]] LIBC_INLINE void *dso_handle() const { return dso_handle_; }
  [[nodiscard]] LIBC_INLINE uint32_t session_id() const { return session_id_; }

  // --- NT capability detection (sealed read-only after init) ---

  [[nodiscard]] LIBC_INLINE uint32_t nt_build() const { return nt_build_; }
  [[nodiscard]] LIBC_INLINE uint32_t capabilities() const {
    return capabilities_;
  }
  [[nodiscard]] LIBC_INLINE bool has_cap(uint32_t cap) const {
    return (capabilities_ & cap) != 0;
  }
  [[nodiscard]] LIBC_INLINE const NtOptionalSyscalls &optional() const {
    return optional_;
  }

  // --- Sealed VEH dispatch state ---
  // Filter table is populated by the .libcveh sweep during Tier A and
  // never modified afterward. master_veh_handler reads it without any
  // synchronization — sealed memory makes mutation impossible.
  [[nodiscard]] LIBC_INLINE const windows::VehSealedState &
  veh_sealed() const { return veh_sealed_; }

  // --- Mapping table sealed handles ---
  // Written exactly once by MappingTable::ensure_init() during Tier A
  // (Phase 0c.5 in libc_bootstrap), then sealed for the process lifetime.
  // The table is never re-initialised: fork inherits via CoW, exec
  // preserves the VAs, and process fini just lets the seal die with the
  // process. Storing them here makes the L1 root, guard array, and
  // geometry tamper-resistant against arbitrary-write primitives that
  // would otherwise let an attacker redirect every mmap lookup.
  [[nodiscard]] LIBC_INLINE uintptr_t mapping_table_max_view_base() const {
    return mapping_table_max_view_base_;
  }
  [[nodiscard]] LIBC_INLINE size_t mapping_table_l1_size() const {
    return mapping_table_l1_size_;
  }
  [[nodiscard]] LIBC_INLINE void *mapping_table_l1() const {
    return mapping_table_l1_;
  }
  [[nodiscard]] LIBC_INLINE void *mapping_table_remap_guards() const {
    return mapping_table_remap_guards_;
  }

  // --- VaSubstrate sealed state (populated by Tier A Phase 0a) ---
  //
  // The substrate's secrets and root-pointer are written exactly once
  // during Tier A bring-up and sealed for the process lifetime. After
  // the seal they are hardware-immutable (PAGE_READONLY); an attacker
  // with arbitrary write cannot rewrite `substrate_secret_` to forge
  // arena authenticity tags or `substrate_token_key_` to forge
  // SubSlotHandle owner tokens.
  //
  // Both secrets are drawn from ProcessPrng; init_seed_or_trap traps
  // on a zero draw so these fields are guaranteed non-zero. Callers
  // read them via the const accessors below.
  [[nodiscard]] LIBC_INLINE uintptr_t substrate_secret() const {
    return substrate_secret_;
  }
  [[nodiscard]] LIBC_INLINE uintptr_t substrate_token_key() const {
    return substrate_token_key_;
  }
  [[nodiscard]] LIBC_INLINE void *substrate_root() const {
    return substrate_root_;
  }

private:
  // --- Cache line 0: read-only constants ---

  uint32_t page_size_;                         //   4 B
  uint32_t alloc_granularity_;                 //   4 B
  void *min_address_;                          //   8 B
  void *max_address_;                          //   8 B
  void *module_handle_;                        //   8 B  HINSTANCE of c.dll
  void *dso_handle_;                           //   8 B  __dso_handle
                                               // = 40 B

  // NT session id. Immutable for process lifetime; determines the scope of
  // \Sessions\<id>\BaseNamedObjects for session-local named NT objects
  // (SysV IPC, FIFOs, POSIX semaphores, ALPC ports). Sealed read-only so
  // a corrupted PEB cannot redirect names across session boundaries.
  uint32_t session_id_;                        //   4 B
  [[maybe_unused]] uint32_t _reserved0_;       //   4 B  pad to 8-byte align
                                               // = 48 B

  // --- NT capability detection (populated in Phase 0, sealed with zone) ---

  uint32_t nt_build_;                          //   4 B  unshimmed build number
  uint32_t capabilities_;                      //   4 B  nt_cap:: bitmask
  NtOptionalSyscalls optional_;                //  16 B  sealed function pointers
                                               // = 72 B

  // --- Mapping table sealed handles (populated by Tier A Phase 0c.5) ---
  // Truly write-once for the process lifetime — survive fork via CoW,
  // exec via VA preservation, and only released at process fini.
  uintptr_t mapping_table_max_view_base_;      //   8 B
  size_t mapping_table_l1_size_;               //   8 B
  void *mapping_table_l1_;                     //   8 B  cpp::Atomic<L2Page*>*
  void *mapping_table_remap_guards_;           //   8 B  cpp::Atomic<uintptr_t>*
                                               // = 104 B

  // --- Sealed VEH dispatch state (populated by Tier A sweep) ---
  windows::VehSealedState veh_sealed_;

  // --- VaSubstrate sealed state (populated by Tier A Phase 0a) ---
  // Three immutable-for-process-lifetime pointers/secrets. All non-zero
  // by contract (ProcessPrng fail-closed). The `substrate_root_` points
  // at the namespace-scope VaSubstrate global; placing it under the
  // seal makes dispatch-through-the-root tamper-resistant against an
  // arbitrary-write primitive that would otherwise redirect every
  // libc-internal allocation via a forged vtable / root pointer.
  uintptr_t substrate_secret_;                 //   8 B  ProcessPrng, !=0
  uintptr_t substrate_token_key_;              //   8 B  ProcessPrng, !=0, indep
  void *substrate_root_;                       //   8 B  &g_substrate
                                               // = 128 B used (prev 104)

  // Explicit padding to page boundary. All mutable fields live in Zone 0b
  // (page 1) or Zone 1 (page 2+). The static_assert below verifies size.
  //
  // VEH_MAX_FILTERS is the load-bearing budget here: every additional
  // filter slot adds sizeof(VehFilter) bytes to VehSealedState. The next
  // static_assert keeps a confusing "negative array bound" diagnostic
  // from blocking the actual root cause if the filter table ever grows
  // past the page budget.
  static_assert(128 + sizeof(windows::VehSealedState) < 4096,
                "VehSealedState + substrate fields exceed Zone 0 page "
                "budget — reduce VEH_MAX_FILTERS or shrink VehFilter");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
  uint8_t _pad[4096 - 128 - sizeof(windows::VehSealedState)];
#pragma clang diagnostic pop
};

// =========================================================================
// PcbZone0b: fork-mutable read-only-after-init page
//
// Same private + friend gate as PcbZone0, but the page is unsealed/resealed
// during libc_fork_reinit() and veh_core fini to update fields that change
// across fork or DLL re-registration. Lives in its own page so the fork
// unseal window doesn't expose the lifetime-immutable Zone 0 surface.
// =========================================================================

struct PcbZone0b {
  friend class internal::PcbInitAccess;
  friend struct PcbZone0bLayoutCheck;

public:
  [[nodiscard]] LIBC_INLINE uintptr_t security_cookie() const {
    return security_cookie_;
  }
  [[nodiscard]] LIBC_INLINE uintptr_t security_cookie_complement() const {
    return security_cookie_complement_;
  }
  [[nodiscard]] LIBC_INLINE pid_t pid() const { return pid_; }
  [[nodiscard]] LIBC_INLINE DWORD parent_pid() const { return parent_pid_; }
  [[nodiscard]] LIBC_INLINE void *dll_notify_cookie() const {
    return dll_notify_cookie_;
  }

  // Canary validation — public read, safe for any caller.
  [[nodiscard]] LIBC_INLINE bool check_canary() const {
    return zone_canary_ == (security_cookie_ ^ PCB_CANARY_MAGIC);
  }

private:
  pid_t pid_;                                  //   4 B
  DWORD parent_pid_;                           //   4 B
  uintptr_t security_cookie_;                  //   8 B
  uintptr_t security_cookie_complement_;       //   8 B
  uintptr_t zone_canary_;                      //   8 B
  void *dll_notify_cookie_;                    //   8 B  combined LdrRegisterDllNotification
                                               // = 40 B

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
  uint8_t _pad[4096 - 40];
#pragma clang diagnostic pop
};

// alignas(4096) is load-bearing: pcb_seal_readonly_a/b call
// NtProtectVirtualMemory on `&g_pcb.zone0` and `&g_pcb.zone0b` with a
// page-sized region. Both must lie at page boundaries or the seal will
// overlap an adjacent zone and either fail or protect the wrong bytes.
// The default `#pragma section(".pcb")` gives only 16-byte alignment;
// without alignas the page-aligned layout was incidental.
struct alignas(4096) ProcessControlBlock {
  // ==================================================================
  // ZONE 0: LIFETIME-IMMUTABLE PAGE (sealed PAGE_READONLY at end of
  //         Tier A; never unsealed). Holds PCB constants and the sealed
  //         VEH dispatch table.
  // ==================================================================

  PcbZone0 zone0;

  // ==================================================================
  // ZONE 0b: FORK-MUTABLE PAGE (sealed PAGE_READONLY at end of Tier A;
  //          unsealed only inside libc_fork_reinit() and veh_core fini
  //          to rewrite pid / cookies / dll-notify cookie).
  // ==================================================================

  PcbZone0b zone0b;

  // ==================================================================
  // ZONE 1: INIT STATE (observability only — not a safety mechanism)
  //
  // Observable phase of libc bring-up. Written with RELEASE at the end
  // of __libc_bootstrap() (→ 1) and __libc_dll_init() (→ 2). Read via
  // pcb_init_state() with ACQUIRE for lazy-init preconditions, fork
  // reinit state queries, and debug assertions.
  //
  // Not load-bearing: the Windows loader lock serializes DllMain
  // notifications, so Tier A/B races with THREAD_ATTACH / recursive
  // DllMain cannot actually occur. This flag exists to let callers
  // *assert* expected state, not to enforce it.
  // ==================================================================

  cpp::Atomic<uint32_t> init_state;

  // ==================================================================
  // ZONE 1: PROCESS IDENTITY (mutable at runtime)
  //
  // Fields written by setpgid(), setuid(), umask(), etc. Must stay
  // in the mutable zone (page 1+), never in the sealed page 0.
  // ==================================================================

  windows_identity::ProcessIdentityState identity;

  // ==================================================================
  // SIGNAL SUBSYSTEM
  //
  // Canonical process-wide signal state: handlers, dispatch routing,
  // latest SIGCHLD snapshot, stop coordination, inherited child state,
  // and ALPC transport cache.
  // ==================================================================

  signal_state::SignalHandlerState signal_handler;
  signal_state::SignalDispatchState signal_dispatch;
  signal_state::SignalSigchldState signal_sigchld;
  signal_state::SignalTransportState signal_transport;
  signal_state::SignalChildState signal_child;
  signal_state::SignalStopState signal_stop;
  internal::AlpcBusState alpc_bus;

  // ==================================================================
  // CONSOLE / TTY POLICY
  // ==================================================================

  internal::ConsoleProcessState console;
  internal::ConsoleTtyState console_tty;

  // ==================================================================
  // ENVIRONMENT
  // ==================================================================

  internal::EnvironmentState environment;

  // ==================================================================
  // MEMORY SUBSYSTEM
  // ==================================================================

  // ------------------------------------------------------------------
  // Program break (brk/sbrk) — demand-growing linear heap segment.
  //
  // Sits directly on page_alloc.h primitives, independent of mmap and
  // the mapping table. The reservation starts small (one allocation
  // granularity = 64 KB) and extends into adjacent VA on demand.
  // ------------------------------------------------------------------
  internal::BrkProcessState brk;

  // ------------------------------------------------------------------
  // Mapping table — runtime-mutable half.
  //
  // The sealed-for-life half (l1 / l1_size / max_view_base /
  // remap_guards) lives in Zone 0 as g_pcb.zone0.mapping_table_*;
  // that half is written exactly once during Tier A Phase 0c.5 and
  // stays PAGE_READONLY for the process lifetime. This Zone 1 member
  // holds the mutable bookkeeping (init-state latch, in-flight remap
  // counter, diagnostic counters, guard-array watermark).
  // ------------------------------------------------------------------
  windows::memory::MappingTableProcessState mapping_table;

  // ==================================================================
  // THREADS — registry + wait infrastructure
  // ==================================================================

  // Thread registry — flat slab + bitmap + hash index.
  ThreadRegistryState thread_registry;

  // Main thread pre-allocated signal + lifecycle state. Avoids heap
  // allocation for the initial thread and stores the canonical types inline.
  MainThreadState main_thread;

  // ==================================================================
  // REACTOR — fixed kernel objects + metadata
  //
  // SlabPool for reactor slots stays external (growable).
  // ==================================================================

  internal::reactor::ReactorState reactor;

  // ==================================================================
  // RESOURCE LIMITS (was RlimitState)
  // ==================================================================

  windows::RlimitProcessState rlimit;

  // ==================================================================
  // VEH — mutable half only. The sealed half (filter table + TLS indices)
  // lives in Zone 0 at g_pcb.zone0.veh_sealed() and is hardware-protected
  // for process lifetime. handler_handle is the only field that mutates
  // post-init (dll_load_callback re-issues RtlAddVectoredExceptionHandler
  // on every DLL load to maintain front-of-chain).
  // ==================================================================

  windows::VehMutableState veh_mutable;

  // ==================================================================
  // MEMORY PROTECTION KEYS (was PkeyState — partial)
  //
  // The 16-key allocation bitmap and per-key rights are spec-bounded
  // (hardware limit). The range table is workload-bounded and stays
  // external behind pkey_range_table pointer.
  // ==================================================================

  windows::PkeyProcessState pkey;

  // ==================================================================
  // INTERVAL TIMER (was RealTimerState)
  // ==================================================================

  internal::ItimerState itimer;
  // Note: ReactorToken (16 B) stays file-local in itimer_ops.cpp —
  // it is implementation plumbing, not observable process state.

  // ==================================================================
  // TLS CLEANUP TABLE
  // ==================================================================

  internal::TlsCleanupState tls_cleanup;

  // ==================================================================
  // NLS
  //
  // Raw blob base plus decoded pointers into the read-only kernel NLS
  // data. Set once during lazy init (callonce), const after. Locale
  // globals (c_locale, global_locale) stay in upstream
  // src/locale/locale.cpp.
  // ==================================================================

  internal::NlsState nls;

  // ==================================================================
  // IPC — small fixed state
  // ==================================================================

  internal::IpcProcessState ipc;

  // ==================================================================
  // CHILD TABLE — fixed metadata only
  //
  // ChildEntry pool and pgid_index are workload-bounded (growable),
  // so they stay external. Only the fixed process-lifetime metadata
  // (lock, list head, event handle) lives here.
  // ==================================================================

  process::ChildTableState child_table;

  // ==================================================================
  // EXEC / PROCESS
  // ==================================================================

  internal::StartupProcessState startup;
  internal::ExecProcessState exec;
};

// =========================================================================
// Layout validation
//
// These assertions catch silent field drift across compiler versions or
// struct reorganizations. Modeled after the NT PEB/TEB static_asserts.
// =========================================================================

// PcbZone0 internal layout — offsets within the zone itself.
// Friend struct is used so offsetof can access private members.
struct PcbZone0LayoutCheck {
  static_assert(offsetof(PcbZone0, session_id_) == 40,
                "Session id must follow PCB constants block");
  static_assert(offsetof(PcbZone0, nt_build_) == 48,
                "NT build must follow session id");
  static_assert(offsetof(PcbZone0, capabilities_) == 52,
                "Capabilities must follow nt_build (pinned for nt_caps_fast)");
  static_assert(offsetof(PcbZone0, capabilities_) ==
                    internal_nt_caps::kPcbCapabilitiesOffset,
                "nt_caps_fast offset must match PcbZone0::capabilities_");
  static_assert(offsetof(PcbZone0, optional_) == 56,
                "Optional syscall table must follow capability flags");
  static_assert(offsetof(PcbZone0, mapping_table_max_view_base_) == 72,
                "Mapping-table max_view_base must follow optional syscall table");
  static_assert(offsetof(PcbZone0, mapping_table_l1_size_) == 80,
                "Mapping-table l1_size must follow max_view_base");
  static_assert(offsetof(PcbZone0, mapping_table_l1_) == 88,
                "Mapping-table l1 pointer must follow l1_size");
  static_assert(offsetof(PcbZone0, mapping_table_remap_guards_) == 96,
                "Mapping-table remap_guards pointer must follow l1");
  static_assert(offsetof(PcbZone0, veh_sealed_) == 104,
                "Sealed VEH state must follow mapping-table handles");
  // Substrate fields follow veh_sealed_; their exact offsets depend on
  // sizeof(VehSealedState) so we assert relative ordering rather than
  // absolute offsets.
  static_assert(offsetof(PcbZone0, substrate_secret_) ==
                    offsetof(PcbZone0, veh_sealed_) +
                        sizeof(windows::VehSealedState),
                "substrate_secret_ must immediately follow veh_sealed_");
  static_assert(offsetof(PcbZone0, substrate_token_key_) ==
                    offsetof(PcbZone0, substrate_secret_) + sizeof(uintptr_t),
                "substrate_token_key_ must follow substrate_secret_");
  static_assert(offsetof(PcbZone0, substrate_root_) ==
                    offsetof(PcbZone0, substrate_token_key_) +
                        sizeof(uintptr_t),
                "substrate_root_ must follow substrate_token_key_");
};

struct PcbZone0bLayoutCheck {
  static_assert(offsetof(PcbZone0b, pid_) == 0,
                "PID must be at start of Zone 0b");
  static_assert(offsetof(PcbZone0b, parent_pid_) == 4,
                "parent_pid follows pid");
  static_assert(offsetof(PcbZone0b, security_cookie_) == 8,
                "Security cookie follows parent_pid");
  static_assert(offsetof(PcbZone0b, dll_notify_cookie_) == 32,
                "dll_notify_cookie must follow zone_canary");
};

// Both zones are exactly one page (4096 bytes on x64).
static_assert(sizeof(PcbZone0) == 4096,
              "PcbZone0 must be exactly one page");
static_assert(sizeof(PcbZone0b) == 4096,
              "PcbZone0b must be exactly one page");

// PCB struct must be page-aligned so pcb_seal_readonly_a/b can call
// NtProtectVirtualMemory(&g_pcb.zone0|zone0b, page_size, PAGE_READONLY)
// without overlapping the surrounding section. PageSize is 4 KB on every
// supported architecture (x64 + AArch64 user-mode); LargePage isn't used
// here. The alignas(4096) on ProcessControlBlock pins this, and we
// double-check the in-instance alignment statically.
static_assert(alignof(ProcessControlBlock) >= 4096,
              "ProcessControlBlock must be 4 KB-aligned for zone seals");
static_assert(FIELD_OFFSET(ProcessControlBlock, zone0) == 0,
              "zone0 must be the first member (page-0 base)");

// Zone 0b starts at page 1; Zone 1 starts at page 2.
static_assert(FIELD_OFFSET(ProcessControlBlock, zone0b) == 4096,
              "Zone 0b must start at page 1 (offset 4096)");
static_assert(FIELD_OFFSET(ProcessControlBlock, init_state) == 8192,
              "Zone 1 must start at page 2 (offset 8192)");

// The PCB must be trivially destructible — no cleanup on process exit.
// Handles and regions are released by subsystem fini functions, not dtors.
static_assert(__is_trivially_destructible(ProcessControlBlock),
              "PCB must be trivially destructible — no implicit cleanup");

// PcbZone0 / PcbZone0b must also be trivially destructible.
static_assert(__is_trivially_destructible(PcbZone0),
              "PcbZone0 must be trivially destructible");
static_assert(__is_trivially_destructible(PcbZone0b),
              "PcbZone0b must be trivially destructible");

// Note: ProcessControlBlock is NOT trivially constructible because RawMutex
// has a constexpr user-provided constructor (initializes futex to UNLOCKED=0).
// This is intentional — the `= {}` initializer produces the same state as
// zero-fill, and the PE loader's demand-zero + value-initialization gives
// correct initial state for all fields including the mutexes.

// =========================================================================
// Global instance
// =========================================================================

// Defined in process_control_block.cpp, placed in the ".pcb" PE section.
// constinit guarantees the zero-initializer is resolved at compile time —
// no dynamic init ordering surprises. Internal-only: g_pcb is not exported
// from c.dll (it holds sealed process-wide state that POSIX consumers have
// no business reading directly).
extern ProcessControlBlock g_pcb;

// =========================================================================
// Protection zone operations
// =========================================================================

// Seal Zone 0 (page 0) as PAGE_READONLY at end of Tier A. Called once;
// never paired with an unseal in normal operation. Returns true on success.
[[nodiscard]] bool pcb_seal_readonly_a();

// Seal Zone 0b (page 1) as PAGE_READONLY at end of Tier A.
[[nodiscard]] bool pcb_seal_readonly_b();

// Unseal Zone 0b only — used by libc_fork_reinit() and veh_core fini to
// rewrite pid / cookies / dll-notify cookie. Zone 0a is never unsealed
// (no API exposes that — see process_control_block.cpp).
[[nodiscard]] bool pcb_unseal_readonly_b();

// True iff a write into PcbZone0b is currently safe — either we are in the
// pre-Tier-A bring-up window (PCB pages still demand-zero PAGE_READWRITE),
// or someone has opened a pcb_unseal_readonly_b()/pcb_seal_readonly_b()
// pair around their write batch. Used by debug-only assertions in
// PcbInitAccess Zone 0b setters; the production path treats this as
// hardware-enforced (a stray write to sealed Zone 0b raises an AV that
// the master VEH does not catch).
[[nodiscard]] bool zone0b_writable_now();

// Validate the Zone 0b canary via the public accessor on PcbZone0b.
LIBC_INLINE bool pcb_check_canary() { return g_pcb.zone0b.check_canary(); }

// =========================================================================
// Init-state observability AND phase-ordering enforcement
//
// Values grow strictly monotonically. The intermediate Tier A phases
// exist so that each Tier A init function can assert its immediate
// predecessor ran — turning the libc_bootstrap.cpp call ordering from
// an implicit, source-order-only invariant into a runtime-checked
// state machine. A future refactor that reorders bootstrap calls (or
// a new caller that invokes a phase out of order) trips the assert at
// first run instead of leaving a subtle latent bug.
//
// Phase contract (Tier A, all run single-threaded under loader lock):
//   None            -- pre-bootstrap.
//   TierA_VehUp     -- master VEH + reentry-guard TLS installed; any
//                      fault from here on routes through our handler.
//   TierA_PcbWritten -- PCB Zone 0 constants written (page_size,
//                       addresses, module_handle, nt_build, caps, ...).
//   TierA_MappingTable -- mapping table + region pool live; their Zone 0
//                         handles (l1_, remap_guards_, geometry) are
//                         written but Zone 0 is not yet sealed. Pre-
//                         requisite for any subsystem that wants to
//                         register VA into the mapping table during
//                         later Tier A phases.
//   TierA_MlockPolicy -- onfault state constructed. Gate that
//                        register_all_static_veh_filters() reads so it
//                        cannot install the mlock filter before the
//                        state it dereferences exists.
//   TierA_FiltersInstalled -- .libcveh sweep complete; every filter in
//                             the sealed table is live.
//   TierA_Identity  -- uid/gid/privilege + parent_pid published.
//   TierA           -- Zone 0/0b sealed PAGE_READONLY. Tier A complete.
//   TierB           -- allocator, pools, fd table, signal, reactor,
//                      stdio, ALPC bus up. Fully initialised.
//
// `pcb_init_state_advance_checked(expected, next)` pins the edge: it
// fastfails via NtTerminateProcess if the current state is not the
// expected predecessor. Used exclusively during bootstrap to lock the
// ordering in place. `pcb_init_state_advance` remains for the loose
// Tier A / Tier B publish calls and for test harnesses.
// =========================================================================

enum class PcbInitState : uint32_t {
  None = 0,
  TierA_VehUp = 1,
  TierA_PcbWritten = 2,
  TierA_MappingTable = 3,
  TierA_MlockPolicy = 4,
  TierA_FiltersInstalled = 5,
  TierA_Identity = 6,
  TierA = 7,
  TierB = 8,
};

LIBC_INLINE PcbInitState pcb_init_state() {
  return static_cast<PcbInitState>(
      g_pcb.init_state.load(cpp::MemoryOrder::ACQUIRE));
}

LIBC_INLINE void pcb_init_state_advance(PcbInitState state) {
  g_pcb.init_state.store(static_cast<uint32_t>(state),
                         cpp::MemoryOrder::RELEASE);
}

// Hard-fail CAS on the init-state field. If the observed state is not
// `expected`, the caller violated the bootstrap phase contract — a
// future reorderer, or an out-of-sequence invocation — and we terminate
// rather than ship with whatever state invariants the skipped phase
// was supposed to establish.
void pcb_init_state_advance_checked(PcbInitState expected,
                                    PcbInitState next);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_H
