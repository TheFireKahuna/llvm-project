//===-- Windows x64 implementation of sigsetjmp ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Windows x64 sigsetjmp. No epilogue pattern -- mask save/restore is split:
//   sigsetjmp: saves the mask into buf->sigmask (if savesigs)
//   siglongjmp: restores it before calling longjmp
//
// This avoids the need to wrap setjmp in a non-naked function, which would
// create a frame mismatch with RtlUnwindEx. Instead, sigsetjmp is naked:
// it saves the mask via a helper, sets did_save_mask, then tail-calls setjmp.
// The caller's context is captured directly by the Windows sigsetjmp macro.

#define LLVM_LIBC_SIGSETJMP_DONT_DEFINE_MACRO
#include "src/setjmp/sigsetjmp.h"
#include "hdr/offsetof_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#define LLVM_LIBC_SETJMP_DONT_DEFINE_MACRO
#include "src/setjmp/setjmp_impl.h"
#undef LLVM_LIBC_SETJMP_DONT_DEFINE_MACRO
#undef LLVM_LIBC_SIGSETJMP_DONT_DEFINE_MACRO
#include "src/signal/sigprocmask.h"
#include "hdr/signal_macros.h"

#if !defined(LIBC_TARGET_ARCH_IS_X86_64)
#error "Invalid file include"
#endif

#if !defined(LIBC_TARGET_OS_IS_WINDOWS)
#error "This file is Windows-specific"
#endif

// Mirror the setjmp_windows.cpp pattern: the sigsetjmp macro in sigsetjmp.h
// expands to the extern-"C" `__llvm_libc_sigsetjmp`. Under PUBLIC_PACKAGING,
// LLVM_LIBC_FUNCTION already emits that alias. Without it (hermetic/unit
// __internal__ builds), emit the label ourselves at the naked-asm entry so
// callers that reach the symbol via the macro still resolve.
#ifdef LIBC_COPT_PUBLIC_PACKAGING
#define LIBC_SIGSETJMP_EXTERN_C_ALIAS ""
#else
#define LIBC_SIGSETJMP_EXTERN_C_ALIAS                                          \
  ".globl __llvm_libc_sigsetjmp\n\t"                                           \
  "__llvm_libc_sigsetjmp:\n\t"
#endif

namespace LIBC_NAMESPACE_DECL {

// Helper: save current signal mask into buf->sigmask and set did_save_mask.
// Called from the naked sigsetjmp before tail-calling setjmp.
// SysV ABI: buf arrives in rdi (the only arg); caller-saved regs may be
// clobbered by this call, so the naked asm spills rdi/rdx into callee-saves
// before invoking.
static void __sigsetjmp_save_mask(__jmp_buf *buf) {
  buf->did_save_mask = 1;
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, nullptr, &buf->sigmask);
}

// SysV AMD64 ABI: rdi = buf, esi = savesigs, rdx = frame (from macro).
// Tail-calls setjmp, which expects rdi = buf, rsi = frame.
//
// Stack math at each `call` site:
//   entry               RSP % 16 == 8  (return addr on top)
//   push rbx            RSP % 16 == 0
//   push r12            RSP % 16 == 8  (misaligned for call)
//   sub $8,  %rsp       RSP % 16 == 0  (aligned -- SysV requires 16B
//                                       alignment before a `call`)
//   call save_mask      ...
//   add $8,  %rsp       RSP % 16 == 8
//   pop r12             RSP % 16 == 0
//   pop rbx             RSP % 16 == 8  (entry -- return addr on top)
//   mov rbx->rdi, r12->rsi (recovered buf, frame)
//   jmp setjmp          tail-call, preserves return addr
//
// No shadow space (SysV has none, unlike MS x64 which reserved 32 bytes).
[[gnu::naked]]
LLVM_LIBC_FUNCTION(int, __llvm_libc_sigsetjmp, (sigjmp_buf, int, void *)) {
  asm(LIBC_SIGSETJMP_EXTERN_C_ALIAS R"(
      .seh_proc __llvm_libc_sigsetjmp
      .seh_endprologue

      // Store did_save_mask flag
      mov %%esi, %c[did_save](%%rdi)
      test %%esi, %%esi
      jz .Lnosave

      // Need to call __sigsetjmp_save_mask(buf) while preserving
      // rdi (buf) and rdx (frame) across the call. Spill into the
      // callee-saved rbx/r12, then restore.
      push %%rbx
      push %%r12
      sub $8, %%rsp
      mov %%rdi, %%rbx
      mov %%rdx, %%r12
      // rdi already holds buf -- save_mask's sole arg, no mov needed.
      call %P[save_mask]
      mov %%rbx, %%rdi
      mov %%r12, %%rsi
      add $8, %%rsp
      pop %%r12
      pop %%rbx
      jmp %P[setjmp]

.Lnosave:
      mov %%rdx, %%rsi
      jmp %P[setjmp]

      .seh_endproc
      )" ::[setjmp] "X"(__llvm_libc_setjmp),
      [save_mask] "X"(__sigsetjmp_save_mask),
      [did_save] "i"(offsetof(__jmp_buf, did_save_mask))
      : "rax", "rdx", "rsi");
}

} // namespace LIBC_NAMESPACE_DECL
