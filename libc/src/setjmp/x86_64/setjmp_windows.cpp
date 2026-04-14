//===-- Windows x64 implementation of setjmp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "hdr/offsetof_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#define LLVM_LIBC_SETJMP_DONT_DEFINE_MACRO
#include "src/setjmp/setjmp_impl.h"
#undef LLVM_LIBC_SETJMP_DONT_DEFINE_MACRO

#if !defined(LIBC_TARGET_ARCH_IS_X86_64)
#error "Invalid file include"
#endif

#if !defined(LIBC_TARGET_OS_IS_WINDOWS)
#error "This file is Windows-specific"
#endif

namespace LIBC_NAMESPACE_DECL {

// Windows x64 ABI: buf in rcx, frame in rdx. Callee-saved: rbx, rbp, rdi,
// rsi, r12-r15, XMM6-XMM15, MXCSR, x87 FCW.
//
// The frame argument is the caller's establisher frame for RtlUnwindEx
// (used by longjmp). Callers pass it via the Windows setjmp macro using
// __builtin_frame_address(0). sigsetjmp forwards it explicitly.
[[gnu::naked]]
LLVM_LIBC_FUNCTION(int, __llvm_libc_setjmp, (jmp_buf buf, void *frame)) {
  asm(R"(
      mov %%rdx, %c[frame](%%rcx)

      mov %%rbx, %c[rbx](%%rcx)
      mov %%rbp, %c[rbp](%%rcx)
      mov %%r12, %c[r12](%%rcx)
      mov %%r13, %c[r13](%%rcx)
      mov %%r14, %c[r14](%%rcx)
      mov %%r15, %c[r15](%%rcx)
      mov %%rdi, %c[rdi](%%rcx)
      mov %%rsi, %c[rsi](%%rcx)

      lea 8(%%rsp), %%rax
      mov %%rax, %c[rsp](%%rcx)

      mov (%%rsp), %%rax
      mov %%rax, %c[rip](%%rcx)

      movups %%xmm6,  (%c[xmm]+0x00)(%%rcx)
      movups %%xmm7,  (%c[xmm]+0x10)(%%rcx)
      movups %%xmm8,  (%c[xmm]+0x20)(%%rcx)
      movups %%xmm9,  (%c[xmm]+0x30)(%%rcx)
      movups %%xmm10, (%c[xmm]+0x40)(%%rcx)
      movups %%xmm11, (%c[xmm]+0x50)(%%rcx)
      movups %%xmm12, (%c[xmm]+0x60)(%%rcx)
      movups %%xmm13, (%c[xmm]+0x70)(%%rcx)
      movups %%xmm14, (%c[xmm]+0x80)(%%rcx)
      movups %%xmm15, (%c[xmm]+0x90)(%%rcx)

      stmxcsr %c[mxcsr](%%rcx)
      fnstcw  %c[fpcw](%%rcx)

      xorl %%eax, %%eax
      retq)" ::[rbx] "i"(offsetof(__jmp_buf, rbx)),
      [rbp] "i"(offsetof(__jmp_buf, rbp)), [r12] "i"(offsetof(__jmp_buf, r12)),
      [r13] "i"(offsetof(__jmp_buf, r13)), [r14] "i"(offsetof(__jmp_buf, r14)),
      [r15] "i"(offsetof(__jmp_buf, r15)), [rsp] "i"(offsetof(__jmp_buf, rsp)),
      [rip] "i"(offsetof(__jmp_buf, rip)), [rdi] "i"(offsetof(__jmp_buf, rdi)),
      [rsi] "i"(offsetof(__jmp_buf, rsi)), [xmm] "i"(offsetof(__jmp_buf, xmm)),
      [mxcsr] "i"(offsetof(__jmp_buf, mxcsr)),
      [fpcw] "i"(offsetof(__jmp_buf, fpcw)),
      [frame] "i"(offsetof(__jmp_buf, frame))
      : "rax");
}

} // namespace LIBC_NAMESPACE_DECL
