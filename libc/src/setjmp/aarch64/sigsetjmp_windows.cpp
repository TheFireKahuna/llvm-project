//===-- Windows ARM64 implementation of sigsetjmp -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Windows sigsetjmp: no epilogue pattern. Mask save/restore is split:
//   sigsetjmp: saves the mask into buf->sigmask (if savesigs)
//   siglongjmp: restores it before calling longjmp

#include "src/setjmp/sigsetjmp.h"
#include "hdr/offsetof_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/setjmp/setjmp_impl.h"
#include "src/signal/sigprocmask.h"
#include "hdr/signal_macros.h"

#if !defined(LIBC_TARGET_ARCH_IS_AARCH64)
#error "Invalid file include"
#endif

#if !defined(LIBC_TARGET_OS_IS_WINDOWS)
#error "This file is Windows-specific"
#endif

namespace LIBC_NAMESPACE_DECL {

// Save current signal mask into buf->sigmask and set did_save_mask.
// Must preserve x0 and x2 -- only clobbers volatiles.
static void __sigsetjmp_save_mask(__jmp_buf *buf) {
  buf->did_save_mask = 1;
  LIBC_NAMESPACE::sigprocmask(SIG_BLOCK, nullptr, &buf->sigmask);
}

// x0 = buf, w1 = savesigs, x2 = frame (from macro)
[[gnu::naked]]
LLVM_LIBC_FUNCTION(int, sigsetjmp, (sigjmp_buf, int, void *)) {
  asm(R"(
      // Store did_save_mask flag (0 or nonzero)
      str w1, [x0, %c[did_save]]
      cbz w1, .Lnosave

      // Save mask path: preserve x0 (buf) and x2 (frame) across call.
      // Use callee-saved x19/x20 -- save and restore them ourselves.
      stp x29, x30, [sp, #-32]!
      stp x19, x20, [sp, #16]
      mov x29, sp
      mov x19, x0
      mov x20, x2
      bl %c[save_mask]
      mov x0, x19
      mov x1, x20
      ldp x19, x20, [sp, #16]
      ldp x29, x30, [sp], #32
      b %c[setjmp]

  .Lnosave:
      mov x1, x2
      b %c[setjmp]
  )" ::[setjmp] "i"(setjmp),
      [save_mask] "i"(__sigsetjmp_save_mask),
      [did_save] "i"(offsetof(__jmp_buf, did_save_mask)));
}

} // namespace LIBC_NAMESPACE_DECL
