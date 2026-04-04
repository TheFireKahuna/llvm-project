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

// LLVM_LIBC_FUNCTION emits the extern-"C" `__llvm_libc_setjmp` public alias
// only when LIBC_COPT_PUBLIC_PACKAGING is defined. The `__internal__`
// entrypoint variant (used by hermetic + cross-targeted unit test links on
// freestanding Windows) builds without it, so without extra help only the
// C++-mangled `LIBC_NAMESPACE::__llvm_libc_setjmp` is emitted.
//
// setjmp cannot be forwarded through a wrapper (a forwarder's frame would be
// captured instead of the caller's, breaking longjmp). Emit the extern-"C"
// alias directly at the naked-asm entry so both symbols resolve to the same
// code address. Prefix the asm body with the alias label when we are in the
// `__internal__` build; keep empty under PUBLIC_PACKAGING to avoid a
// duplicate-symbol collision with LLVM_LIBC_FUNCTION_IMPL_4's own alias.
#ifdef LIBC_COPT_PUBLIC_PACKAGING
#define LIBC_SETJMP_EXTERN_C_ALIAS ""
#else
#define LIBC_SETJMP_EXTERN_C_ALIAS                                             \
  ".globl __llvm_libc_setjmp\n\t"                                              \
  "__llvm_libc_setjmp:\n\t"
#endif

namespace LIBC_NAMESPACE_DECL {

// SysV AMD64 ABI: buf in rdi, frame in rsi.
// Callee-saved integer regs: rbx, rbp, r12-r15 (plus rsp).
// Caller-saved (volatile): rax, rcx, rdx, rsi, rdi, r8-r11, xmm0-xmm15.
// Per psABI §3.2.1 MXCSR control bits and x87 FCW are callee-preserved.
//
// The frame argument is the caller's establisher frame for RtlUnwindEx
// (used by longjmp). Callers pass it via the Windows setjmp macro using
// __builtin_frame_address(0). sigsetjmp forwards it explicitly.
//
// No SEH prologue directives are needed: the function allocates no stack,
// pushes no registers, and is a single leaf-style block of stores.
[[gnu::naked]]
LLVM_LIBC_FUNCTION(int, __llvm_libc_setjmp, (jmp_buf buf, void *frame)) {
  asm(LIBC_SETJMP_EXTERN_C_ALIAS R"(
      mov %%rsi, %c[frame](%%rdi)

      mov %%rbx, %c[rbx](%%rdi)
      mov %%rbp, %c[rbp](%%rdi)
      mov %%r12, %c[r12](%%rdi)
      mov %%r13, %c[r13](%%rdi)
      mov %%r14, %c[r14](%%rdi)
      mov %%r15, %c[r15](%%rdi)

      lea 8(%%rsp), %%rax
      mov %%rax, %c[rsp](%%rdi)

      mov (%%rsp), %%rax
      mov %%rax, %c[rip](%%rdi)

      stmxcsr %c[mxcsr](%%rdi)
      fnstcw  %c[fpcw](%%rdi)

      xorl %%eax, %%eax
      retq)" ::[rbx] "i"(offsetof(__jmp_buf, rbx)),
      [rbp] "i"(offsetof(__jmp_buf, rbp)), [r12] "i"(offsetof(__jmp_buf, r12)),
      [r13] "i"(offsetof(__jmp_buf, r13)), [r14] "i"(offsetof(__jmp_buf, r14)),
      [r15] "i"(offsetof(__jmp_buf, r15)), [rsp] "i"(offsetof(__jmp_buf, rsp)),
      [rip] "i"(offsetof(__jmp_buf, rip)),
      [mxcsr] "i"(offsetof(__jmp_buf, mxcsr)),
      [fpcw] "i"(offsetof(__jmp_buf, fpcw)),
      [frame] "i"(offsetof(__jmp_buf, frame))
      : "rax");
}

} // namespace LIBC_NAMESPACE_DECL
