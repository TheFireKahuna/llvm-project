//===-- Signal delivery — preserved utilities ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities preserved from the original signal delivery engine. The core
// delivery logic (handler invocation, dispatch, pending management) has moved
// to the four-layer architecture:
//   Layer 1: pending/pending_storage.h
//   Layer 2: transport/{veh,console,apc}_transport.h
//   Layer 3: dispatch/dispatch_engine.h
//   Layer 4: control/process_control.h
//
// This file retains only context_win32_to_ucontext, which is used by the
// dispatch engine (Layer 3) and VEH transport (Layer 2a) via extern
// declarations.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/signal_internal.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// Convert Win32 CONTEXT to POSIX ucontext_t. Populates mcontext_t gregs
// from the platform register set and copies FP/SIMD state.
void context_win32_to_ucontext(const CONTEXT *win_ctx, ucontext_t *uc) {
  // Zero the output to ensure unused fields (uc_link, padding) are clean.
  __builtin_memset(uc, 0, sizeof(ucontext_t));

  mcontext_t *mc = &uc->uc_mcontext;

#ifdef __x86_64__
  mc->gregs[REG_R8] = win_ctx->R8;
  mc->gregs[REG_R9] = win_ctx->R9;
  mc->gregs[REG_R10] = win_ctx->R10;
  mc->gregs[REG_R11] = win_ctx->R11;
  mc->gregs[REG_R12] = win_ctx->R12;
  mc->gregs[REG_R13] = win_ctx->R13;
  mc->gregs[REG_R14] = win_ctx->R14;
  mc->gregs[REG_R15] = win_ctx->R15;
  mc->gregs[REG_RDI] = win_ctx->Rdi;
  mc->gregs[REG_RSI] = win_ctx->Rsi;
  mc->gregs[REG_RBP] = win_ctx->Rbp;
  mc->gregs[REG_RBX] = win_ctx->Rbx;
  mc->gregs[REG_RDX] = win_ctx->Rdx;
  mc->gregs[REG_RAX] = win_ctx->Rax;
  mc->gregs[REG_RCX] = win_ctx->Rcx;
  mc->gregs[REG_RSP] = win_ctx->Rsp;
  mc->gregs[REG_RIP] = win_ctx->Rip;
  mc->gregs[REG_EFL] = win_ctx->EFlags;

  const auto *src = reinterpret_cast<const unsigned char *>(&win_ctx->FltSave);
  for (unsigned i = 0; i < sizeof(mc->fpregs); ++i)
    mc->fpregs[i] = src[i];
#elif defined(__aarch64__)
  for (int i = 0; i < 29; ++i)
    mc->gregs[i] = win_ctx->X[i];
  mc->gregs[REG_X29] = win_ctx->Fp;
  mc->gregs[REG_X30] = win_ctx->Lr;
  mc->gregs[REG_SP] = win_ctx->Sp;
  mc->gregs[REG_PC] = win_ctx->Pc;
  mc->gregs[REG_PSTATE] = win_ctx->Cpsr;

  const auto *vsrc = reinterpret_cast<const unsigned char *>(win_ctx->V);
  for (unsigned i = 0; i < sizeof(mc->vregs); ++i)
    mc->vregs[i] = vsrc[i];
  mc->fpcr = win_ctx->Fpcr;
  mc->fpsr = win_ctx->Fpsr;
#endif

  // Fill uc_sigmask from current thread's blocked set.
  ThreadSignalState *thread = get_thread_state_noinit();
  if (thread)
    uc->uc_sigmask = thread->blocked_signals;

  // Fill uc_stack from thread's alt stack if configured.
  if (thread) {
    uc->uc_stack.ss_sp = thread->alt_stack_sp;
    uc->uc_stack.ss_flags = thread->alt_stack_flags;
    uc->uc_stack.ss_size = thread->alt_stack_size;
  }
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
