//===-- Hardware breakpoint / watchpoint API (x86-64, Win11+) ----*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin NT-native wrapper over the x86 debug registers (DR0..DR3, DR6, DR7)
// that exposes up to four concurrent hardware watch/break points per thread.
//
// Design:
//   - Hardware is the canonical state. Free-slot discovery reads DR7's L0..L3
//     enable bits; we don't keep a shadow table. This costs zero persistent
//     memory per thread (the ThreadLifecycle is untouched) and survives fork
//     correctly — the child starts with DR7=0 per the architecture.
//   - Cross-thread arm/disarm suspends the target via the Win11-only
//     NtCreateThreadStateChange / NtChangeThreadState pair. That API is
//     crash-safe: if the arming thread dies before resume, the kernel auto-
//     resumes the target when the state-change handle closes.
//   - Self-arm skips the suspend and writes the thread context directly. On
//     x86-64 the kernel stages DR values into the saved context and installs
//     them on return-from-syscall; DR changes take effect the instant the
//     NtSetContextThread call returns.
//   - The VEH transport (signal/transport/veh_transport.cpp) already reads
//     DR6 to synthesize TRAP_HWBKPT vs TRAP_TRACE; see clear_dr6_status()
//     below for the sticky-bit reset that pairs with it.
//
// Pairing with SIGTRAP:
//   A hit delivers EXCEPTION_SINGLE_STEP, which VEH converts to SIGTRAP with
//   si_code=TRAP_HWBKPT (DR6 B0..B3 set). The signal handler sees si_addr =
//   rec->ExceptionAddress, which is the PC of the *triggering instruction*
//   for data-access traps and the watched PC for exec traps.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_HW_BREAKPOINT_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_HW_BREAKPOINT_H

#include "src/__support/OSUtil/windows/nt/nt_types.h" // HANDLE, CONTEXT
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace hw_bp {

// Access modes — map directly to DR7 R/Wn bits.
enum : unsigned {
  ACCESS_EXEC = 0,  // Instruction execution. len must be 1.
  ACCESS_WRITE = 1, // Data writes only.
  ACCESS_RW = 3,    // Data reads + writes (no execute-only read trap exists).
};

// Number of hardware slots on x86-64. DR4/DR5 are aliases for DR6/DR7; only
// DR0..DR3 are usable address slots.
inline constexpr unsigned NUM_SLOTS = 4;

// Per-slot state returned by query().
struct Watchpoint {
  void *addr;      // DRn address (undefined when !enabled).
  unsigned len;    // 1, 2, 4, 8 bytes (undefined when !enabled).
  unsigned access; // ACCESS_EXEC / ACCESS_WRITE / ACCESS_RW (undefined when
                   // !enabled).
  bool enabled;    // DR7 Ln bit.
};

// Arm a hardware watchpoint on `thread`.
//
//   thread:  NT thread handle with THREAD_GET_CONTEXT | THREAD_SET_CONTEXT
//            rights. Pass NtCurrentThread() for same-thread arming (skips
//            the suspend round-trip).
//   addr:    watched virtual address. Must be `len`-aligned.
//   len:     1 | 2 | 4 | 8 bytes. For ACCESS_EXEC, len must be 1.
//   access:  ACCESS_EXEC | ACCESS_WRITE | ACCESS_RW.
//
// Returns the assigned slot index (0..3) on success, or a negative errno:
//   -EINVAL   invalid len, access, or misaligned addr
//   -EAGAIN   all 4 slots already in use
//   -EPERM    kernel denied context access (handle lacks rights)
//   -EIO      NT context read/write failed
int arm(HANDLE thread, void *addr, unsigned len, unsigned access);

// Disarm the single slot. slot must be in [0, NUM_SLOTS). Zeroes DRn, clears
// DR7 Ln + R/Wn + LENn fields for that slot. Returns 0 on success, or a
// negative errno matching arm().
int disarm(HANDLE thread, unsigned slot);

// Disarm every slot on `thread` in one context round-trip. Useful on thread
// exit, fork-child reset, or debugger detach.
int disarm_all(HANDLE thread);

// Snapshot the four slots of `thread` into `out`. Returns 0 on success, or
// -EIO if the context read failed.
int query(HANDLE thread, Watchpoint out[NUM_SLOTS]);

// Clear DR6 B0..B3 and BS (bit 14) on an in-hand CONTEXT. Intel leaves the
// status bits sticky: hardware sets them on a hit and only a debugger (or
// this) ever clears them. Call this from the VEH transport after reading
// DR6 to decide TRAP_TRACE vs TRAP_HWBKPT, so the next trap shows fresh
// cause bits rather than a stale OR.
//
// The CONTEXT must have CONTEXT_DEBUG_REGISTERS set in ContextFlags; on the
// VEH path it does. Writes flow back to the thread on
// EXCEPTION_CONTINUE_EXECUTION.
void clear_dr6_status(CONTEXT *ctx);

} // namespace hw_bp
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_HW_BREAKPOINT_H
