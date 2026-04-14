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

namespace LIBC_NAMESPACE_DECL {

// Helper: save current signal mask into buf->sigmask and set did_save_mask.
// Called from the naked sigsetjmp before tail-calling setjmp.
// Must preserve rcx (buf) and r8 (frame) -- only clobbers volatiles.
static void __sigsetjmp_save_mask(__jmp_buf *buf) {
  buf->did_save_mask = 1;
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, nullptr, &buf->sigmask);
}

// rcx = buf, edx = savesigs, r8 = frame (from macro)
[[gnu::naked]]
LLVM_LIBC_FUNCTION(int, __llvm_libc_sigsetjmp, (sigjmp_buf, int, void *)) {
  asm(R"(
      .seh_proc __llvm_libc_sigsetjmp
      .seh_endprologue

      // Store did_save_mask flag
      mov %%edx, %c[did_save](%%rcx)
      test %%edx, %%edx
      jz .Lnosave

      // Need to call __sigsetjmp_save_mask(buf) while preserving
      // rcx (buf) and r8 (frame). Use callee-saved registers.
      push %%rbx
      push %%r12
      sub $32, %%rsp
      mov %%rcx, %%rbx
      mov %%r8, %%r12
      call %P[save_mask]
      mov %%rbx, %%rcx
      mov %%r12, %%rdx
      add $32, %%rsp
      pop %%r12
      pop %%rbx
      jmp %P[setjmp]

.Lnosave:
      mov %%r8, %%rdx
      jmp %P[setjmp]

      .seh_endproc
      )" ::[setjmp] "X"(__llvm_libc_setjmp),
      [save_mask] "X"(__sigsetjmp_save_mask),
      [did_save] "i"(offsetof(__jmp_buf, did_save_mask))
      : "rax", "rdx");
}

} // namespace LIBC_NAMESPACE_DECL
