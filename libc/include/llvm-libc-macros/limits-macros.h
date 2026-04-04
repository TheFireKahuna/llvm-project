//===-- Definition of macros from limits.h --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_LIMITS_MACROS_H
#define LLVM_LIBC_MACROS_LIMITS_MACROS_H

// Define all C23 macro constants of limits.h

#ifndef CHAR_BIT
#ifdef __CHAR_BIT__
#define CHAR_BIT __CHAR_BIT__
#else
#define CHAR_BIT 8
#endif // __CHAR_BIT__
#endif // CHAR_BIT

#ifndef MB_LEN_MAX
// Represents a single UTF-32 wide character in the default locale.
#define MB_LEN_MAX 4
#endif // MB_LEN_MAX

// *_WIDTH macros

#ifndef CHAR_WIDTH
#define CHAR_WIDTH CHAR_BIT
#endif // CHAR_WIDTH

#ifndef SCHAR_WIDTH
#define SCHAR_WIDTH CHAR_BIT
#endif // SCHAR_WIDTH

#ifndef UCHAR_WIDTH
#define UCHAR_WIDTH CHAR_BIT
#endif // UCHAR_WIDTH

#ifndef SHRT_WIDTH
#ifdef __SHRT_WIDTH__
#define SHRT_WIDTH __SHRT_WIDTH__
#else
#define SHRT_WIDTH 16
#endif // __SHRT_WIDTH__
#endif // SHRT_WIDTH

#ifndef USHRT_WIDTH
#define USHRT_WIDTH SHRT_WIDTH
#endif // USHRT_WIDTH

#ifndef INT_WIDTH
#ifdef __INT_WIDTH__
#define INT_WIDTH __INT_WIDTH__
#else
#define INT_WIDTH 32
#endif // __INT_WIDTH__
#endif // INT_WIDTH

#ifndef WORD_BIT
#define WORD_BIT INT_WIDTH
#endif // WORD_BIT

#ifndef UINT_WIDTH
#define UINT_WIDTH INT_WIDTH
#endif // UINT_WIDTH

#ifndef LONG_WIDTH
#ifdef __LONG_WIDTH__
#define LONG_WIDTH __LONG_WIDTH__
#elif defined(__WORDSIZE)
#define LONG_WIDTH __WORDSIZE
#else
// Use __SIZEOF_LONG__ * CHAR_BIT as backup.  This is needed for clang-13 or
// before.
#define LONG_WIDTH (__SIZEOF_LONG__ * CHAR_BIT)
#endif // __LONG_WIDTH__
#endif // LONG_WIDTH

#ifndef LONG_BIT
#define LONG_BIT LONG_WIDTH
#endif // LONG_BIT

#ifndef ULONG_WIDTH
#define ULONG_WIDTH LONG_WIDTH
#endif // ULONG_WIDTH

#ifndef LLONG_WIDTH
#ifdef __LLONG_WIDTH__
#define LLONG_WIDTH __LLONG_WIDTH__
#else
#define LLONG_WIDTH 64
#endif // __LLONG_WIDTH__
#endif // LLONG_WIDTH

#ifndef ULLONG_WIDTH
#define ULLONG_WIDTH LLONG_WIDTH
#endif // ULLONG_WIDTH

#ifndef BOOL_WIDTH
#ifdef __BOOL_WIDTH__
#define BOOL_WIDTH __BOOL_WIDTH__
#else
#define BOOL_WIDTH 1
#endif // __BOOL_WIDTH__
#endif // BOOL_WIDTH

// *_MAX macros

#ifndef SCHAR_MAX
#ifdef __SCHAR_MAX__
#define SCHAR_MAX __SCHAR_MAX__
#else
#define SCHAR_MAX 0x7f
#endif // __SCHAR_MAX__
#endif // SCHAR_MAX

#ifndef UCHAR_MAX
#define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#endif // UCHAR_MAX

// Check if char is unsigned.
#if !defined(__CHAR_UNSIGNED__) && ('\xff' > 0)
#define __CHAR_UNSIGNED__
#endif

#ifndef CHAR_MAX
#ifdef __CHAR_UNSIGNED__
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MAX SCHAR_MAX
#endif // __CHAR_UNSIGNED__
#endif // CHAR_MAX

#ifndef SHRT_MAX
#ifdef __SHRT_MAX__
#define SHRT_MAX __SHRT_MAX__
#else
#define SHRT_MAX 0x7fff
#endif // __SHRT_MAX__
#endif // SHRT_MAX

#ifndef USHRT_MAX
#define USHRT_MAX (SHRT_MAX * 2U + 1U)
#endif // USHRT_MAX

#ifndef INT_MAX
#ifdef __INT_MAX__
#define INT_MAX __INT_MAX__
#else
#define INT_MAX (0 ^ (1 << (INT_WIDTH - 1)))
#endif // __INT_MAX__
#endif // INT_MAX

#ifndef UINT_MAX
#define UINT_MAX (INT_MAX * 2U + 1U)
#endif // UINT_MAX

#ifndef LONG_MAX
#ifdef __LONG_MAX__
#define LONG_MAX __LONG_MAX__
#else
#define LONG_MAX (0L ^ (1L << (LONG_WIDTH - 1)))
#endif // __LONG_MAX__
#endif // LONG_MAX

#ifndef ULONG_MAX
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#endif // ULONG_MAX

#ifndef LLONG_MAX
#ifdef __LONG_LONG_MAX__
#define LLONG_MAX __LONG_LONG_MAX__
#else
#define LLONG_MAX (0LL ^ (1LL << (LLONG_WIDTH - 1)))
#endif // __LONG_LONG_MAX__
#endif // LLONG_MAX

#ifndef ULLONG_MAX
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif // ULLONG_MAX

// *_MIN macros

#ifndef SCHAR_MIN
#define SCHAR_MIN (-SCHAR_MAX - 1)
#endif // SCHAR_MIN

#ifndef UCHAR_MIN
#define UCHAR_MIN 0
#endif // UCHAR_MIN

#ifndef CHAR_MIN
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN UCHAR_MIN
#else
#define CHAR_MIN SCHAR_MIN
#endif // __CHAR_UNSIGNED__
#endif // CHAR_MIN

#ifndef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX - 1)
#endif // SHRT_MIN

#ifndef USHRT_MIN
#define USHRT_MIN 0U
#endif // USHRT_MIN

#ifndef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#endif // INT_MIN

#ifndef UINT_MIN
#define UINT_MIN 0U
#endif // UINT_MIN

#ifndef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#endif // LONG_MIN

#ifndef ULONG_MIN
#define ULONG_MIN 0UL
#endif // ULONG_MIN

#ifndef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#endif // LLONG_MIN

#ifndef ULLONG_MIN
#define ULLONG_MIN 0ULL
#endif // ULLONG_MIN

#ifndef SSIZE_MAX
#ifdef __PTRDIFF_MAX__
#define SSIZE_MAX __PTRDIFF_MAX__
#elif defined(__INTPTR_MAX__)
#define SSIZE_MAX __INTPTR_MAX__
#endif // __PTRDIFF_MAX__
#endif // SSIZE_MAX

#ifndef NL_LANGMAX
#define NL_LANGMAX INT_MAX
#endif // NL_LANGMAX

#ifndef NL_MSGMAX
#define NL_MSGMAX INT_MAX
#endif // NL_MSGMAX

#ifndef NL_NMAX
#define NL_NMAX INT_MAX
#endif // NL_NMAX

#ifndef NL_SETMAX
#define NL_SETMAX INT_MAX
#endif // NL_SETMAX

#ifndef NL_TEXTMAX
#define NL_TEXTMAX INT_MAX
#endif // NL_TEXTMAX

#ifndef NZERO
#define NZERO 20
#endif // NZERO

#ifndef _POSIX_MAX_CANON
#define _POSIX_MAX_CANON 255
#endif

#ifndef _POSIX_MAX_INPUT
#define _POSIX_MAX_INPUT 255
#endif

#ifndef _POSIX_NAME_MAX
#define _POSIX_PATH_MAX 256
#endif

#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX 4096
#endif

#ifndef NL_ARGMAX
#define NL_ARGMAX _POSIX_ARG_MAX
#endif // NL_ARGMAX

// POSIX thread-specific data key limit (XBD <limits.h>).
#ifndef PTHREAD_KEYS_MAX
#define PTHREAD_KEYS_MAX 1024
#endif

// PIPE_BUF: maximum number of bytes guaranteed to be written atomically
// to a pipe or FIFO. POSIX minimum is 512; we use 4096 (matches Linux).
#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif // IOV_MAX

#ifndef _XOPEN_IOV_MAX
#define _XOPEN_IOV_MAX 16
#endif // _XOPEN_IOV_MAX

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif // PATH_MAX

#ifndef _XOPEN_PATH_MAX
#define _XOPEN_PATH_MAX 1024
#endif // _XOPEN_PATH_MAX

#ifndef NAME_MAX
#define NAME_MAX 255
#endif // NAME_MAX

#ifndef _XOPEN_NAME_MAX
#define _XOPEN_NAME_MAX 255
#endif // _XOPEN_NAME_MAX

// POSIX.2 utility limits (XBD <limits.h>).
#ifndef BC_BASE_MAX
#define BC_BASE_MAX 99
#endif
#ifndef BC_DIM_MAX
#define BC_DIM_MAX 2048
#endif
#ifndef BC_SCALE_MAX
#define BC_SCALE_MAX 99
#endif
#ifndef BC_STRING_MAX
#define BC_STRING_MAX 1000
#endif
#ifndef EXPR_NEST_MAX
#define EXPR_NEST_MAX 32
#endif
#ifndef LINE_MAX
#define LINE_MAX 2048
#endif
#ifndef RE_DUP_MAX
#define RE_DUP_MAX 255
#endif
#ifndef NGROUPS_MAX
#define NGROUPS_MAX 65536
#endif
#ifndef CHARCLASS_NAME_MAX
#define CHARCLASS_NAME_MAX 2048
#endif
#ifndef COLL_WEIGHTS_MAX
#define COLL_WEIGHTS_MAX 2
#endif
#ifndef _POSIX_CLOCKRES_MIN
#define _POSIX_CLOCKRES_MIN 20000000
#endif
#ifndef _POSIX_AIO_LISTIO_MAX
#define _POSIX_AIO_LISTIO_MAX 2
#endif
#ifndef _POSIX_AIO_MAX
#define _POSIX_AIO_MAX 1
#endif
#ifndef _POSIX_DELAYTIMER_MAX
#define _POSIX_DELAYTIMER_MAX 32
#endif
#ifndef _POSIX_HOST_NAME_MAX
#define _POSIX_HOST_NAME_MAX 255
#endif
#ifndef _POSIX_LINK_MAX
#define _POSIX_LINK_MAX 8
#endif
#ifndef _POSIX_LOGIN_NAME_MAX
#define _POSIX_LOGIN_NAME_MAX 9
#endif
#ifndef _POSIX_MQ_OPEN_MAX
#define _POSIX_MQ_OPEN_MAX 8
#endif
#ifndef _POSIX_MQ_PRIO_MAX
#define _POSIX_MQ_PRIO_MAX 32
#endif
#ifndef _POSIX_OPEN_MAX
#define _POSIX_OPEN_MAX 20
#endif
#ifndef _POSIX_RE_DUP_MAX
#define _POSIX_RE_DUP_MAX 255
#endif
#ifndef _POSIX_RTSIG_MAX
#define _POSIX_RTSIG_MAX 8
#endif
#ifndef _POSIX_SEM_NSEMS_MAX
#define _POSIX_SEM_NSEMS_MAX 256
#endif
#ifndef _POSIX_SEM_VALUE_MAX
#define _POSIX_SEM_VALUE_MAX 32767
#endif
#ifndef _POSIX_SIGQUEUE_MAX
#define _POSIX_SIGQUEUE_MAX 32
#endif
#ifndef _POSIX_STREAM_MAX
#define _POSIX_STREAM_MAX 8
#endif
#ifndef _POSIX_SS_REPL_MAX
#define _POSIX_SS_REPL_MAX 4
#endif
#ifndef _POSIX_SYMLINK_MAX
#define _POSIX_SYMLINK_MAX 255
#endif
#ifndef _POSIX_SYMLOOP_MAX
#define _POSIX_SYMLOOP_MAX 8
#endif
#ifndef _POSIX_THREAD_DESTRUCTOR_ITERATIONS
#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#endif
#ifndef _POSIX_THREAD_KEYS_MAX
#define _POSIX_THREAD_KEYS_MAX 128
#endif
#ifndef _POSIX_THREAD_THREADS_MAX
#define _POSIX_THREAD_THREADS_MAX 64
#endif
#ifndef _POSIX_TIMER_MAX
#define _POSIX_TIMER_MAX 32
#endif
#ifndef _POSIX_TRACE_EVENT_NAME_MAX
#define _POSIX_TRACE_EVENT_NAME_MAX 30
#endif
#ifndef _POSIX_TRACE_NAME_MAX
#define _POSIX_TRACE_NAME_MAX 8
#endif
#ifndef _POSIX_TRACE_SYS_MAX
#define _POSIX_TRACE_SYS_MAX 8
#endif
#ifndef _POSIX_TRACE_USER_EVENT_MAX
#define _POSIX_TRACE_USER_EVENT_MAX 32
#endif
#ifndef _POSIX_TTY_NAME_MAX
#define _POSIX_TTY_NAME_MAX 9
#endif
#ifndef _POSIX_TZNAME_MAX
#define _POSIX_TZNAME_MAX 6
#endif
#ifndef _POSIX_NGROUPS_MAX
#define _POSIX_NGROUPS_MAX 8
#endif
#ifndef _POSIX_PIPE_BUF
#define _POSIX_PIPE_BUF 512
#endif
#ifndef _POSIX_SSIZE_MAX
#define _POSIX_SSIZE_MAX 32767
#endif
#ifndef _POSIX_CHILD_MAX
#define _POSIX_CHILD_MAX 25
#endif
#ifndef _POSIX_NAME_MAX
#define _POSIX_NAME_MAX 14
#endif
#ifndef _POSIX2_BC_BASE_MAX
#define _POSIX2_BC_BASE_MAX 99
#endif
#ifndef _POSIX2_BC_DIM_MAX
#define _POSIX2_BC_DIM_MAX 2048
#endif
#ifndef _POSIX2_BC_SCALE_MAX
#define _POSIX2_BC_SCALE_MAX 99
#endif
#ifndef _POSIX2_BC_STRING_MAX
#define _POSIX2_BC_STRING_MAX 1000
#endif
#ifndef _POSIX2_COLL_WEIGHTS_MAX
#define _POSIX2_COLL_WEIGHTS_MAX 2
#endif
#ifndef _POSIX2_EXPR_NEST_MAX
#define _POSIX2_EXPR_NEST_MAX 32
#endif
#ifndef _POSIX2_LINE_MAX
#define _POSIX2_LINE_MAX 2048
#endif
#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX 255
#endif
#ifndef _POSIX2_CHARCLASS_NAME_MAX
#define _POSIX2_CHARCLASS_NAME_MAX 14
#endif

#endif // LLVM_LIBC_MACROS_LIMITS_MACROS_H
