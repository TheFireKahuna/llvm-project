//===-- Windows x64 implementation of longjmp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Windows x64 longjmp must unwind through intermediate frames to invoke
// C++ destructors and SEH __finally blocks. A raw register restore would
// skip all cleanup. RtlUnwindEx is the OS unwinder -- it handles all frame
// types (Itanium, SEH, system DLLs) and never returns.

#include "src/setjmp/longjmp.h"
#include "include/llvm-libc-macros/offsetof-macro.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#if !defined(LIBC_TARGET_ARCH_IS_X86_64)
#error "Invalid file include"
#endif

#if !defined(LIBC_TARGET_OS_IS_WINDOWS)
#error "This file is Windows-specific"
#endif

#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"

namespace LIBC_NAMESPACE_DECL {

// Weak reference: when linked into a full program with c.dll, the signal
// subsystem provides this. In standalone test binaries it resolves to
// nullptr (returns no thread state, skipping the SS_ONSTACK cleanup).
namespace signal_state {
extern "C" __LIBC_SELECTANY_ATTR
    ThreadSignalState *(*get_thread_state_noinit_ptr)() = nullptr;
} // namespace signal_state

LLVM_LIBC_FUNCTION(void, longjmp, (jmp_buf buf, int val)) {
  auto *b = reinterpret_cast<__jmp_buf *>(buf);
  if (val == 0)
    val = 1;

  // Clear SS_ONSTACK if we're jumping out of a signal handler that was
  // executing on the alternate signal stack. On Linux the kernel clears
  // this via sigreturn; on Windows it's purely userspace state, so longjmp
  // must do it explicitly.
  if (signal_state::get_thread_state_noinit_ptr) {
    auto *sig_state = signal_state::get_thread_state_noinit_ptr();
    if (sig_state)
      sig_state->alt_stack_flags &= static_cast<unsigned short>(
          ~static_cast<unsigned short>(SS_ONSTACK));
  }

  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;

  // SysV AMD64 ABI: callee-saved integer regs are rbx, rbp, r12-r15 (plus
  // rsp/rip for control flow). Rdi, Rsi, and Xmm6-Xmm15 are caller-saved
  // under SysV, so setjmp does not capture them and longjmp leaves the
  // corresponding CONTEXT fields zero-initialized. RtlUnwindEx will write
  // those zeros into the target's registers, which is safe because the
  // SysV code at the longjmp target treats those registers as volatile.
  ctx.Rbx = b->rbx;
  ctx.Rbp = b->rbp;
  ctx.R12 = b->r12;
  ctx.R13 = b->r13;
  ctx.R14 = b->r14;
  ctx.R15 = b->r15;
  ctx.Rsp = b->rsp;
  ctx.Rip = b->rip;

  // FP control -- set both top-level and FltSave copies. MXCSR control bits
  // and x87 FCW are callee-preserved under psABI §3.2.1.
  ctx.MxCsr = b->mxcsr;
  ctx.FltSave.MxCsr = b->mxcsr;
  ctx.FltSave.ControlWord = b->fpcw;

  RtlUnwindEx(
      reinterpret_cast<PVOID>(b->frame),
      reinterpret_cast<PVOID>(b->rip),
      nullptr, // ExceptionRecord
      reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(val)),
      &ctx,
      nullptr); // HistoryTable
  __builtin_unreachable();
}

} // namespace LIBC_NAMESPACE_DECL
