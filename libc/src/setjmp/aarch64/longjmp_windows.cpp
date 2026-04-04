//===-- Windows ARM64 implementation of longjmp ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Windows ARM64 longjmp must unwind through intermediate frames to invoke
// C++ destructors and SEH __finally blocks. A raw register restore would
// skip all cleanup. RtlUnwindEx handles all frame types (Itanium, SEH,
// system DLLs) and never returns.

#include "src/setjmp/longjmp.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#if !defined(LIBC_TARGET_ARCH_IS_AARCH64)
#error "Invalid file include"
#endif

#if !defined(LIBC_TARGET_OS_IS_WINDOWS)
#error "This file is Windows-specific"
#endif

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/signal.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(void, longjmp, (jmp_buf buf, int val)) {
  auto *b = reinterpret_cast<__jmp_buf *>(buf);
  if (val == 0)
    val = 1;

  // Clear SS_ONSTACK if we're jumping out of a signal handler that was
  // executing on the alternate signal stack. On Linux the kernel clears
  // this via sigreturn; on Windows it's purely userspace state, so longjmp
  // must do it explicitly. Non-allocating lookup -- safe in any context.
  auto *sig_state = signal_state::get_thread_state_noinit();
  if (sig_state)
    sig_state->alt_stack_flags &=
        static_cast<unsigned short>(~static_cast<unsigned short>(SS_ONSTACK));

  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;

  // Callee-saved x19-x28 from opaque[0..9]
  ctx.X19 = static_cast<DWORD64>(b->opaque[0]);
  ctx.X20 = static_cast<DWORD64>(b->opaque[1]);
  ctx.X21 = static_cast<DWORD64>(b->opaque[2]);
  ctx.X22 = static_cast<DWORD64>(b->opaque[3]);
  ctx.X23 = static_cast<DWORD64>(b->opaque[4]);
  ctx.X24 = static_cast<DWORD64>(b->opaque[5]);
  ctx.X25 = static_cast<DWORD64>(b->opaque[6]);
  ctx.X26 = static_cast<DWORD64>(b->opaque[7]);
  ctx.X27 = static_cast<DWORD64>(b->opaque[8]);
  ctx.X28 = static_cast<DWORD64>(b->opaque[9]);

  // Frame pointer and link register
  ctx.Fp = static_cast<DWORD64>(b->opaque[10]);
  ctx.Lr = static_cast<DWORD64>(b->opaque[11]);
  ctx.Sp = static_cast<DWORD64>(b->opaque[12]);
  ctx.Pc = static_cast<DWORD64>(b->opaque[11]); // Return to setjmp's caller

  // Callee-saved FP registers: d8-d15 (low 64 bits of V8-V15).
#if __ARM_FP
  for (int i = 0; i < 8; ++i) {
    ctx.V[8 + i].Low = static_cast<ULONGLONG>(b->fopaque[i]);
    ctx.V[8 + i].High = 0;
  }
#endif

  // FP control/status
  ctx.Fpcr = b->fpcr;
  ctx.Fpsr = b->fpsr;

  RtlUnwindEx(
      reinterpret_cast<PVOID>(b->frame),
      reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(b->opaque[11])),
      nullptr,
      reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(val)),
      &ctx,
      nullptr);
  __builtin_unreachable();
}

} // namespace LIBC_NAMESPACE_DECL
