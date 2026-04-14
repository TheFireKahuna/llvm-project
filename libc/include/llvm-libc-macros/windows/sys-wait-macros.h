//===-- Definition of macros from sys/wait.h for Windows -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Uses the same status encoding as Linux for binary compatibility of wait
// status values. This is pure bit packing with no kernel dependency.
//
//   Exit:      status = (exit_code & 0xFF) << 8
//   Signal:    status = (signum & 0x7F)
//   Stopped:   status = (signum << 8) | 0x7F
//   Continued: status = 0xFFFF
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SYS_WAIT_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SYS_WAIT_MACROS_H

// Wait options
#define WNOHANG 0x00000001
#define WUNTRACED 0x00000002
#define WSTOPPED WUNTRACED
#define WEXITED 0x00000004
#define WCONTINUED 0x00000008
#define WNOWAIT 0x01000000

// waitid selectors
#define P_ALL 0
#define P_PID 1
#define P_PGID 2

// Status decoding
#define WTERMSIG(status) ((status) & 0x7F)
#define WEXITSTATUS(status) (((status) & 0xFF00) >> 8)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WIFEXITED(status) (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) ((WTERMSIG(status) + 1) >= 2)
#define WIFSTOPPED(status) (WTERMSIG(status) == 0x7F)
#define WIFCONTINUED(status) ((status) == 0xFFFF)
#define WCOREDUMP(status) ((status) & WCOREFLAG)

// Status encoding
#define WCOREFLAG 0x80
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#define W_STOPCODE(sig) ((sig) << 8 | 0x7F)

#endif // LLVM_LIBC_MACROS_WINDOWS_SYS_WAIT_MACROS_H
