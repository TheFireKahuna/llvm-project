//===-- Definition of mcontext_t type -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_MCONTEXT_T_H
#define LLVM_LIBC_TYPES_MCONTEXT_T_H

#ifdef __linux__

// Linux mcontext_t is architecture-specific and defined by the kernel.
// TODO: define for Linux targets as needed.

#elif defined(_WIN32) || defined(__NTPOSIX__)

// Windows mcontext_t: machine register state for SA_SIGINFO signal handlers.
// Populated from the Win32 CONTEXT structure.

#if defined(__x86_64__)

typedef struct {
  // General-purpose registers
  __UINT64_TYPE__ gregs[23];
  // Floating-point state (fxsave layout, 512 bytes)
  __UINT8_TYPE__ fpregs[512];
} mcontext_t;

// Indices into gregs[], matching the POSIX REG_* convention.
// The order is chosen for compatibility with existing POSIX code; the values
// themselves are arbitrary since there is no kernel ABI to match on Windows.
#define REG_R8 0
#define REG_R9 1
#define REG_R10 2
#define REG_R11 3
#define REG_R12 4
#define REG_R13 5
#define REG_R14 6
#define REG_R15 7
#define REG_RDI 8
#define REG_RSI 9
#define REG_RBP 10
#define REG_RBX 11
#define REG_RDX 12
#define REG_RAX 13
#define REG_RCX 14
#define REG_RSP 15
#define REG_RIP 16
#define REG_EFL 17
#define REG_CSGSFS 18
#define REG_ERR 19
#define REG_TRAPNO 20
#define REG_OLDMASK 21
#define REG_CR2 22
#define NGREG 23

#elif defined(__aarch64__)

typedef struct {
  // General-purpose registers: x0-x30, sp, pc, pstate (34 entries)
  __UINT64_TYPE__ gregs[34];
  // NEON/SIMD registers: v0-v31 (32 x 128-bit = 512 bytes)
  __UINT8_TYPE__ vregs[512];
  // FP control/status
  __UINT32_TYPE__ fpcr;
  __UINT32_TYPE__ fpsr;
} mcontext_t;

// Indices into gregs[].
#define REG_X0 0
// x1-x28 at indices 1-28
#define REG_X29 29
#define REG_X30 30
#define REG_SP 31
#define REG_PC 32
#define REG_PSTATE 33
#define NGREG 34

#else
#error "mcontext_t: unsupported architecture"
#endif

#endif // _WIN32
#endif // LLVM_LIBC_TYPES_MCONTEXT_T_H
