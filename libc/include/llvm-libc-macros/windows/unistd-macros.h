//===-- Definition of macros from unistd.h --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_UNISTD_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_UNISTD_MACROS_H

// POSIX version identification.
#define _POSIX_VERSION 200809L
#define _POSIX2_VERSION 200809L
#define _XOPEN_VERSION 700

// POSIX option / feature-test macros (XBD <unistd.h>).
#define _POSIX_THREADS 200809L
#define _POSIX_THREAD_SAFE_FUNCTIONS 200809L
#define _POSIX_THREAD_ATTR_STACKADDR 200809L
#define _POSIX_THREAD_ATTR_STACKSIZE 200809L
#define _POSIX_THREAD_PROCESS_SHARED 200809L
#define _POSIX_THREAD_PRIO_INHERIT (-1)
#define _POSIX_THREAD_PRIO_PROTECT (-1)
#define _POSIX_THREAD_PRIORITY_SCHEDULING (-1)
#define _POSIX_BARRIERS 200809L
#define _POSIX_READER_WRITER_LOCKS 200809L
#define _POSIX_SPIN_LOCKS 200809L
#define _POSIX_MAPPED_FILES 200809L
#define _POSIX_MEMORY_PROTECTION 200809L
#define _POSIX_MEMLOCK (-1)
#define _POSIX_MEMLOCK_RANGE 200809L
#define _POSIX_SPAWN 200809L
#define _POSIX_SHARED_MEMORY_OBJECTS 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L
#define _POSIX_CLOCK_SELECTION 200809L
#define _POSIX_CPUTIME 200809L
#define _POSIX_THREAD_CPUTIME 200809L
#define _POSIX_FSYNC 200809L
#define _POSIX_SYNCHRONIZED_IO 200809L
#define _POSIX_TIMEOUTS 200809L
#define _POSIX_TIMERS 200809L
#define _POSIX_SEMAPHORES 200809L
#define _POSIX_JOB_CONTROL 1
#define _POSIX_SAVED_IDS 1
#define _POSIX_REALTIME_SIGNALS 200809L
#define _POSIX_MESSAGE_PASSING (-1)
#define _POSIX_ASYNCHRONOUS_IO (-1)
#define _POSIX_REGEXP 1
#define _POSIX_SHELL 1
#define _POSIX_ADVISORY_INFO (-1)
#define _POSIX_PRIORITY_SCHEDULING (-1)
#define _POSIX_IPV6 200809L
#define _POSIX_RAW_SOCKETS 200809L

// POSIX.2 option macros.
#define _POSIX2_C_BIND 200809L
#define _POSIX2_C_DEV 200809L
#define _POSIX2_SW_DEV 200809L
#define _POSIX2_LOCALEDEF 200809L
#define _POSIX2_CHAR_TERM 1
#define _POSIX2_FORT_DEV (-1)
#define _POSIX2_FORT_RUN (-1)
#define _POSIX2_PBS (-1)
#define _POSIX2_PBS_ACCOUNTING (-1)
#define _POSIX2_PBS_CHECKPOINT (-1)
#define _POSIX2_PBS_LOCATE (-1)
#define _POSIX2_PBS_MESSAGE (-1)
#define _POSIX2_PBS_TRACK (-1)
#define _POSIX2_UPE 200809L

// POSIX.1-2008 programming environments.
// NTPOSIX uses 64-bit off_t on every architecture. Current Windows NTPOSIX
// targets are LLP64, so only 32-bit targets match a standard POSIX V7
// environment today (ILP32 with a 64-bit off_t). Keep the LP64 branch for
// completeness if a true LP64 Windows target is ever added.
#if __SIZEOF_LONG__ == 4 && __SIZEOF_POINTER__ == 4
#define _POSIX_V7_ILP32_OFFBIG 200809L
#else
#define _POSIX_V7_ILP32_OFFBIG (-1)
#endif

#if __SIZEOF_LONG__ == 8 && __SIZEOF_POINTER__ == 8
#define _POSIX_V7_LP64_OFF64 200809L
#else
#define _POSIX_V7_LP64_OFF64 (-1)
#endif

#define _POSIX_V7_LPBIG_OFFBIG (-1)

// Optional POSIX features that are not implemented by this target.
#define _POSIX_DEVICE_CONTROL (-1)
#define _POSIX_PRIORITIZED_IO (-1)
#define _POSIX_SPORADIC_SERVER (-1)
#define _POSIX_THREAD_ROBUST_PRIO_INHERIT (-1)
#define _POSIX_THREAD_ROBUST_PRIO_PROTECT (-1)
#define _POSIX_THREAD_SPORADIC_SERVER (-1)
#define _POSIX_TYPED_MEMORY_OBJECTS (-1)

// XSI / X/Open feature macros.
#define _XOPEN_ENH_I18N 1
#define _XOPEN_UNIX 1
#define _XOPEN_CRYPT (-1)
#define _XOPEN_LEGACY (-1)
#define _XOPEN_REALTIME 1
#define _XOPEN_REALTIME_THREADS 1
#define _XOPEN_SHM 1
#define _XOPEN_UUCP (-1)

// confstr names.
#define _CS_PATH 0
#define _CS_POSIX_V7_ILP32_OFF32_CFLAGS 1
#define _CS_POSIX_V7_ILP32_OFF32_LDFLAGS 2
#define _CS_POSIX_V7_ILP32_OFF32_LIBS 3
#define _CS_POSIX_V7_ILP32_OFFBIG_CFLAGS 4
#define _CS_POSIX_V7_ILP32_OFFBIG_LDFLAGS 5
#define _CS_POSIX_V7_ILP32_OFFBIG_LIBS 6
#define _CS_POSIX_V7_LP64_OFF64_CFLAGS 7
#define _CS_POSIX_V7_LP64_OFF64_LDFLAGS 8
#define _CS_POSIX_V7_LP64_OFF64_LIBS 9
#define _CS_POSIX_V7_LPBIG_OFFBIG_CFLAGS 10
#define _CS_POSIX_V7_LPBIG_OFFBIG_LDFLAGS 11
#define _CS_POSIX_V7_LPBIG_OFFBIG_LIBS 12
#define _CS_POSIX_V7_THREADS_CFLAGS 13
#define _CS_POSIX_V7_THREADS_LDFLAGS 14
#define _CS_POSIX_V7_WIDTH_RESTRICTED_ENVS 15
#define _CS_V7_ENV 16

// Values for mode argument to access().
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// lockf(3) commands.
#define F_ULOCK 0
#define F_LOCK 1
#define F_TLOCK 2
#define F_TEST 3

//===----------------------------------------------------------------------===//
// sysconf names
//
// Numbering is platform-specific (not prescribed by POSIX). We use our own
// implementation-local numbering for the Windows target, grouped by category
// matching the POSIX.1-2024 sysconf specification tables.
//===----------------------------------------------------------------------===//

// Resource limits (from <limits.h> configurable values).
#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_NGROUPS_MAX 3
#define _SC_OPEN_MAX 4
#define _SC_STREAM_MAX 5
#define _SC_TZNAME_MAX 6
#define _SC_PAGESIZE 7
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_RE_DUP_MAX 8
#define _SC_LOGIN_NAME_MAX 9
#define _SC_TTY_NAME_MAX 10
#define _SC_SYMLOOP_MAX 11
#define _SC_HOST_NAME_MAX 12
#define _SC_LINE_MAX 13
#define _SC_ATEXIT_MAX 14
#define _SC_IOV_MAX 15
#define _SC_NSIG 16

// Processor / memory topology.
#define _SC_NPROCESSORS_CONF 17
#define _SC_NPROCESSORS_ONLN 18
#define _SC_PHYS_PAGES 19
#define _SC_AVPHYS_PAGES 20

// Thread limits.
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 21
#define _SC_THREAD_KEYS_MAX 22
#define _SC_THREAD_STACK_MIN 23
#define _SC_THREAD_THREADS_MAX 24

// Cache topology.
#define _SC_LEVEL1_DCACHE_LINESIZE 25
#define _SC_LEVEL1_DCACHE_SIZE 26
#define _SC_LEVEL1_DCACHE_ASSOC 27
#define _SC_LEVEL1_ICACHE_LINESIZE 28
#define _SC_LEVEL1_ICACHE_SIZE 29
#define _SC_LEVEL1_ICACHE_ASSOC 30
#define _SC_LEVEL2_CACHE_LINESIZE 31
#define _SC_LEVEL2_CACHE_SIZE 32
#define _SC_LEVEL2_CACHE_ASSOC 33
#define _SC_LEVEL3_CACHE_LINESIZE 34
#define _SC_LEVEL3_CACHE_SIZE 35
#define _SC_LEVEL3_CACHE_ASSOC 36
#define _SC_LEVEL4_CACHE_LINESIZE 37
#define _SC_LEVEL4_CACHE_SIZE 38
#define _SC_LEVEL4_CACHE_ASSOC 39

// POSIX version.
#define _SC_VERSION 40

// POSIX.2 version / option queries.
#define _SC_2_VERSION 41
#define _SC_2_C_BIND 42
#define _SC_2_C_DEV 43
#define _SC_2_CHAR_TERM 44
#define _SC_2_FORT_RUN 45
#define _SC_2_LOCALEDEF 46
#define _SC_2_SW_DEV 47
#define _SC_2_UPE 48
#define _SC_2_FORT_DEV 90
#define _SC_2_PBS 91
#define _SC_2_PBS_ACCOUNTING 92
#define _SC_2_PBS_CHECKPOINT 93
#define _SC_2_PBS_LOCATE 94
#define _SC_2_PBS_MESSAGE 95
#define _SC_2_PBS_TRACK 96

// POSIX.1-2008 programming environment queries.
#define _SC_V7_ILP32_OFF32 49
#define _SC_V7_ILP32_OFFBIG 50
#define _SC_V7_LP64_OFF64 51
#define _SC_V7_LPBIG_OFFBIG 52

// Feature-test option queries. sysconf returns 200809L if the feature is
// supported, or -1 if not. Each corresponds to a _POSIX_* compile-time
// constant from <unistd.h>.
#define _SC_THREADS 53
#define _SC_THREAD_SAFE_FUNCTIONS 54
#define _SC_THREAD_ATTR_STACKADDR 55
#define _SC_THREAD_ATTR_STACKSIZE 56
#define _SC_THREAD_PROCESS_SHARED 57
#define _SC_THREAD_PRIO_INHERIT 58
#define _SC_THREAD_PRIO_PROTECT 59
#define _SC_THREAD_PRIORITY_SCHEDULING 60
#define _SC_BARRIERS 61
#define _SC_READER_WRITER_LOCKS 62
#define _SC_SPIN_LOCKS 63
#define _SC_MAPPED_FILES 64
#define _SC_MEMORY_PROTECTION 65
#define _SC_MEMLOCK 66
#define _SC_MEMLOCK_RANGE 67
#define _SC_SPAWN 68
#define _SC_SHARED_MEMORY_OBJECTS 69
#define _SC_MONOTONIC_CLOCK 70
#define _SC_CLOCK_SELECTION 71
#define _SC_CPUTIME 72
#define _SC_THREAD_CPUTIME 73
#define _SC_FSYNC 74
#define _SC_SYNCHRONIZED_IO 75
#define _SC_TIMEOUTS 76
#define _SC_TIMERS 77
#define _SC_SEMAPHORES 78
#define _SC_JOB_CONTROL 79
#define _SC_SAVED_IDS 80
#define _SC_REALTIME_SIGNALS 81
#define _SC_MESSAGE_PASSING 82
#define _SC_ASYNCHRONOUS_IO 83
#define _SC_REGEXP 84
#define _SC_SHELL 85
#define _SC_ADVISORY_INFO 86
#define _SC_PRIORITY_SCHEDULING 87
#define _SC_IPV6 88
#define _SC_RAW_SOCKETS 89
#define _SC_AIO_LISTIO_MAX 97
#define _SC_AIO_MAX 98
#define _SC_AIO_PRIO_DELTA_MAX 99
#define _SC_BC_BASE_MAX 100
#define _SC_BC_DIM_MAX 101
#define _SC_BC_SCALE_MAX 102
#define _SC_BC_STRING_MAX 103
#define _SC_COLL_WEIGHTS_MAX 104
#define _SC_DELAYTIMER_MAX 105
#define _SC_DEVICE_CONTROL 106
#define _SC_EXPR_NEST_MAX 107
#define _SC_GETGR_R_SIZE_MAX 108
#define _SC_GETPW_R_SIZE_MAX 109
#define _SC_MQ_OPEN_MAX 110
#define _SC_MQ_PRIO_MAX 111
#define _SC_PRIORITIZED_IO 112
#define _SC_RTSIG_MAX 113
#define _SC_SEM_NSEMS_MAX 114
#define _SC_SEM_VALUE_MAX 115
#define _SC_SIGQUEUE_MAX 116
#define _SC_SPORADIC_SERVER 117
#define _SC_SS_REPL_MAX 118
#define _SC_TIMER_MAX 119
#define _SC_THREAD_ROBUST_PRIO_INHERIT 120
#define _SC_THREAD_ROBUST_PRIO_PROTECT 121
#define _SC_THREAD_SPORADIC_SERVER 122
#define _SC_TYPED_MEMORY_OBJECTS 123
#define _SC_V8_ILP32_OFF32 124
#define _SC_V8_ILP32_OFFBIG 125
#define _SC_V8_LP64_OFF64 126
#define _SC_V8_LPBIG_OFFBIG 127
#define _SC_XOPEN_CRYPT 128
#define _SC_XOPEN_ENH_I18N 129
#define _SC_XOPEN_REALTIME 130
#define _SC_XOPEN_REALTIME_THREADS 131
#define _SC_XOPEN_SHM 132
#define _SC_XOPEN_UNIX 133
#define _SC_XOPEN_UUCP 134
#define _SC_XOPEN_VERSION 135

//===----------------------------------------------------------------------===//
// pathconf / fpathconf names — numbering matches Linux.
//===----------------------------------------------------------------------===//

#define _PC_FILESIZEBITS 0
#define _PC_LINK_MAX 1
#define _PC_MAX_CANON 2
#define _PC_MAX_INPUT 3
#define _PC_NAME_MAX 4
#define _PC_PATH_MAX 5
#define _PC_PIPE_BUF 6
#define _PC_2_SYMLINKS 7
#define _PC_ALLOC_SIZE_MIN 8
#define _PC_REC_INCR_XFER_SIZE 9
#define _PC_REC_MAX_XFER_SIZE 10
#define _PC_REC_MIN_XFER_SIZE 11
#define _PC_REC_XFER_ALIGN 12
#define _PC_SYMLINK_MAX 13
#define _PC_CHOWN_RESTRICTED 14
#define _PC_NO_TRUNC 15
#define _PC_VDISABLE 16
#define _PC_ASYNC_IO 17
#define _PC_PRIO_IO 18
#define _PC_SYNC_IO 19
#define _PC_TIMESTAMP_RESOLUTION 20
#define _PC_TEXTDOMAIN_MAX 21
#define _PC_FALLOC 22

// POSIX limit constants.
#define _POSIX_MAX_CANON 255
#define _POSIX_MAX_INPUT 255
#define _POSIX_PATH_MAX 256
#define _POSIX_CHOWN_RESTRICTED 1
#define _POSIX_PIPE_BUF 512
#define _POSIX_NO_TRUNC 1
#define _POSIX_VDISABLE '\0'

#endif // LLVM_LIBC_MACROS_WINDOWS_UNISTD_MACROS_H
