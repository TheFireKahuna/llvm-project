//===-- Definition of type __jmp_buf --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES___JMP_BUF_H
#define LLVM_LIBC_TYPES___JMP_BUF_H

// TODO: implement sigjmp_buf related functions for other architectures
// Issue: https://github.com/llvm/llvm-project/issues/136358
#if defined(__linux__) || defined(__NTPOSIX__)
#if defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) ||        \
    defined(__arm__) || defined(__riscv)
#define __LIBC_HAS_SIGJMP_BUF
#endif
#endif

#if defined(__LIBC_HAS_SIGJMP_BUF)
#include "sigset_t.h"
#endif

typedef struct {
#ifdef __x86_64__
  __UINT64_TYPE__ rbx;
  __UINT64_TYPE__ rbp;
  __UINT64_TYPE__ r12;
  __UINT64_TYPE__ r13;
  __UINT64_TYPE__ r14;
  __UINT64_TYPE__ r15;
  __UINTPTR_TYPE__ rsp;
  __UINTPTR_TYPE__ rip;
#if defined(__NTPOSIX__)
  // SysV AMD64 ABI: rdi, rsi, xmm0-xmm15 are caller-saved (volatile), so
  // setjmp does not capture them. Only rbx, rbp, r12-r15 (above) plus
  // MXCSR control bits and x87 FCW are callee-preserved per psABI §3.2.1.
  __UINT32_TYPE__ mxcsr;
  __UINT16_TYPE__ fpcw;
  __UINT16_TYPE__ _pad;
  // Establisher frame for RtlUnwindEx (longjmp unwind target)
  __UINT64_TYPE__ frame;
  // Whether sigsetjmp saved the signal mask. Read by siglongjmp to decide
  // whether to restore it before calling longjmp.
  __UINT32_TYPE__ did_save_mask;
  __UINT32_TYPE__ _pad2;
#endif
#elif defined(__i386__)
  long ebx;
  long esi;
  long edi;
  long ebp;
  long esp;
  long eip;
#elif defined(__riscv)
  /* Program counter.  */
  long int __pc;
  /* Callee-saved registers.  */
  long int __regs[12];
  /* Stack pointer.  */
  long int __sp;
  /* Callee-saved floating point registers.  */
#if __riscv_float_abi_double
  double __fpregs[12];
#elif defined(__riscv_float_abi_single)
#error "__jmp_buf not available for your target architecture."
#endif
#elif defined(__arm__)
  // r4, r5, r6, r7, r8, r9, r10, r11, r12, lr
  long opaque[10];
#elif defined(__aarch64__)
  long opaque[14]; // x19-x29, lr, sp, optional x18
#if __ARM_FP
  long fopaque[8]; // d8-d15
#endif
#if defined(__NTPOSIX__)
  // FP control/status — callee-saved on Windows ARM64
  __UINT32_TYPE__ fpcr;
  __UINT32_TYPE__ fpsr;
  // Establisher frame for RtlUnwindEx (longjmp unwind target)
  __UINT64_TYPE__ frame;
  // Whether sigsetjmp saved the signal mask
  __UINT32_TYPE__ did_save_mask;
  __UINT32_TYPE__ _pad;
#endif
#else
#error "__jmp_buf not available for your target architecture."
#endif
#if defined(__LIBC_HAS_SIGJMP_BUF)
  // return address
  void *sig_retaddr;
  // extra register buffer to avoid indefinite stack growth in sigsetjmp
  void *sig_extra;
  // signal masks
  sigset_t sigmask;
#endif
} __jmp_buf;

#undef __LIBC_HAS_SIGJMP_BUF

#endif // LLVM_LIBC_TYPES___JMP_BUF_H
