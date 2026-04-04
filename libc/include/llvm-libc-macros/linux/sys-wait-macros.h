//===-- Definition of macros from sys/wait.h ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_LINUX_SYS_WAIT_MACROS_H
#define LLVM_LIBC_MACROS_LINUX_SYS_WAIT_MACROS_H

#include <linux/wait.h>

#ifndef WNOHANG
#define WNOHANG 0x00000001
#endif

#ifndef WUNTRACED
#define WUNTRACED 0x00000002
#endif

#ifndef WSTOPPED
#define WSTOPPED WUNTRACED
#endif

#ifndef WEXITED
#define WEXITED 0x00000004
#endif

#ifndef WCONTINUED
#define WCONTINUED 0x00000008
#endif

#ifndef WNOWAIT
#define WNOWAIT 0x01000000
#endif

#ifndef P_ALL
#define P_ALL 0
#endif

#ifndef P_PID
#define P_PID 1
#endif

#ifndef P_PGID
#define P_PGID 2
#endif

#define WCOREDUMP(status) ((status) & WCOREFLAG)
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFCONTINUED(status) ((status) == 0xffff)
#define WIFEXITED(status) (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) ((WTERMSIG(status) + 1) >= 2)
#define WIFSTOPPED(status) (WTERMSIG(status) == 0x7f)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WTERMSIG(status) ((status) & 0x7f)

#define WCOREFLAG 0x80
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#define W_STOPCODE(sig) ((sig) << 8 | 0x7f)

#endif // LLVM_LIBC_MACROS_LINUX_SYS_WAIT_MACROS_H
