//===-- Definition of Windows signal number macros ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Uses POSIX-compatible signal numbers, NOT UCRT's incompatible values.
// UCRT uses different numbers (e.g., SIGABRT=22), but we use standard POSIX
// numbers for compatibility with portable code.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SIGNAL_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SIGNAL_MACROS_H

#include "__llvm-libc-common.h"

// Standard POSIX signal numbers
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
// SIGPIPE must be synthesized by the I/O layer (write path) when
// ERROR_BROKEN_PIPE is returned — Windows has no kernel SIGPIPE delivery.
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL SIGIO
#define SIGPWR 30
#define SIGSYS 31

// Real-time signals. Reserve 32-33 for internal use (matching glibc's NPTL
// convention where userspace SIGRTMIN is 34). No kernel RT delivery on
// Windows — these are software-only, delivered via the same APC/pending
// mechanism as standard signals. No POSIX queuing semantics (multiple
// sends coalesce like standard signals).
//
// __SIGRTMIN is the raw RT floor (32). Signals __SIGRTMIN through
// SIGRTMIN-1 (i.e. 32-33) are reserved for libc-internal use.
// sigprocmask strips these from user masks, matching glibc NPTL behavior.
#define __SIGRTMIN 32
#define SIGRTMIN 34
#define SIGRTMAX 63

#define NSIG 64

// Two 32-bit words: Windows LLP64 unsigned long is 32 bits, so 64 signals
// require two words. Internal code uses uint64_t atomics for lock-free ops
// and converts at the sigset_t boundary.
#define __NSIGSET_WORDS 2

#define SIG_BLOCK 0   // For blocking signals
#define SIG_UNBLOCK 1 // For unblocking signals
#define SIG_SETMASK 2 // For setting signal mask

// Flag values for sigaction.sa_flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_ONSTACK 0x08000000

// Signal stack flags
#define SS_ONSTACK 0x1
#define SS_DISABLE 0x2
#define SS_AUTODISARM (1U << 31) // Matches Linux value

// Stack sizes
#define MINSIGSTKSZ 2048
#define SIGSTKSZ 8192

#define SIG_ERR __LLVM_LIBC_CAST(reinterpret_cast, void (*)(int), -1)
#define SIG_DFL __LLVM_LIBC_CAST(reinterpret_cast, void (*)(int), 0)
#define SIG_IGN __LLVM_LIBC_CAST(reinterpret_cast, void (*)(int), 1)
#define SIG_HOLD __LLVM_LIBC_CAST(reinterpret_cast, void (*)(int), 2)

// sigev_notify values for struct sigevent
#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2

// si_code values: origin of the signal
#define SI_USER 0    // kill(), raise(), or abort()
#define SI_KERNEL 128 // Sent by the kernel
#define SI_QUEUE -1
#define SI_TIMER -2
#define SI_MESGQ -3
#define SI_ASYNCIO -4
#define SI_SIGIO -5
#define SI_TKILL -6

// SIGSEGV si_codes
#define SEGV_MAPERR 1 // Address not mapped to object
#define SEGV_ACCERR 2 // Invalid permissions for mapped object

// SIGBUS si_codes
#define BUS_ADRALN 1 // Invalid address alignment
#define BUS_ADRERR 2 // Nonexistent physical address
#define BUS_OBJERR 3 // Object-specific hardware error

// SIGILL si_codes
#define ILL_ILLOPC 1 // Illegal opcode
#define ILL_ILLOPN 2 // Illegal operand
#define ILL_ILLADR 3 // Illegal addressing mode
#define ILL_ILLTRP 4 // Illegal trap
#define ILL_PRVOPC 5 // Privileged opcode
#define ILL_PRVREG 6 // Privileged register
#define ILL_COPROC 7 // Coprocessor error
#define ILL_BADSTK 8 // Internal stack error

// SIGFPE si_codes
#define FPE_INTDIV 1 // Integer divide by zero
#define FPE_INTOVF 2 // Integer overflow
#define FPE_FLTDIV 3 // Floating-point divide by zero
#define FPE_FLTOVF 4 // Floating-point overflow
#define FPE_FLTUND 5 // Floating-point underflow
#define FPE_FLTRES 6 // Floating-point inexact result
#define FPE_FLTINV 7 // Invalid floating-point operation
#define FPE_FLTSUB 8 // Subscript out of range

// SIGTRAP si_codes
#define TRAP_BRKPT 1  // Process breakpoint
#define TRAP_TRACE 2  // Process trace trap
#define TRAP_BRANCH 3 // Process taken branch trap
#define TRAP_HWBKPT 4 // Hardware breakpoint/watchpoint
#define TRAP_UNK 5    // Undiagnosed trap

// SIGCHLD si_codes
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

#endif // LLVM_LIBC_MACROS_WINDOWS_SIGNAL_MACROS_H
