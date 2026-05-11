//===-- Unified VEH dispatch framework ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single process-global VEH handler with a priority-sorted dispatch table.
// Subsystems register filter callbacks instead of managing independent VEH
// handlers. One DLL-load notification callback maintains front-of-chain
// position using add-then-remove ordering (never absent from the chain).
//
// Eliminates:
//   - Multiple independent RtlAddVectoredExceptionHandler(1, ...) calls
//   - Per-subsystem DLL-load re-registration callbacks
//   - Cross-subsystem VEH coordination (signal calling memory infrastructure)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_CORE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_CORE_H

#include "src/__support/OSUtil/windows/veh/veh_state.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Exception code to bitmask encoding
//===----------------------------------------------------------------------===//
//
// Windows exception codes are 32-bit NTSTATUS values. A direct bitmask is
// impossible, so we define a small enum of exception categories mapped to
// bit positions. The master handler maps NTSTATUS -> bit via a switch.

enum VehExceptionBit : uint32_t {
  VEH_ACCESS_VIOLATION    = 1u << 0,   // 0xC0000005
  VEH_GUARD_PAGE          = 1u << 1,   // 0x80000001
  VEH_INT_DIVIDE_BY_ZERO  = 1u << 2,   // 0xC0000094
  VEH_INT_OVERFLOW        = 1u << 3,   // 0xC0000095
  VEH_FLT_DIVIDE_BY_ZERO  = 1u << 4,   // 0xC000008E
  VEH_FLT_OVERFLOW        = 1u << 5,   // 0xC0000091
  VEH_FLT_UNDERFLOW       = 1u << 6,   // 0xC0000093
  VEH_FLT_INEXACT         = 1u << 7,   // 0xC000008F
  VEH_FLT_INVALID_OP      = 1u << 8,   // 0xC0000090
  VEH_FLT_DENORMAL        = 1u << 9,   // 0xC000008D
  VEH_FLT_STACK_CHECK     = 1u << 10,  // 0xC0000092
  VEH_ILLEGAL_INSTRUCTION = 1u << 11,  // 0xC000001D
  VEH_PRIV_INSTRUCTION    = 1u << 12,  // 0xC0000096
  VEH_BREAKPOINT          = 1u << 13,  // 0x80000003
  VEH_SINGLE_STEP         = 1u << 14,  // 0x80000004
  VEH_DATATYPE_MISALIGN   = 1u << 15,  // 0x80000002
  VEH_IN_PAGE_ERROR       = 1u << 16,  // 0xC0000006
  VEH_ARRAY_BOUNDS        = 1u << 17,  // 0xC000008C
  VEH_STACK_OVERFLOW      = 1u << 18,  // 0xC00000FD
};

/// All floating-point and integer arithmetic exceptions.
inline constexpr uint32_t VEH_ALL_FPE =
    VEH_INT_DIVIDE_BY_ZERO | VEH_INT_OVERFLOW |
    VEH_FLT_DIVIDE_BY_ZERO | VEH_FLT_OVERFLOW | VEH_FLT_UNDERFLOW |
    VEH_FLT_INEXACT | VEH_FLT_INVALID_OP | VEH_FLT_DENORMAL |
    VEH_FLT_STACK_CHECK;

/// All exception codes that the signal subsystem may convert to POSIX signals.
///
/// Deliberately omits VEH_GUARD_PAGE: STATUS_GUARD_PAGE_VIOLATION is a
/// stack-growth probe / mlock-policy passthrough, not a fault. The memory
/// (PRIORITY=10) and mlock (PRIORITY=15) filters claim it before the signal
/// filter (PRIORITY=20) would ever run, so including it in the signal mask
/// only widens the dispatch surface without serving a purpose. Worse, a
/// future refactor that reordered priorities or weakened the runtime stage-
/// 1b skip could silently start delivering legitimate guard-page probes as
/// SIGSEGV.
inline constexpr uint32_t VEH_ALL_SIGNAL =
    VEH_ACCESS_VIOLATION | VEH_STACK_OVERFLOW |
    VEH_DATATYPE_MISALIGN | VEH_IN_PAGE_ERROR | VEH_ARRAY_BOUNDS |
    VEH_ALL_FPE |
    VEH_ILLEGAL_INSTRUCTION | VEH_PRIV_INSTRUCTION |
    VEH_BREAKPOINT | VEH_SINGLE_STEP;

//===----------------------------------------------------------------------===//
// Registration API
//===----------------------------------------------------------------------===//
//
// All VEH filters are statically registered into the `.libcveh$M` PE section
// via LIBC_REGISTER_VEH_FILTER in their owning TU. The Tier A
// register_all_static_veh_filters() sweep populates the dispatch table in
// priority order before Zone 0 seal — there is no runtime register/
// unregister API. The filter table is sealed PAGE_READONLY for the rest of
// process lifetime.
//
// Subsystem-specific runtime gating (e.g. signal's "no SEH-class handler set
// → CONTINUE_SEARCH") is the filter handler's responsibility, not the
// dispatcher's.

/// Insert a static VEH filter into the dispatch table. Single-threaded use
/// only — called from the Tier A `.libcveh` sweep before Zone 0 seal. Not
/// callable post-init (filter table is in sealed memory).
bool insert_static_veh_filter(const VehFilter &filter);

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VEH_VEH_CORE_H
