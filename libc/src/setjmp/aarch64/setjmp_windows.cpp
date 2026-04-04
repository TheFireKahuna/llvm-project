//===-- Windows ARM64 implementation of setjmp ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/offsetof_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/setjmp/setjmp_impl.h"

#if !defined(LIBC_TARGET_ARCH_IS_AARCH64)
#error "Invalid file include"
#endif

#if !defined(LIBC_TARGET_OS_IS_WINDOWS)
#error "This file is Windows-specific"
#endif

namespace LIBC_NAMESPACE_DECL {

// Windows ARM64: x0 = buf, x1 = frame. Callee-saved: x19-x28, x29/fp,
// d8-d15, FPCR, FPSR. Frame is the caller's establisher frame for
// RtlUnwindEx (used by longjmp).
[[gnu::naked]]
LLVM_LIBC_FUNCTION(int, setjmp, ([[maybe_unused]] jmp_buf buf,
                                  [[maybe_unused]] void *frame)) {
  asm(
#if __ARM_FEATURE_PAC_DEFAULT & 1
      R"(
        paciasp
      )"
#elif __ARM_FEATURE_PAC_DEFAULT & 2
      R"(
        pacibsp
      )"
#endif

      // Save callee-saved GPRs (same layout as Linux path).
      R"(
        stp x19, x20, [x0,  #0*16]
        stp x21, x22, [x0,  #1*16]
        stp x23, x24, [x0,  #2*16]
        stp x25, x26, [x0,  #3*16]
        stp x27, x28, [x0,  #4*16]
        stp x29, x30, [x0,  #5*16]
        add x2, sp, #0
        str x2,       [x0,  #6*16]
      )"

#if __ARM_FP
      // Save callee-saved FP registers (d8-d15, low 64 bits of v8-v15).
      R"(
        stp d8,  d9,  [x0,  #7*16]
        stp d10, d11, [x0,  #8*16]
        stp d12, d13, [x0,  #9*16]
        stp d14, d15, [x0, #10*16]
      )"
#endif

      // Save Windows-specific fields: FPCR, FPSR, frame.
      R"(
        mrs x2, fpcr
        str w2, [x0, %c[fpcr]]
        mrs x2, fpsr
        str w2, [x0, %c[fpsr]]
        str x1, [x0, %c[frame]]
        mov x0, #0
      )"

#if (__ARM_FEATURE_PAC_DEFAULT & 7) == 5
      R"(
        autiasp
      )"
#elif (__ARM_FEATURE_PAC_DEFAULT & 7) == 6
      R"(
        autibsp
      )"
#endif

      R"(
        ret
      )" ::[fpcr] "i"(offsetof(__jmp_buf, fpcr)),
      [fpsr] "i"(offsetof(__jmp_buf, fpsr)),
      [frame] "i"(offsetof(__jmp_buf, frame)));
}

} // namespace LIBC_NAMESPACE_DECL
