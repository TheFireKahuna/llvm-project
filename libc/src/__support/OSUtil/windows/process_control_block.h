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
//   Page 0 (PcbZone0) holds read-only-after-init constants. Fields are
//   private — only PcbInitAccess (defined in pcb_init_access.h) can write
//   them. After init, page 0 is sealed PAGE_READONLY via
//   NtProtectVirtualMemory. Remaining pages stay PAGE_READWRITE.
//
// Write discipline:
//   Zone 0: compile-time enforced via private fields + friend PcbInitAccess.
//           Runtime-enforced via PAGE_READONLY seal after init.
//   Zone 1: subsystem code writes through accessor headers (security.h,
//           page_size.h, etc.) — leaf code never includes this header.
//           cpp::Atomic<> on mutable fields prevents accidental plain stores.
//
// PE section:
//   Placed in ".pcb" via #pragma section + __declspec(allocate). The NT
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
#include "src/__support/OSUtil/windows/io/environment_state.h"
#include "src/__support/OSUtil/windows/nt/nt_capabilities.h"
#include "src/__support/OSUtil/windows/ipc/ipc_process_state.h"
#include "src/__support/OSUtil/windows/memory/brk_process_state.h"
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

// Canonical inline state for the process's initial thread. This keeps the main
// thread's long-lived signal and lifecycle objects in the PCB with their real
// types instead of byte storage.
struct MainThreadState {
  signal_state::ThreadSignalState signal;
  ThreadLifecycle lifecycle;
};

// =========================================================================
// Security
// =========================================================================

// XOR salt for the zone canary between read-only and mutable regions.
// The canary (security_cookie ^ PCB_CANARY_MAGIC) detects accidental
// linear buffer overflows — it is NOT a security boundary against
// targeted attacks, since both the salt and cookie are readable.
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
  [[nodiscard]] LIBC_INLINE uintptr_t security_cookie() const {
    return security_cookie_;
  }
  [[nodiscard]] LIBC_INLINE uintptr_t security_cookie_complement() const {
    return security_cookie_complement_;
  }
  [[nodiscard]] LIBC_INLINE void *dso_handle() const { return dso_handle_; }
  [[nodiscard]] LIBC_INLINE pid_t pid() const { return pid_; }
  [[nodiscard]] LIBC_INLINE DWORD parent_pid() const { return parent_pid_; }

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

  // Canary validation — public read, safe for any caller.
  [[nodiscard]] LIBC_INLINE bool check_canary() const {
    return zone_canary_ == (security_cookie_ ^ PCB_CANARY_MAGIC);
  }

private:
  // --- Cache line 0: read-only constants (56 bytes) ---

  uint32_t page_size_;                         //   4 B
  uint32_t alloc_granularity_;                 //   4 B
  void *min_address_;                          //   8 B
  void *max_address_;                          //   8 B
  void *module_handle_;                        //   8 B  HINSTANCE of c.dll
  uintptr_t security_cookie_;                  //   8 B
  uintptr_t security_cookie_complement_;       //   8 B
  void *dso_handle_;                           //   8 B  __dso_handle
                                               // = 56 B

  // Zone canary: security_cookie_ ^ PCB_CANARY_MAGIC.
  // Detects linear overflows from adjacent data into the mutable zone.
  uintptr_t zone_canary_;                      //   8 B
                                               // = 64 B (one cache line)

  // --- Process identity (immutable after init, updated on fork) ---

  pid_t pid_;                                  //   4 B
  DWORD parent_pid_;                           //   4 B
                                               // = 72 B

  // --- NT capability detection (populated in Phase 0, sealed with zone) ---

  uint32_t nt_build_;                          //   4 B  unshimmed build number
  uint32_t capabilities_;                      //   4 B  nt_cap:: bitmask
  NtOptionalSyscalls optional_;                //  16 B  sealed function pointers
                                               // = 96 B total data

  // Explicit padding to page boundary. All mutable fields MUST start at
  // page 1+ (offset 4096). The static_assert below verifies this.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
  uint8_t _pad[4096 - 96]; // Structural padding — forces sizeof == 4096.
#pragma clang diagnostic pop
};

struct ProcessControlBlock {
  // ==================================================================
  // ZONE 0: READ-ONLY AFTER INIT (one page, sealed PAGE_READONLY)
  //
  // Fields are private in PcbZone0. Only PcbInitAccess can write.
  // After pcb_seal_readonly() (Phase 9 of __libc_dll_init()), writes become access violations.
  // ==================================================================

  PcbZone0 zone0;

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
  signal_state::SignalAlpcState signal_alpc;

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
  // VEH — exception filter table + handles
  // ==================================================================

  windows::VehState veh;

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
  static_assert(offsetof(PcbZone0, zone_canary_) == 56,
                "Read-only constants must end at byte 56 (before canary)");
  static_assert(offsetof(PcbZone0, pid_) == 64,
                "PID must begin at byte 64 (after canary)");
  static_assert(offsetof(PcbZone0, nt_build_) == 72,
                "NT build must follow process identity");
  static_assert(offsetof(PcbZone0, optional_) == 80,
                "Optional syscall table must follow capability flags");
};

// PcbZone0 is exactly one page (4096 bytes on x64).
static_assert(sizeof(PcbZone0) == 4096,
              "PcbZone0 must be exactly one page");

// Zone 1 starts at the page boundary in the outer struct.
static_assert(FIELD_OFFSET(ProcessControlBlock, identity) == 4096,
              "Zone 1 must start at page boundary (offset 4096)");

// The PCB must be trivially destructible — no cleanup on process exit.
// Handles and regions are released by subsystem fini functions, not dtors.
static_assert(__is_trivially_destructible(ProcessControlBlock),
              "PCB must be trivially destructible — no implicit cleanup");

// PcbZone0 must also be trivially destructible (it's embedded in PCB).
static_assert(__is_trivially_destructible(PcbZone0),
              "PcbZone0 must be trivially destructible");

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
// no dynamic init ordering surprises.
extern ProcessControlBlock g_pcb;

// =========================================================================
// Protection zone operations
// =========================================================================

// Seal page 0 as PAGE_READONLY after init completes. Called once from
// the last CRT init function. Returns true on success. Failure means
// security-critical fields remain writable — callers must handle this.
[[nodiscard]] bool pcb_seal_readonly();

// Unseal page 0 to PAGE_READWRITE. Used by fork reinit to update PID
// and identity fields. Caller must re-seal after writing.
[[nodiscard]] bool pcb_unseal_readonly();

// Validate the zone canary via the public accessor on PcbZone0.
// Convenience wrapper — equivalent to g_pcb.zone0.check_canary().
LIBC_INLINE bool pcb_check_canary() { return g_pcb.zone0.check_canary(); }

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_PROCESS_CONTROL_BLOCK_H
