//===-- Async-signal-safe stack walker for x64 PE images --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Custom table-driven stack walker that reads .pdata/.xdata directly from
// mapped PE images. Zero allocations, zero syscalls, zero locks.
//
// Properties:
//   - Async-signal-safe: reads only mapped PE metadata and TEB fields.
//   - Fork-safe: PE mappings are COW-stable, no kernel-maintained tables.
//   - Works from any source: captured context, exception CONTEXT, or
//     signal ucontext_t's interrupted_context.
//
// The walker only tracks RSP + GPR state (144 bytes). XMM/XSTATE are
// irrelevant for stack walking and are never touched.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_STACK_WALKER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_STACK_WALKER_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h" // CONTEXT, DWORD64
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// Maximum frames the walker will capture before stopping.
inline constexpr int MAX_WALK_FRAMES = 256;

/// Lightweight register state for stack walking (144 bytes).
/// Only GPRs — no FP/XMM. Indexed by x64 register number:
///   0=RAX, 1=RCX, 2=RDX, 3=RBX, 4=RSP, 5=RBP,
///   6=RSI, 7=RDI, 8=R8 ... 15=R15.
struct WalkState {
  uintptr_t rip;
  uintptr_t rsp;
  uintptr_t gprs[16];
};

/// Walk the stack from the given register state.
///
/// Fills \p buffer with return addresses, skipping the first \p skip frames.
/// Returns the number of frames stored in buffer (at most \p max_frames).
///
/// FaultGuard wraps the walk: if any memory read during the walk faults
/// (corrupt stack, unmapped page), the walk terminates gracefully and
/// returns however many frames were captured before the fault.
int posix_stack_walk(void **buffer, int max_frames, int skip,
                     const WalkState &initial);

#ifdef __x86_64__
/// Initialize a WalkState from a CONTEXT (crash handler, signal handler).
LIBC_INLINE void walk_state_from_context(WalkState &state,
                                         const CONTEXT *ctx) {
  state.rip = ctx->Rip;
  state.rsp = ctx->Rsp;
  state.gprs[0] = ctx->Rax;
  state.gprs[1] = ctx->Rcx;
  state.gprs[2] = ctx->Rdx;
  state.gprs[3] = ctx->Rbx;
  state.gprs[4] = ctx->Rsp;
  state.gprs[5] = ctx->Rbp;
  state.gprs[6] = ctx->Rsi;
  state.gprs[7] = ctx->Rdi;
  state.gprs[8] = ctx->R8;
  state.gprs[9] = ctx->R9;
  state.gprs[10] = ctx->R10;
  state.gprs[11] = ctx->R11;
  state.gprs[12] = ctx->R12;
  state.gprs[13] = ctx->R13;
  state.gprs[14] = ctx->R14;
  state.gprs[15] = ctx->R15;
}
#endif

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_STACK_WALKER_H
