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
#define SYS_inotify_init 253
#define SYS_inotify_add_watch 254
#define SYS_inotify_rm_watch 255

// ===========================================================================
// Mass-wire additions: every Linux x86_64 syscall number whose engine exists
// in-tree (ready in internal::/signal_state::/windows_syscalls::) and is
// reachable through the `syscall_dispatch` switch.
// ===========================================================================

// --- File I/O: stat family ---------------------------------------------------
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lstat 6
#define SYS_newfstatat 262

// --- File I/O: positional / vector ------------------------------------------
#define SYS_pread64 17
#define SYS_pwrite64 18
#define SYS_readv 19
#define SYS_writev 20
#define SYS_preadv 295
#define SYS_pwritev 296

// --- File I/O: fd lifecycle --------------------------------------------------
// SYS_dup is declared here; SYS_dup2 is above (already in base set).
#define SYS_dup 32
#define SYS_dup3 292
#define SYS_pipe 22
#define SYS_pipe2 293

// --- File I/O: fcntl --------------------------------------------------------
#define SYS_fcntl 72

// --- File I/O: sync / flush -------------------------------------------------
#define SYS_fsync 74
#define SYS_fdatasync 75

// --- File I/O: truncation ---------------------------------------------------
#define SYS_truncate 76
#define SYS_ftruncate 77

// --- Path-based metadata / directory ops ------------------------------------
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_fchdir 81
#define SYS_mkdirat 258
#define SYS_unlinkat 263
#define SYS_renameat 264
#define SYS_linkat 265
#define SYS_symlinkat 266
#define SYS_readlinkat 267
#define SYS_faccessat 269

// --- Permissions / ownership ------------------------------------------------
#define SYS_fchmodat 268
#define SYS_fchownat 260
#define SYS_umask 95

// --- Timestamps -------------------------------------------------------------
#define SYS_utimensat 280

// --- Cross-fd transfer ------------------------------------------------------
#define SYS_sendfile 40
#define SYS_copy_file_range 326

// --- Memory-backed fds ------------------------------------------------------
#define SYS_memfd_create 319

// --- Memory: address space --------------------------------------------------
#define SYS_mremap 25

// --- Memory: advice / hints -------------------------------------------------
#define SYS_madvise 28
#define SYS_mincore 27
#define SYS_msync 26

// --- Memory: locking --------------------------------------------------------
#define SYS_mlock 149
#define SYS_mlockall 150
#define SYS_munlock 151
#define SYS_munlockall 152
#define SYS_mlock2 325

// --- Memory: non-linear / NUMA / pkey / placeholder -------------------------
#define SYS_remap_file_pages 216
#define SYS_mbind 237
#define SYS_set_mempolicy 238
#define SYS_get_mempolicy 239
#define SYS_pkey_mprotect 329
#define SYS_pkey_alloc 330
#define SYS_pkey_free 331

// --- SysV shared memory -----------------------------------------------------
#define SYS_shmget 29
#define SYS_shmat 30
#define SYS_shmctl 31
#define SYS_shmdt 67

// --- Signals: wait / query --------------------------------------------------
#define SYS_rt_sigpending 127
#define SYS_rt_sigtimedwait 128
#define SYS_rt_sigqueueinfo 129
#define SYS_rt_sigsuspend 130
#define SYS_tkill 200
// rt_tgsigqueueinfo(tgid, tid, sig, siginfo) — backs pthread_sigqueue.
// Linux x86_64 number is 297.
#define SYS_rt_tgsigqueueinfo 297

// --- Process identity / credentials -----------------------------------------
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_setreuid 113
#define SYS_setregid 114
#define SYS_getgroups 115
#define SYS_setresuid 117
#define SYS_getresuid 118
#define SYS_setresgid 119
#define SYS_getresgid 120

// --- Process lifecycle / wait -----------------------------------------------
#define SYS_fork 57
#define SYS_vfork 58
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_waitid 247
#define SYS_execveat 322

// --- Resource limits / accounting -------------------------------------------
#define SYS_getrlimit 97
#define SYS_getrusage 98
#define SYS_setrlimit 160
#define SYS_prlimit64 302

// --- Random / cpu identity --------------------------------------------------
#define SYS_getrandom 318
#define SYS_getcpu 309

// --- Sockets: lifecycle -----------------------------------------------------
#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept 43
#define SYS_shutdown 48
#define SYS_bind 49
#define SYS_listen 50
#define SYS_socketpair 53

// --- Sockets: data transfer -------------------------------------------------
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_sendmsg 46
#define SYS_recvmsg 47

// --- Sockets: query ---------------------------------------------------------
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_setsockopt 54
#define SYS_getsockopt 55

// --- Multiplexing -----------------------------------------------------------
#define SYS_poll 7
#define SYS_select 23
#define SYS_pselect6 270
#define SYS_ppoll 271
#define SYS_epoll_create 213
#define SYS_epoll_create1 291
#define SYS_epoll_ctl 233
#define SYS_epoll_wait 232

// --- SysV semaphores --------------------------------------------------------
#define SYS_semget 64
#define SYS_semop 65
#define SYS_semctl 66

// --- Time -------------------------------------------------------------------
#define SYS_gettimeofday 96
#define SYS_clock_settime 227
#define SYS_clock_getres 229

// --- POSIX timers -----------------------------------------------------------
#define SYS_timer_create 222
#define SYS_timer_settime 223
#define SYS_timer_gettime 224
#define SYS_timer_getoverrun 225
#define SYS_timer_delete 226

// --- Interval timers --------------------------------------------------------
#define SYS_getitimer 36
#define SYS_setitimer 38

// --- Scheduling policy / params / affinity ----------------------------------
#define SYS_sched_setparam 142
#define SYS_sched_getparam 143
#define SYS_sched_setscheduler 144
#define SYS_sched_getscheduler 145
#define SYS_sched_get_priority_max 146
#define SYS_sched_get_priority_min 147
#define SYS_sched_rr_get_interval 148
#define SYS_sched_setaffinity 203
#define SYS_sched_getaffinity 204

// --- Futex ------------------------------------------------------------------
#define SYS_futex 202

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_NUMBERS_H
