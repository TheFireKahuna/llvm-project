//===-- Windows POSIX syscall number definitions ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux x86_64 syscall numbers used as dispatch IDs on Windows.
// The values are arbitrary on Windows — we use the Linux values for
// familiarity so that callers can use the same SYS_* constants on both
// platforms.
//
// This header is intentionally free of any dependencies so it can be
// included from anywhere without pulling in dispatch machinery.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_NUMBERS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_NUMBERS_H

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_lseek 8

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_brk 12

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
#define SYS_sched_yield 24

// ---------------------------------------------------------------------------
// File descriptor operations
// ---------------------------------------------------------------------------
#define SYS_dup2 33

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
#define SYS_nanosleep 35

// ---------------------------------------------------------------------------
// Process identity
// ---------------------------------------------------------------------------
#define SYS_getpid 39
#define SYS_exit 60
#define SYS_kill 62
#define SYS_getuid 102
#define SYS_getgid 104
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_setpgid 109
#define SYS_getppid 110
#define SYS_getpgrp 111
#define SYS_setsid 112
#define SYS_getpgid 121
#define SYS_getsid 124

// ---------------------------------------------------------------------------
// Resource / scheduling priority
// ---------------------------------------------------------------------------
#define SYS_getpriority 140
#define SYS_setpriority 141

// ---------------------------------------------------------------------------
// Signal stack
// ---------------------------------------------------------------------------
#define SYS_sigaltstack 131

// ---------------------------------------------------------------------------
// Thread identity
// ---------------------------------------------------------------------------
#define SYS_gettid 186

// ---------------------------------------------------------------------------
// Time (continued)
// ---------------------------------------------------------------------------
#define SYS_clock_gettime 228
#define SYS_clock_nanosleep 230

// ---------------------------------------------------------------------------
// Process lifecycle
// ---------------------------------------------------------------------------
#define SYS_exit_group 231

// ---------------------------------------------------------------------------
// Thread-directed signals
// ---------------------------------------------------------------------------
#define SYS_tgkill 234

// ---------------------------------------------------------------------------
// Directory-relative file operations
// ---------------------------------------------------------------------------
#define SYS_openat 257

// ---------------------------------------------------------------------------
// Inotify — filesystem event monitoring
// ---------------------------------------------------------------------------
#define SYS_inotify_init1 294
#define SYS_inotify_add_watch 254
#define SYS_inotify_rm_watch 255

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_NUMBERS_H
