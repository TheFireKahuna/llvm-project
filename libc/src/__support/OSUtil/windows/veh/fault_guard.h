//===-- VEH fault guard for libc-internal page probes ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thread-local, nestable fault recovery for libc code that must probe memory
// (madvise CoW triggers, etc.) or call foreign code that may fault (atexit
// destructors from third-party DLLs).
//
// Design:
//   A FaultGuard is a stack-allocated struct with a jmp_buf, a prev pointer
//   (for nesting), and an exception_mask selecting which hardware faults to
//   intercept. The per-thread chain head lives in a TEB inline TLS slot —
//   single-instruction read, no TLS emulation, fork-safe (child gets null).
//
//   The VEH master handler checks the chain head BEFORE the reentry guard
//   and filter table walk. If a matching fault occurs, it pops the guard
//   and longjmps to the setjmp site. Non-matching faults (C++ exceptions,
//   debugger traps, CLR, etc.) fall through to normal dispatch.
//
// Usage:
//   FaultGuard guard;
//   if (fault_guard_enter(&guard, mask)) {
//     // faulted — guard.exception_code has the raw NTSTATUS
//     return -EIO;
//   }
//   // ... dangerous operation ...
//   fault_guard_leave(&guard);
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_FAULT_GUARD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_FAULT_GUARD_H

#include "src/__support/OSUtil/windows/tls/teb_tls.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"
#include "src/setjmp/setjmp_impl.h"

#include "hdr/types/jmp_buf.h"
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

struct FaultGuard {
  jmp_buf buf;
  FaultGuard *prev;          // nesting chain
  uint32_t exception_mask;   // VehExceptionBit bitmask — which faults to catch
  DWORD exception_code;      // raw NTSTATUS, filled by VEH on fault
};

/// Common masks for callers.

/// Memory probe: only access violation + in-page error.
inline constexpr uint32_t FAULT_GUARD_MEMORY =
    VEH_ACCESS_VIOLATION | VEH_IN_PAGE_ERROR;

/// Destructor guard: all hardware faults except debugger traps and stack
/// overflow. Stack overflow is excluded because longjmp (RtlUnwindEx) needs
/// stack space to run — catching it here would cause infinite VEH recursion.
/// Let it fall through to the OS crash handler for a clean minidump.
/// C++/CLR/COM exceptions have codes outside the bitmask table and pass
/// through to their own personalities automatically.
inline constexpr uint32_t FAULT_GUARD_DTOR =
    VEH_ALL_SIGNAL & ~VEH_BREAKPOINT & ~VEH_SINGLE_STEP & ~VEH_STACK_OVERFLOW;

//===----------------------------------------------------------------------===//
// TEB slot accessors — the TLS index is stored in VehState and passed in
// from the master handler / call-site code.
//===----------------------------------------------------------------------===//

LIBC_INLINE FaultGuard *get_fault_guard(unsigned tls_index) {
  return static_cast<FaultGuard *>(internal::teb_tls_get(tls_index));
}

LIBC_INLINE void set_fault_guard(unsigned tls_index, FaultGuard *g) {
  internal::teb_tls_set(tls_index, static_cast<void *>(g));
}

//===----------------------------------------------------------------------===//
// Enter / leave API
//===----------------------------------------------------------------------===//

/// Enter a fault-guarded region. Returns false on normal setup; returns
/// true on fault. On fault, the raw NTSTATUS exception code is available
/// in g->exception_code (e.g. EXCEPTION_ACCESS_VIOLATION = 0xC0000005).
///
/// The caller must call fault_guard_leave() on the normal (non-fault) path.
/// On the fault path, the guard is already popped by the VEH handler.
[[gnu::returns_twice]]
LIBC_INLINE bool fault_guard_enter(FaultGuard *g, unsigned tls_index,
                                   uint32_t mask) {
  g->exception_mask = mask;
  g->exception_code = 0;
  g->prev = get_fault_guard(tls_index);
  set_fault_guard(tls_index, g);
  if (setjmp(g->buf) != 0) {
    // longjmp'd here from VEH — guard already popped, exception_code filled.
    return true;
  }
  return false;
}

/// Leave a fault-guarded region on the normal (no-fault) path.
LIBC_INLINE void fault_guard_leave(FaultGuard *g, unsigned tls_index) {
  set_fault_guard(tls_index, g->prev);
}

//===----------------------------------------------------------------------===//
// Convenience overloads — read TLS index from g_pcb.veh automatically.
//===----------------------------------------------------------------------===//

[[gnu::returns_twice]]
LIBC_INLINE bool fault_guard_enter(FaultGuard *g, uint32_t mask) {
  return fault_guard_enter(g, g_pcb.veh.fault_guard_tls_index, mask);
}

LIBC_INLINE void fault_guard_leave(FaultGuard *g) {
  fault_guard_leave(g, g_pcb.veh.fault_guard_tls_index);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_FAULT_GUARD_H
