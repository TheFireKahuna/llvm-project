//===---------- Windows POSIX syscall dispatch -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX syscall personality for Windows. Implements syscall_impl as a
// zero-cost dispatch table that routes Linux syscall numbers to the
// corresponding Windows userspace engines.
//
// On Linux, syscall_impl compiles to a single `syscall` instruction via
// inline asm — the kernel is the dispatch target. On Windows, NT doesn't
// have stable syscall numbers, and the POSIX signal/thread semantics are
// implemented in userspace. syscall_impl dispatches to those engines.
//
// Since every call site passes the syscall number as a compile-time
// constant (SYS_*), the compiler constant-folds the switch and emits a
// direct call to the target function. The dispatch compiles away entirely.
//
// Namespace layering:
//   Two engine namespaces, matching the subsystem boundary:
//     internal::       — all non-signal engines (process identity, I/O,
//                        memory, time, scheduling, lifecycle)
//     signal_state::   — signal subsystem (kill, sigaction, sigprocmask,
//                        tgkill, sigaltstack)
//   This mirrors the physical code organization: signal/ is a separate
//   subsystem with its own state, init, and teardown lifecycle.
//   The windows_syscalls:: wrapper layer (syscall_wrappers/*.h) sits
//   above this, providing ErrorOr<T> conversion — matching upstream
//   linux_syscalls:: convention.
//
// Inlining discipline:
//   - syscall_impl and syscall_dispatch are [[gnu::always_inline]] —
//     the switch MUST be inlined for constant folding. As the switch
//     grows, compiler cost heuristics become LESS likely to inline it;
//     always_inline prevents this regression.
//   - Trivial targets (TEB reads, atomic loads, NtYieldExecution) are
//     [[gnu::always_inline]] — a function call (~5ns) on a 1-instruction
//     operation (~1ns) is a 5x penalty. Force inline to emit raw insn.
//   - Complex targets (mmap, kill, sigprocmask, read/write) are
//     forward-declared and out-of-line. A 3-5ns call overhead on a
//     100-1000ns NT API call is noise. The switch constant-folds to a
//     single `call adapter` — zero dispatch overhead, just the inherent
//     function call.
//
// Error convention matches Linux: 0 or positive on success, negative
// errno on failure (e.g., -ESRCH). Callers negate to recover the code.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_H

#include "src/__support/CPP/bit.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/syscall_numbers.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getpriority.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpgid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpriority.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"
#include "hdr/types/clockid_t.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/off_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/ssize_t.h"
#include "hdr/types/struct_timespec.h"

namespace LIBC_NAMESPACE_DECL {

// Re-export error helpers for convenience (backward compat).
using windows_util::get_last_error_as_errno;
using windows_util::win32_to_errno;

// ===========================================================================
// Trivial inline targets (internal::) — [[gnu::always_inline]]
//
// These compile to 1-3 instructions. A function call would dominate the
// actual work. Force inline so the compiler emits the raw instruction
// at the call site after constant-folding the switch.
// ===========================================================================

namespace internal {

// --- Process identity (TEB reads / atomic loads) ---

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_getpid() {
  return static_cast<intptr_t>(NtCurrentProcessId());
}

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_gettid() {
  return static_cast<intptr_t>(NtCurrentThreadId());
}

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_getuid() {
  return static_cast<intptr_t>(
      g_pcb.identity.real_uid.load(cpp::MemoryOrder::RELAXED));
}

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_geteuid() {
  return static_cast<intptr_t>(
      g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED));
}

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_getgid() {
  return static_cast<intptr_t>(
      g_pcb.identity.real_gid.load(cpp::MemoryOrder::RELAXED));
}

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_getegid() {
  return static_cast<intptr_t>(
      g_pcb.identity.eff_gid.load(cpp::MemoryOrder::RELAXED));
}

// --- Scheduling ---

[[gnu::always_inline]] LIBC_INLINE intptr_t sys_sched_yield() {
  ::NtYieldExecution();
  return 0;
}

// --- Process identity (cached NT query — fast path is atomic load) ---

LIBC_INLINE intptr_t sys_getppid() {
  // Parent PID never changes. PID 0 (System Idle Process) can never be a
  // parent, so it serves as the "not yet queried" sentinel.
  static cpp::Atomic<pid_t> cached_ppid{0};
  pid_t ppid = cached_ppid.load(cpp::MemoryOrder::RELAXED);
  if (ppid != 0)
    return static_cast<intptr_t>(ppid);
  PROCESS_BASIC_INFORMATION pbi;
  NTSTATUS status = ::NtQueryInformationProcess(
      NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
  if (!NT_SUCCESS(status))
    return -static_cast<intptr_t>(EIO);
  ppid = static_cast<pid_t>(pbi.InheritedFromUniqueProcessId);
  cached_ppid.store(ppid, cpp::MemoryOrder::RELAXED);
  return static_cast<intptr_t>(ppid);
}

// --- Session management (atomic loads + PEB write) ---

LIBC_INLINE intptr_t sys_getsid(intptr_t pid) {
  auto result = windows_syscalls::getsid(static_cast<pid_t>(pid));
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return static_cast<intptr_t>(result.value());
}

LIBC_INLINE intptr_t sys_setsid() {
  auto result = windows_syscalls::setsid();
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return static_cast<intptr_t>(result.value());
}

// --- Priority management (NtGetNextProcess-based system-wide walk) ---

LIBC_INLINE intptr_t sys_getpriority(intptr_t which, intptr_t who) {
  auto result = windows_syscalls::getpriority(static_cast<int>(which),
                                              static_cast<id_t>(who));
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  // Return kNZero - nice (range 1..40) — same encoding as the Linux kernel.
  return static_cast<intptr_t>(internal::kNZero - result.value());
}

LIBC_INLINE intptr_t sys_setpriority(intptr_t which, intptr_t who,
                                     intptr_t nice) {
  auto result = windows_syscalls::setpriority(
      static_cast<int>(which), static_cast<id_t>(who), static_cast<int>(nice));
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return 0;
}

// --- Process group management (metadata-based — no NT kernel objects) ---

LIBC_INLINE intptr_t sys_setpgid(intptr_t pid, intptr_t pgid) {
  auto result = windows_syscalls::setpgid(static_cast<pid_t>(pid),
                                          static_cast<pid_t>(pgid));
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return 0;
}

LIBC_INLINE intptr_t sys_getpgid(intptr_t pid) {
  auto result = windows_syscalls::getpgid(static_cast<pid_t>(pid));
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return static_cast<intptr_t>(result.value());
}

LIBC_INLINE intptr_t sys_getpgrp() {
  return static_cast<intptr_t>(windows_syscalls::getpgrp());
}

// --- Process lifecycle ---

LIBC_INLINE intptr_t sys_exit(intptr_t status) {
  ::NtTerminateProcess(NtCurrentProcess(), static_cast<NTSTATUS>(status));
  __builtin_unreachable();
}

} // namespace internal

// ===========================================================================
// Complex out-of-line targets — forward declarations only.
//
// These are substantial functions (10-500+ lines, NT API calls). The
// compiler emits a direct `call adapter` after constant-folding the
// switch — zero dispatch overhead, just the inherent function call.
//
// These are the Windows "kernel" functions — they implement the Linux
// syscall semantics in userspace. Like Linux kernel syscall handlers,
// they return -errno on failure, 0 or positive on success. The
// LLVM_LIBC_FUNCTION entry points convert to POSIX ABI (return -1,
// set libc_errno) — that's the libc/kernel boundary.
//
// Link-time resolution: for targets that would create CMake circular
// deps (fd_table, mapping_table, clock), we use forward declarations
// only — no CMake dep on `syscall`. The linker resolves the symbol
// from the implementation's object library at final link.
// ===========================================================================

// --- Signal subsystem (CMake dep: signal_subsystem, no cycle) ---
// These are "kernel" functions: return -errno on failure, 0 on success.
namespace signal_state {
intptr_t deliver_signal_to_thread(DWORD tid, int signum);
intptr_t kill(intptr_t pid, int sig);
intptr_t rt_sigprocmask(int how, const void *set, void *oldset);
intptr_t rt_sigaction(int sig, const void *act, void *oldact);
intptr_t sigaltstack(const void *ss, void *oss);
} // namespace signal_state

// --- Memory management (forward-declared only — would create CMake cycle) ---
// These are kernel functions: return 0 on success (mprotect/munmap) or
// address-as-intptr_t on success (mmap/brk), -errno on failure (mmap) or
// current-break-unchanged on failure (brk). Forward-declared here because
// including the implementation headers would create a mapping_table → osutil
// → syscall cycle. The linker resolves these from the mman/brk object
// libraries at final link.
namespace internal {
intptr_t mprotect(void *addr, size_t size, int prot);
intptr_t munmap(void *addr, size_t size);
intptr_t mmap(void *addr, size_t size, int prot, int flags, int fd,
              off_t offset);
intptr_t sys_brk(void *addr);
} // namespace internal

// --- File I/O (forward-declared only — would create CMake cycle) ---
// These are kernel functions: return value on success, -errno on failure.
// Forward-declared here because including the implementation headers would
// create a fd_table → osutil → syscall cycle. The linker resolves these
// from the fcntl and read_write object libraries at final link.
namespace internal {
// File control (fcntl.cpp):
intptr_t open(const char *path, int flags, mode_t mode);
intptr_t openat(int dirfd, const char *path, int flags, mode_t mode);
intptr_t close(int fd);
intptr_t lseek(int fd, off_t offset, int whence);
intptr_t dup2(int oldfd, int newfd);
// Data transfer (read_write.cpp):
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
// Time (forward-declared only — would create CMake cycle via osutil):
// clock_gettime returns ErrorOr<int> (cross-platform contract shared with
// Linux VDSO path); dispatch adapter translates to kernel convention.
// nanosleep/clock_nanosleep return intptr_t with -errno natively.
ErrorOr<int> clock_gettime(clockid_t clockid, timespec *ts);
intptr_t nanosleep(const timespec *req, timespec *rem);
intptr_t clock_nanosleep(clockid_t clockid, int flags, const timespec *req,
                         timespec *rem);
// Inotify (inotify_ops.cpp):
intptr_t inotify_init1(int flags);
intptr_t inotify_add_watch(int fd, const char *pathname, uint32_t mask);
intptr_t inotify_rm_watch(int fd, int wd);
} // namespace internal

// ===========================================================================
// Dispatch core — [[gnu::always_inline]], non-negotiable.
//
// The switch MUST be inlined at every call site so the compiler sees the
// compile-time constant SYS_* number, constant-folds the switch, and
// dead-code eliminates all other branches. As the case count grows,
// compiler cost heuristics become LESS likely to inline; always_inline
// prevents this regression.
// ===========================================================================

namespace internal {

[[gnu::always_inline]] LIBC_INLINE intptr_t
syscall_dispatch(intptr_t __number, intptr_t __arg1, intptr_t __arg2,
                 intptr_t __arg3, intptr_t __arg4, intptr_t __arg5,
                 intptr_t __arg6) {

  switch (__number) {

  // --- Process identity (trivial inline — raw instructions) ---

  case SYS_getpid:
    return sys_getpid();

  case SYS_gettid:
    return sys_gettid();

  case SYS_getuid:
    return sys_getuid();

  case SYS_geteuid:
    return sys_geteuid();

  case SYS_getgid:
    return sys_getgid();

  case SYS_getegid:
    return sys_getegid();

  case SYS_getppid:
    return sys_getppid();

  case SYS_setpgid:
    return sys_setpgid(__arg1, __arg2);

  case SYS_getpgid:
    return sys_getpgid(__arg1);

  case SYS_getpgrp:
    return sys_getpgrp();

  case SYS_setsid:
    return sys_setsid();

  case SYS_getsid:
    return sys_getsid(__arg1);

  // --- Resource priority (system-wide NtGetNextProcess walk) ---

  case SYS_getpriority:
    return sys_getpriority(__arg1, __arg2);

  case SYS_setpriority:
    return sys_setpriority(__arg1, __arg2, __arg3);

  // --- Scheduling (trivial inline) ---

  case SYS_sched_yield:
    return sys_sched_yield();

  // --- Process lifecycle (inline — 3 instructions + noreturn) ---

  case SYS_exit:
  case SYS_exit_group:
    sys_exit(__arg1);
    __builtin_unreachable();

  // --- Signals (out-of-line — kernel functions) ---

  case SYS_tgkill:
    // tgkill(tgid, tid, sig) — tgid (__arg1) ignored; Windows threads
    // all share the same PID. deliver_signal_to_thread handles self-
    // delivery synchronously (matching Linux kernel tgkill semantics).
    return signal_state::deliver_signal_to_thread(
        static_cast<DWORD>(__arg2), static_cast<int>(__arg3));

  case SYS_rt_sigprocmask:
    // 4th arg (sigsetsize) ignored — Windows sigset_t is fixed-size.
    return signal_state::rt_sigprocmask(
        static_cast<int>(__arg1), reinterpret_cast<const void *>(__arg2),
        reinterpret_cast<void *>(__arg3));

  case SYS_kill:
    return signal_state::kill(__arg1, static_cast<int>(__arg2));

  case SYS_rt_sigaction:
    return signal_state::rt_sigaction(
        static_cast<int>(__arg1), reinterpret_cast<const void *>(__arg2),
        reinterpret_cast<void *>(__arg3));

  case SYS_sigaltstack:
    return signal_state::sigaltstack(
        reinterpret_cast<const void *>(__arg1),
        reinterpret_cast<void *>(__arg2));

  // --- File I/O (out-of-line — fd_table, link-time resolution) ---

  case SYS_open:
    return internal::open(reinterpret_cast<const char *>(__arg1),
                          static_cast<int>(__arg2),
                          static_cast<mode_t>(__arg3));

  case SYS_openat:
    return internal::openat(static_cast<int>(__arg1),
                            reinterpret_cast<const char *>(__arg2),
                            static_cast<int>(__arg3),
                            static_cast<mode_t>(__arg4));

  case SYS_close:
    return internal::close(static_cast<int>(__arg1));

  case SYS_lseek:
    return internal::lseek(static_cast<int>(__arg1),
                           static_cast<off_t>(__arg2),
                           static_cast<int>(__arg3));

  case SYS_dup2:
    return internal::dup2(static_cast<int>(__arg1),
                          static_cast<int>(__arg2));

  // --- Data transfer (out-of-line — fd_table + IO Ring, link-time) ---

  case SYS_read:
    return internal::read(static_cast<int>(__arg1),
                          reinterpret_cast<void *>(__arg2),
                          static_cast<size_t>(__arg3));

  case SYS_write:
    return internal::write(static_cast<int>(__arg1),
                           reinterpret_cast<const void *>(__arg2),
                           static_cast<size_t>(__arg3));

  // --- Memory management (out-of-line — mapping_table, link-time) ---

  case SYS_mprotect:
    return internal::mprotect(reinterpret_cast<void *>(__arg1),
                              static_cast<size_t>(__arg2),
                              static_cast<int>(__arg3));

  case SYS_munmap:
    return internal::munmap(reinterpret_cast<void *>(__arg1),
                            static_cast<size_t>(__arg2));

  case SYS_mmap:
    return internal::mmap(reinterpret_cast<void *>(__arg1),
                          static_cast<size_t>(__arg2),
                          static_cast<int>(__arg3),
                          static_cast<int>(__arg4),
                          static_cast<int>(__arg5),
                          static_cast<off_t>(__arg6));

  case SYS_brk:
    return internal::sys_brk(reinterpret_cast<void *>(__arg1));

  // --- Time (out-of-line — clock sources, link-time resolution) ---

  case SYS_clock_gettime: {
    // clock_gettime uses the cross-platform ErrorOr<int> contract (shared
    // with Linux VDSO path). Translate to kernel convention here.
    auto result = internal::clock_gettime(
        static_cast<clockid_t>(__arg1),
        reinterpret_cast<timespec *>(__arg2));
    return result.has_value() ? intptr_t{0}
                              : -static_cast<intptr_t>(result.error());
  }

  case SYS_nanosleep:
    return internal::nanosleep(reinterpret_cast<const timespec *>(__arg1),
                               reinterpret_cast<timespec *>(__arg2));

  case SYS_clock_nanosleep:
    return internal::clock_nanosleep(
        static_cast<clockid_t>(__arg1), static_cast<int>(__arg2),
        reinterpret_cast<const timespec *>(__arg3),
        reinterpret_cast<timespec *>(__arg4));

  // --- Inotify (out-of-line — inotify_ops, link-time resolution) ---

  case SYS_inotify_init1:
    return internal::inotify_init1(static_cast<int>(__arg1));

  case SYS_inotify_add_watch:
    return internal::inotify_add_watch(
        static_cast<int>(__arg1),
        reinterpret_cast<const char *>(__arg2),
        static_cast<uint32_t>(__arg3));

  case SYS_inotify_rm_watch:
    return internal::inotify_rm_watch(static_cast<int>(__arg1),
                                      static_cast<int>(__arg2));

  default:
    return -ENOSYS;
  }
}

} // namespace internal

// ---------------------------------------------------------------------------
// syscall_impl overloads — match Linux arch-specific signatures exactly.
//
// [[gnu::always_inline]] ensures the switch is inlined at every call site.
// Since the syscall number is always a compile-time constant (SYS_*),
// the compiler constant-folds the switch, dead-code eliminates all other
// branches, and emits a direct call to the target function.
// ---------------------------------------------------------------------------

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number) {
  return internal::syscall_dispatch(__number, 0, 0, 0, 0, 0, 0);
}

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number,
                                                         intptr_t __arg1) {
  return internal::syscall_dispatch(__number, __arg1, 0, 0, 0, 0, 0);
}

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number,
                                                         intptr_t __arg1,
                                                         intptr_t __arg2) {
  return internal::syscall_dispatch(__number, __arg1, __arg2, 0, 0, 0, 0);
}

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number,
                                                         intptr_t __arg1,
                                                         intptr_t __arg2,
                                                         intptr_t __arg3) {
  return internal::syscall_dispatch(__number, __arg1, __arg2, __arg3, 0, 0, 0);
}

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number,
                                                         intptr_t __arg1,
                                                         intptr_t __arg2,
                                                         intptr_t __arg3,
                                                         intptr_t __arg4) {
  return internal::syscall_dispatch(__number, __arg1, __arg2, __arg3, __arg4,
                                   0, 0);
}

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number,
                                                         intptr_t __arg1,
                                                         intptr_t __arg2,
                                                         intptr_t __arg3,
                                                         intptr_t __arg4,
                                                         intptr_t __arg5) {
  return internal::syscall_dispatch(__number, __arg1, __arg2, __arg3, __arg4,
                                   __arg5, 0);
}

[[gnu::always_inline]] LIBC_INLINE intptr_t syscall_impl(intptr_t __number,
                                                         intptr_t __arg1,
                                                         intptr_t __arg2,
                                                         intptr_t __arg3,
                                                         intptr_t __arg4,
                                                         intptr_t __arg5,
                                                         intptr_t __arg6) {
  return internal::syscall_dispatch(__number, __arg1, __arg2, __arg3, __arg4,
                                   __arg5, __arg6);
}

// Template wrapper — casts all args to intptr_t and the result to R.
// Analogous to the Linux OSUtil/linux/syscall.h interface (which uses long,
// but long is pointer-width on LP64; intptr_t is pointer-width everywhere).
template <typename R, typename... Ts>
LIBC_INLINE R syscall_impl(intptr_t __number, Ts... ts) {
  static_assert(sizeof...(Ts) <= 6, "Too many arguments for syscall");
  return cpp::bit_or_static_cast<R>(
      syscall_impl(__number, (intptr_t)ts...));
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_H
