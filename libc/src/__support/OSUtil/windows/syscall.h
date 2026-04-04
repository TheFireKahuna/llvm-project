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
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/syscall_numbers.h"
#include "src/__support/threads/windows/futex_addr.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getpriority.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpgid.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/setpriority.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include "hdr/errno_macros.h"
#include "hdr/time_macros.h"
#include "hdr/types/clockid_t.h"
#include "hdr/types/gid_t.h"
#include "hdr/types/id_t.h"
#include "hdr/types/idtype_t.h"
#include "hdr/types/key_t.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/nfds_t.h"
#include "hdr/types/off_t.h"
#include "hdr/types/pid_t.h"
#include "hdr/types/siginfo_t.h"
#include "hdr/types/sigset_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/socklen_t.h"
#include "hdr/types/ssize_t.h"
#include "hdr/types/stack_t.h"
#include "hdr/types/struct_timespec.h"
#include "hdr/types/timer_t.h"
#include "hdr/types/uid_t.h"
#include "hdr/types/union_sigval.h"

// Struct types referenced by dispatch adapters. Included via proxy headers
// so the struct definitions are in the global namespace (as POSIX mandates)
// without upsetting clang-tidy's LIBC_NAMESPACE_DECL enclosure rule.
#include "hdr/types/struct_epoll_event.h"
#include "hdr/types/struct_iovec.h"
#include "hdr/types/struct_itimerspec.h"
#include "hdr/types/struct_itimerval.h"
#include "hdr/types/struct_msghdr.h"
#include "hdr/types/struct_pollfd.h"
#include "hdr/types/struct_rlimit.h"
#include "hdr/types/struct_rusage.h"
#include "hdr/types/struct_sched_param.h"
#include "hdr/types/struct_sembuf.h"
#include "hdr/types/struct_shmid_ds.h"
#include "hdr/types/struct_sigevent.h"
#include "hdr/types/struct_sockaddr.h"
#include "hdr/types/struct_timeval.h"
#include "hdr/types/cpu_set_t.h"
#include "include/llvm-libc-types/fd_set.h"
#include "include/llvm-libc-types/struct_stat.h"

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
intptr_t dup(int oldfd);
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

// --- Mass-wire forward declarations ---------------------------------------
// All engines implement Linux syscall semantics (return value on success,
// -errno on failure). Grouped by subsystem. Linker resolves each from its
// own object library at final link; `.syscall_dispatch_targets` aggregates
// them so freestanding test binaries still get a closed link.
namespace internal {

// File I/O — stat family (stat_ops.cpp):
intptr_t stat(const char *path, struct stat *statbuf);
intptr_t lstat(const char *path, struct stat *statbuf);
intptr_t fstat(int fd, struct stat *statbuf);
intptr_t fstatat(int dfd, const char *path, struct stat *statbuf, int flags);

// File I/O — data transfer (file_ops.cpp / scatter_io_ops.cpp):
intptr_t pread(int fd, void *buf, size_t count, off_t offset);
intptr_t pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

// File I/O — fd lifecycle + sync + truncate (file_ops.cpp):
intptr_t dup3(int oldfd, int newfd, int flags);
intptr_t pipe2(int pipefd[2], int flags);
intptr_t fsync(int fd);
intptr_t fdatasync(int fd);
intptr_t ftruncate(int fd, off_t length);
intptr_t truncate(const char *path, off_t length);
intptr_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                         size_t len, unsigned int flags);

// fcntl (fcntl.cpp) — returns ErrorOr<int>; dispatch adapter unpacks:
ErrorOr<int> fcntl(int fd, int cmd, void *arg);

// Path ops (path_ops.cpp):
intptr_t faccessat(int dfd, const char *path, int amode, int flag);
intptr_t unlinkat(int dfd, const char *path, int flags);
intptr_t linkat(int olddfd, const char *oldpath, int newdfd,
                const char *newpath, int flags);
intptr_t symlinkat(const char *target, int newdfd, const char *linkpath);
intptr_t readlinkat(int dfd, const char *path, char *buf, size_t bufsiz);
intptr_t chdir(const char *path);
intptr_t fchdir(int fd);
intptr_t getcwd(char *buf, size_t size);

// Directory + rename (mkdir_ops.cpp / rename_ops.cpp):
intptr_t mkdirat(int dfd, const char *path, mode_t mode);
intptr_t renameat(int olddfd, const char *oldpath, int newdfd,
                  const char *newpath);

// Timestamps (utimes_ops.cpp):
intptr_t utimensat(int dirfd, const char *path, const struct timespec times[2],
                   int flags);

// Permissions (chmod_ops.cpp / ownership_ops.cpp):
intptr_t chmod(const char *path, mode_t mode);
intptr_t fchmod(int fd, mode_t mode);
intptr_t fchmodat(int dfd, const char *path, mode_t mode, int flags);
intptr_t chown(const char *path, uid_t owner, gid_t group);
intptr_t fchown(int fd, uid_t owner, gid_t group);
intptr_t fchownat(int dfd, const char *path, uid_t owner, gid_t group,
                  int flags);

// Cross-fd transfer:
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

// Memory-backed fds (shm_ops.cpp):
intptr_t memfd_create(const char *name, unsigned int flags);

// Memory (vm_protect.cpp / mremap_engine.cpp / madvise_ops.cpp / ...):
intptr_t mremap(void *old_addr, size_t old_size, size_t new_size, int flags,
                void *new_addr);
intptr_t madvise(void *addr, size_t length, int advice);
intptr_t mincore(void *addr, size_t length, unsigned char *vec);
intptr_t msync(void *addr, size_t length, int flags);
intptr_t mlock(const void *addr, size_t len);
intptr_t mlock2(const void *addr, size_t len, int flags);
intptr_t mlockall(int flags);
intptr_t munlock(const void *addr, size_t len);
intptr_t munlockall();
intptr_t remap_file_pages(void *addr, size_t size, int prot, size_t pgoff,
                          int flags);
intptr_t set_mempolicy(int mode, const unsigned long *nodemask,
                       unsigned long maxnode);
intptr_t get_mempolicy(int *mode, unsigned long *nodemask,
                       unsigned long maxnode, void *addr, unsigned long flags);
intptr_t mbind(void *addr, unsigned long len, int mode,
               const unsigned long *nodemask, unsigned long maxnode,
               unsigned flags);
intptr_t pkey_mprotect(void *addr, size_t len, int prot, int pkey);
intptr_t pkey_alloc(unsigned int flags, unsigned int access_rights);
intptr_t pkey_free(int pkey);

// SysV shared memory (sysv_shm_ops.cpp):
intptr_t shmget(key_t key, size_t size, int shmflg);
intptr_t shmat(int shmid, const void *shmaddr, int shmflg);
intptr_t shmdt(const void *shmaddr);
intptr_t shmctl(int shmid, int cmd, struct shmid_ds *buf);

// Signal wait / query (signal_syscalls.cpp):
intptr_t sigpending(sigset_t *set);
intptr_t sigsuspend(const sigset_t *mask);
intptr_t sigtimedwait(const sigset_t *set, siginfo_t *info,
                      const struct timespec *timeout);
intptr_t sigqueue(pid_t pid, int sig, const union sigval value);
intptr_t sigqueue_thread(pid_t tid, int sig, const union sigval value);

// Process lifecycle / wait (exec_ops.cpp / wait_ops.cpp / unistd/windows/fork.cpp):
// sys_fork uses the asm("__llvm_libc_sys_fork") rename so this C++
// declaration resolves to the naked-asm engine symbol at link time.
intptr_t sys_fork(void) asm("__llvm_libc_sys_fork");
intptr_t execve(const char *path, char *const argv[], char *const envp[]);
intptr_t execveat(int dirfd, const char *pathname, char *const argv[],
                  char *const envp[], int flags);
intptr_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
intptr_t waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);

// Identity / credentials (identity_ops.cpp):
intptr_t setuid(uid_t uid);
intptr_t setgid(gid_t gid);
intptr_t setreuid(uid_t ruid, uid_t euid);
intptr_t setregid(gid_t rgid, gid_t egid);
intptr_t setresuid(uid_t ruid, uid_t euid, uid_t suid);
intptr_t setresgid(gid_t rgid, gid_t egid, gid_t sgid);
intptr_t getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
intptr_t getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
intptr_t getgroups(int size, gid_t list[]);

// Resource limits / accounting (resource_ops.cpp):
intptr_t getrlimit(int resource, struct rlimit *lim);
intptr_t setrlimit(int resource, const struct rlimit *lim);
intptr_t prlimit(int pid, int resource, const struct rlimit *new_limit,
                 struct rlimit *old_limit);
intptr_t getrusage(int who, struct rusage *usage);

// CPU identity (sched_ops.cpp):
intptr_t getcpu(unsigned int *cpu, unsigned int *node);

// Sockets — lifecycle (socket_lifecycle.cpp):
intptr_t socket(int domain, int type, int protocol);
intptr_t bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
intptr_t listen(int sockfd, int backlog);
intptr_t connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
intptr_t accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
intptr_t shutdown(int sockfd, int how);
intptr_t socketpair(int domain, int type, int protocol, int sv[2]);

// Sockets — data (socket_data.cpp):
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

// Sockets — query (socket_query.cpp):
intptr_t getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
intptr_t getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
intptr_t setsockopt(int sockfd, int level, int optname, const void *optval,
                    socklen_t optlen);
intptr_t getsockopt(int sockfd, int level, int optname, void *optval,
                    socklen_t *optlen);

// Multiplexing (poll_ops.cpp / select_ops.cpp / epoll_ops.cpp):
intptr_t poll(struct pollfd *fds, nfds_t nfds, int timeout);
intptr_t ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo,
               const sigset_t *sigmask);
intptr_t select(int nfds, fd_set *read_set, fd_set *write_set,
                fd_set *error_set, struct timeval *timeout);
intptr_t pselect(int nfds, fd_set *read_set, fd_set *write_set,
                 fd_set *error_set, const struct timespec *timeout,
                 const sigset_t *sigmask);
intptr_t epoll_create1(int flags);
intptr_t epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
intptr_t epoll_wait(int epfd, struct epoll_event *events, int maxevents,
                    int timeout);

// Legacy inotify (inotify_ops.cpp):
// inotify_init(void) is inotify_init1(0); no separate engine needed.

// SysV semaphores (sysv_sem_ops.cpp):
intptr_t semget(key_t key, int nsems, int semflg);
intptr_t semop(int semid, struct sembuf *sops, size_t nsops);
intptr_t semctl(int semid, int semnum, int cmd, intptr_t cmd_arg);

// Time — clock_settime / clock_getres (ErrorOr contract for clock_settime;
// clock_getres returns intptr_t kernel convention):
ErrorOr<int> clock_settime(clockid_t clockid, const timespec *ts);
intptr_t clock_getres(clockid_t id, struct timespec *res);

// POSIX timers (timer_ops.cpp):
intptr_t timer_create(clockid_t clockid, struct sigevent *sevp,
                      timer_t *timerid);
intptr_t timer_settime(timer_t timerid, int flags,
                       const struct itimerspec *new_value,
                       struct itimerspec *old_value);
intptr_t timer_gettime(timer_t timerid, struct itimerspec *curr_value);
intptr_t timer_getoverrun(timer_t timerid);
intptr_t timer_delete(timer_t timerid);

// Interval timers (itimer_ops.cpp):
intptr_t setitimer(int which, const struct itimerval *new_value,
                   struct itimerval *old_value);
intptr_t getitimer(int which, struct itimerval *curr_value);

// Scheduling (scheduler_ops.cpp / affinity_ops.cpp / sched_ops.cpp):
intptr_t sched_setscheduler(pid_t tid, int policy,
                            const struct sched_param *param);
intptr_t sched_getscheduler(pid_t tid);
intptr_t sched_setparam(pid_t tid, const struct sched_param *param);
intptr_t sched_getparam(pid_t tid, struct sched_param *param);
intptr_t sched_get_priority_max(int policy);
intptr_t sched_get_priority_min(int policy);
intptr_t sched_rr_get_interval(pid_t tid, struct timespec *tp);
intptr_t sched_getaffinity(pid_t tid, size_t cpuset_size, cpu_set_t *mask);
intptr_t sched_setaffinity(pid_t tid, size_t cpuset_size,
                           const cpu_set_t *mask);

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

  case SYS_dup:
    return internal::dup(static_cast<int>(__arg1));

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

  case SYS_inotify_init:
    // Legacy inotify_init() == inotify_init1(0).
    return internal::inotify_init1(0);

  // =========================================================================
  // Mass-wire additions. Every `case` below dispatches a Linux x86_64
  // syscall to an engine that's already fully implemented and object-library
  // linked via `.syscall_dispatch_targets`. Kernel convention throughout:
  // value/0 on success, -errno on failure. Argument casts match the Linux
  // x86_64 ABI ordering of the target syscall.
  // =========================================================================

  // --- File I/O: stat family ---------------------------------------------
  case SYS_stat:
    return internal::stat(reinterpret_cast<const char *>(__arg1),
                          reinterpret_cast<struct stat *>(__arg2));
  case SYS_fstat:
    return internal::fstat(static_cast<int>(__arg1),
                           reinterpret_cast<struct stat *>(__arg2));
  case SYS_lstat:
    return internal::lstat(reinterpret_cast<const char *>(__arg1),
                           reinterpret_cast<struct stat *>(__arg2));
  case SYS_newfstatat:
    return internal::fstatat(static_cast<int>(__arg1),
                             reinterpret_cast<const char *>(__arg2),
                             reinterpret_cast<struct stat *>(__arg3),
                             static_cast<int>(__arg4));

  // --- File I/O: positional / vector -------------------------------------
  case SYS_pread64:
    return internal::pread(static_cast<int>(__arg1),
                           reinterpret_cast<void *>(__arg2),
                           static_cast<size_t>(__arg3),
                           static_cast<off_t>(__arg4));
  case SYS_pwrite64:
    return internal::pwrite(static_cast<int>(__arg1),
                            reinterpret_cast<const void *>(__arg2),
                            static_cast<size_t>(__arg3),
                            static_cast<off_t>(__arg4));
  case SYS_readv:
    return static_cast<intptr_t>(internal::readv(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct iovec *>(__arg2),
        static_cast<int>(__arg3)));
  case SYS_writev:
    return static_cast<intptr_t>(internal::writev(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct iovec *>(__arg2),
        static_cast<int>(__arg3)));
  case SYS_preadv:
    return static_cast<intptr_t>(internal::preadv(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct iovec *>(__arg2),
        static_cast<int>(__arg3), static_cast<off_t>(__arg4)));
  case SYS_pwritev:
    return static_cast<intptr_t>(internal::pwritev(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct iovec *>(__arg2),
        static_cast<int>(__arg3), static_cast<off_t>(__arg4)));

  // --- File I/O: fd lifecycle --------------------------------------------
  case SYS_dup3:
    return internal::dup3(static_cast<int>(__arg1), static_cast<int>(__arg2),
                          static_cast<int>(__arg3));
  case SYS_pipe:
    // pipe(fds) == pipe2(fds, 0).
    return internal::pipe2(reinterpret_cast<int *>(__arg1), 0);
  case SYS_pipe2:
    return internal::pipe2(reinterpret_cast<int *>(__arg1),
                           static_cast<int>(__arg2));

  // --- File I/O: fcntl (ErrorOr<int> engine — unpack to kernel conv.) ---
  case SYS_fcntl: {
    auto r = internal::fcntl(static_cast<int>(__arg1),
                             static_cast<int>(__arg2),
                             reinterpret_cast<void *>(__arg3));
    return r.has_value() ? static_cast<intptr_t>(r.value())
                         : -static_cast<intptr_t>(r.error());
  }

  // --- File I/O: sync / flush / truncate ---------------------------------
  case SYS_fsync:
    return internal::fsync(static_cast<int>(__arg1));
  case SYS_fdatasync:
    return internal::fdatasync(static_cast<int>(__arg1));
  case SYS_ftruncate:
    return internal::ftruncate(static_cast<int>(__arg1),
                               static_cast<off_t>(__arg2));
  case SYS_truncate:
    return internal::truncate(reinterpret_cast<const char *>(__arg1),
                              static_cast<off_t>(__arg2));

  // --- Path-based ops ----------------------------------------------------
  case SYS_getcwd:
    return internal::getcwd(reinterpret_cast<char *>(__arg1),
                            static_cast<size_t>(__arg2));
  case SYS_chdir:
    return internal::chdir(reinterpret_cast<const char *>(__arg1));
  case SYS_fchdir:
    return internal::fchdir(static_cast<int>(__arg1));
  case SYS_mkdirat:
    return internal::mkdirat(static_cast<int>(__arg1),
                             reinterpret_cast<const char *>(__arg2),
                             static_cast<mode_t>(__arg3));
  case SYS_unlinkat:
    return internal::unlinkat(static_cast<int>(__arg1),
                              reinterpret_cast<const char *>(__arg2),
                              static_cast<int>(__arg3));
  case SYS_renameat:
    return internal::renameat(static_cast<int>(__arg1),
                              reinterpret_cast<const char *>(__arg2),
                              static_cast<int>(__arg3),
                              reinterpret_cast<const char *>(__arg4));
  case SYS_linkat:
    return internal::linkat(static_cast<int>(__arg1),
                            reinterpret_cast<const char *>(__arg2),
                            static_cast<int>(__arg3),
                            reinterpret_cast<const char *>(__arg4),
                            static_cast<int>(__arg5));
  case SYS_symlinkat:
    return internal::symlinkat(reinterpret_cast<const char *>(__arg1),
                               static_cast<int>(__arg2),
                               reinterpret_cast<const char *>(__arg3));
  case SYS_readlinkat:
    return internal::readlinkat(static_cast<int>(__arg1),
                                reinterpret_cast<const char *>(__arg2),
                                reinterpret_cast<char *>(__arg3),
                                static_cast<size_t>(__arg4));
  case SYS_faccessat:
    return internal::faccessat(static_cast<int>(__arg1),
                               reinterpret_cast<const char *>(__arg2),
                               static_cast<int>(__arg3),
                               static_cast<int>(__arg4));

  // --- Permissions / ownership ------------------------------------------
  case SYS_fchmodat:
    return internal::fchmodat(static_cast<int>(__arg1),
                              reinterpret_cast<const char *>(__arg2),
                              static_cast<mode_t>(__arg3),
                              static_cast<int>(__arg4));
  case SYS_fchownat:
    return internal::fchownat(static_cast<int>(__arg1),
                              reinterpret_cast<const char *>(__arg2),
                              static_cast<uid_t>(__arg3),
                              static_cast<gid_t>(__arg4),
                              static_cast<int>(__arg5));
  case SYS_umask:
    // umask is an atomic exchange in PCB identity. Return the previous
    // value masked to 9 bits, matching the POSIX/Linux contract.
    return static_cast<intptr_t>(g_pcb.identity.umask.exchange(
        static_cast<unsigned>(__arg1) & 0777, cpp::MemoryOrder::RELAXED));

  // --- Timestamps --------------------------------------------------------
  case SYS_utimensat:
    return internal::utimensat(
        static_cast<int>(__arg1), reinterpret_cast<const char *>(__arg2),
        reinterpret_cast<const struct timespec *>(__arg3),
        static_cast<int>(__arg4));

  // --- Cross-fd transfer -------------------------------------------------
  case SYS_sendfile:
    return static_cast<intptr_t>(internal::sendfile(
        static_cast<int>(__arg1), static_cast<int>(__arg2),
        reinterpret_cast<off_t *>(__arg3), static_cast<size_t>(__arg4)));
  case SYS_copy_file_range:
    return internal::copy_file_range(
        static_cast<int>(__arg1), reinterpret_cast<off_t *>(__arg2),
        static_cast<int>(__arg3), reinterpret_cast<off_t *>(__arg4),
        static_cast<size_t>(__arg5), static_cast<unsigned int>(__arg6));

  // --- Memory-backed fds -------------------------------------------------
  case SYS_memfd_create:
    return internal::memfd_create(reinterpret_cast<const char *>(__arg1),
                                  static_cast<unsigned int>(__arg2));

  // --- Memory: address space + advice + locking + policy ----------------
  case SYS_mremap:
    return internal::mremap(reinterpret_cast<void *>(__arg1),
                            static_cast<size_t>(__arg2),
                            static_cast<size_t>(__arg3),
                            static_cast<int>(__arg4),
                            reinterpret_cast<void *>(__arg5));
  case SYS_madvise:
    return internal::madvise(reinterpret_cast<void *>(__arg1),
                             static_cast<size_t>(__arg2),
                             static_cast<int>(__arg3));
  case SYS_mincore:
    return internal::mincore(reinterpret_cast<void *>(__arg1),
                             static_cast<size_t>(__arg2),
                             reinterpret_cast<unsigned char *>(__arg3));
  case SYS_msync:
    return internal::msync(reinterpret_cast<void *>(__arg1),
                           static_cast<size_t>(__arg2),
                           static_cast<int>(__arg3));
  case SYS_mlock:
    return internal::mlock(reinterpret_cast<const void *>(__arg1),
                           static_cast<size_t>(__arg2));
  case SYS_mlock2:
    return internal::mlock2(reinterpret_cast<const void *>(__arg1),
                            static_cast<size_t>(__arg2),
                            static_cast<int>(__arg3));
  case SYS_mlockall:
    return internal::mlockall(static_cast<int>(__arg1));
  case SYS_munlock:
    return internal::munlock(reinterpret_cast<const void *>(__arg1),
                             static_cast<size_t>(__arg2));
  case SYS_munlockall:
    return internal::munlockall();
  case SYS_remap_file_pages:
    return internal::remap_file_pages(reinterpret_cast<void *>(__arg1),
                                      static_cast<size_t>(__arg2),
                                      static_cast<int>(__arg3),
                                      static_cast<size_t>(__arg4),
                                      static_cast<int>(__arg5));
  case SYS_set_mempolicy:
    return internal::set_mempolicy(
        static_cast<int>(__arg1),
        reinterpret_cast<const unsigned long *>(__arg2),
        static_cast<unsigned long>(__arg3));
  case SYS_get_mempolicy:
    return internal::get_mempolicy(
        reinterpret_cast<int *>(__arg1),
        reinterpret_cast<unsigned long *>(__arg2),
        static_cast<unsigned long>(__arg3),
        reinterpret_cast<void *>(__arg4),
        static_cast<unsigned long>(__arg5));
  case SYS_mbind:
    return internal::mbind(reinterpret_cast<void *>(__arg1),
                           static_cast<unsigned long>(__arg2),
                           static_cast<int>(__arg3),
                           reinterpret_cast<const unsigned long *>(__arg4),
                           static_cast<unsigned long>(__arg5),
                           static_cast<unsigned>(__arg6));
  case SYS_pkey_mprotect:
    return internal::pkey_mprotect(reinterpret_cast<void *>(__arg1),
                                   static_cast<size_t>(__arg2),
                                   static_cast<int>(__arg3),
                                   static_cast<int>(__arg4));
  case SYS_pkey_alloc:
    return internal::pkey_alloc(static_cast<unsigned int>(__arg1),
                                static_cast<unsigned int>(__arg2));
  case SYS_pkey_free:
    return internal::pkey_free(static_cast<int>(__arg1));

  // --- SysV shared memory ------------------------------------------------
  case SYS_shmget:
    return internal::shmget(static_cast<key_t>(__arg1),
                            static_cast<size_t>(__arg2),
                            static_cast<int>(__arg3));
  case SYS_shmat:
    return internal::shmat(static_cast<int>(__arg1),
                           reinterpret_cast<const void *>(__arg2),
                           static_cast<int>(__arg3));
  case SYS_shmdt:
    return internal::shmdt(reinterpret_cast<const void *>(__arg1));
  case SYS_shmctl:
    return internal::shmctl(static_cast<int>(__arg1),
                            static_cast<int>(__arg2),
                            reinterpret_cast<struct shmid_ds *>(__arg3));

  // --- Signals: wait / query --------------------------------------------
  case SYS_rt_sigpending:
    // sigsetsize (arg2) ignored — sigset_t is fixed-size on this backend.
    return internal::sigpending(reinterpret_cast<sigset_t *>(__arg1));
  case SYS_rt_sigsuspend:
    // sigsetsize (arg2) ignored.
    return internal::sigsuspend(reinterpret_cast<const sigset_t *>(__arg1));
  case SYS_rt_sigtimedwait:
    // Returns signal number on success (≥0), -errno on failure. sigsetsize
    // (arg4) ignored.
    return internal::sigtimedwait(
        reinterpret_cast<const sigset_t *>(__arg1),
        reinterpret_cast<siginfo_t *>(__arg2),
        reinterpret_cast<const struct timespec *>(__arg3));
  case SYS_rt_sigqueueinfo: {
    // Linux passes a full siginfo_t*; our engine takes sigval only.
    // Forward the sigval payload; si_code is implied SI_QUEUE by the engine.
    const siginfo_t *si = reinterpret_cast<const siginfo_t *>(__arg3);
    union sigval v{};
    if (si)
      v = si->si_value;
    return internal::sigqueue(static_cast<pid_t>(__arg1),
                              static_cast<int>(__arg2), v);
  }
  case SYS_rt_tgsigqueueinfo: {
    // rt_tgsigqueueinfo(tgid, tid, sig, info). All Windows threads share
    // the same PID so tgid is ignored; the engine routes by tid.
    const siginfo_t *si = reinterpret_cast<const siginfo_t *>(__arg4);
    union sigval v{};
    if (si)
      v = si->si_value;
    return internal::sigqueue_thread(static_cast<pid_t>(__arg2),
                                     static_cast<int>(__arg3), v);
  }
  case SYS_tkill:
    // tkill(tid, sig) == tgkill(*, tid, sig). deliver_signal_to_thread
    // already accepts a raw NT TID.
    return signal_state::deliver_signal_to_thread(
        static_cast<DWORD>(__arg1), static_cast<int>(__arg2));

  // --- Process lifecycle / wait -----------------------------------------
  case SYS_fork:
  case SYS_vfork:
    // vfork is permitted to behave identically to fork (POSIX + Linux
    // historical guidance). The naked-asm engine does the clone and
    // returns pid/0/-errno in kernel convention.
    return internal::sys_fork();

  case SYS_execveat:
    // execveat never returns on success; engine returns -errno on failure.
    return internal::execveat(static_cast<int>(__arg1),
                              reinterpret_cast<const char *>(__arg2),
                              reinterpret_cast<char *const *>(__arg3),
                              reinterpret_cast<char *const *>(__arg4),
                              static_cast<int>(__arg5));

  case SYS_execve:
    // execve never returns on success; engine returns -errno on failure.
    return internal::execve(reinterpret_cast<const char *>(__arg1),
                            reinterpret_cast<char *const *>(__arg2),
                            reinterpret_cast<char *const *>(__arg3));
  case SYS_wait4:
    return internal::wait4(static_cast<pid_t>(__arg1),
                           reinterpret_cast<int *>(__arg2),
                           static_cast<int>(__arg3),
                           reinterpret_cast<struct rusage *>(__arg4));
  case SYS_waitid:
    return internal::waitid(static_cast<idtype_t>(__arg1),
                            static_cast<id_t>(__arg2),
                            reinterpret_cast<siginfo_t *>(__arg3),
                            static_cast<int>(__arg4));

  // --- Identity / credentials -------------------------------------------
  case SYS_setuid:
    return internal::setuid(static_cast<uid_t>(__arg1));
  case SYS_setgid:
    return internal::setgid(static_cast<gid_t>(__arg1));
  case SYS_setreuid:
    return internal::setreuid(static_cast<uid_t>(__arg1),
                              static_cast<uid_t>(__arg2));
  case SYS_setregid:
    return internal::setregid(static_cast<gid_t>(__arg1),
                              static_cast<gid_t>(__arg2));
  case SYS_setresuid:
    return internal::setresuid(static_cast<uid_t>(__arg1),
                               static_cast<uid_t>(__arg2),
                               static_cast<uid_t>(__arg3));
  case SYS_setresgid:
    return internal::setresgid(static_cast<gid_t>(__arg1),
                               static_cast<gid_t>(__arg2),
                               static_cast<gid_t>(__arg3));
  case SYS_getresuid:
    return internal::getresuid(reinterpret_cast<uid_t *>(__arg1),
                               reinterpret_cast<uid_t *>(__arg2),
                               reinterpret_cast<uid_t *>(__arg3));
  case SYS_getresgid:
    return internal::getresgid(reinterpret_cast<gid_t *>(__arg1),
                               reinterpret_cast<gid_t *>(__arg2),
                               reinterpret_cast<gid_t *>(__arg3));
  case SYS_getgroups:
    return internal::getgroups(static_cast<int>(__arg1),
                               reinterpret_cast<gid_t *>(__arg2));

  // --- Resource limits / accounting -------------------------------------
  case SYS_getrlimit:
    return internal::getrlimit(static_cast<int>(__arg1),
                               reinterpret_cast<struct rlimit *>(__arg2));
  case SYS_setrlimit:
    return internal::setrlimit(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct rlimit *>(__arg2));
  case SYS_prlimit64:
    return internal::prlimit(
        static_cast<int>(__arg1), static_cast<int>(__arg2),
        reinterpret_cast<const struct rlimit *>(__arg3),
        reinterpret_cast<struct rlimit *>(__arg4));
  case SYS_getrusage:
    return internal::getrusage(static_cast<int>(__arg1),
                               reinterpret_cast<struct rusage *>(__arg2));

  // --- Random / CPU -----------------------------------------------------
  case SYS_getrandom: {
    // Inline ProcessPrng — matches windows_syscalls::getrandom semantics.
    constexpr unsigned int VALID_FLAGS = 0x0007; // RANDOM|NONBLOCK|INSECURE
    unsigned int flags = static_cast<unsigned int>(__arg3);
    void *buf = reinterpret_cast<void *>(__arg1);
    size_t buflen = static_cast<size_t>(__arg2);
    if (flags & ~VALID_FLAGS)
      return -static_cast<intptr_t>(EINVAL);
    if (buf == nullptr && buflen > 0)
      return -static_cast<intptr_t>(EFAULT);
    if (buflen == 0)
      return 0;
    ::ProcessPrng(static_cast<unsigned char *>(buf), buflen);
    return static_cast<intptr_t>(buflen);
  }
  case SYS_getcpu:
    // tcache (arg3) is ignored on Linux since 2.6.24; we do the same.
    return internal::getcpu(reinterpret_cast<unsigned int *>(__arg1),
                            reinterpret_cast<unsigned int *>(__arg2));

  // --- Sockets: lifecycle -----------------------------------------------
  case SYS_socket:
    return internal::socket(static_cast<int>(__arg1),
                            static_cast<int>(__arg2),
                            static_cast<int>(__arg3));
  case SYS_bind:
    return internal::bind(static_cast<int>(__arg1),
                          reinterpret_cast<const struct sockaddr *>(__arg2),
                          static_cast<socklen_t>(__arg3));
  case SYS_listen:
    return internal::listen(static_cast<int>(__arg1),
                            static_cast<int>(__arg2));
  case SYS_connect:
    return internal::connect(static_cast<int>(__arg1),
                             reinterpret_cast<const struct sockaddr *>(__arg2),
                             static_cast<socklen_t>(__arg3));
  case SYS_accept:
    return internal::accept(static_cast<int>(__arg1),
                            reinterpret_cast<struct sockaddr *>(__arg2),
                            reinterpret_cast<socklen_t *>(__arg3));
  case SYS_shutdown:
    return internal::shutdown(static_cast<int>(__arg1),
                              static_cast<int>(__arg2));
  case SYS_socketpair:
    return internal::socketpair(static_cast<int>(__arg1),
                                static_cast<int>(__arg2),
                                static_cast<int>(__arg3),
                                reinterpret_cast<int *>(__arg4));

  // --- Sockets: data transfer -------------------------------------------
  case SYS_sendto:
    return static_cast<intptr_t>(internal::sendto(
        static_cast<int>(__arg1), reinterpret_cast<const void *>(__arg2),
        static_cast<size_t>(__arg3), static_cast<int>(__arg4),
        reinterpret_cast<const struct sockaddr *>(__arg5),
        static_cast<socklen_t>(__arg6)));
  case SYS_recvfrom:
    return static_cast<intptr_t>(internal::recvfrom(
        static_cast<int>(__arg1), reinterpret_cast<void *>(__arg2),
        static_cast<size_t>(__arg3), static_cast<int>(__arg4),
        reinterpret_cast<struct sockaddr *>(__arg5),
        reinterpret_cast<socklen_t *>(__arg6)));
  case SYS_sendmsg:
    return static_cast<intptr_t>(internal::sendmsg(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct msghdr *>(__arg2),
        static_cast<int>(__arg3)));
  case SYS_recvmsg:
    return static_cast<intptr_t>(internal::recvmsg(
        static_cast<int>(__arg1), reinterpret_cast<struct msghdr *>(__arg2),
        static_cast<int>(__arg3)));

  // --- Sockets: query ---------------------------------------------------
  case SYS_getsockname:
    return internal::getsockname(
        static_cast<int>(__arg1),
        reinterpret_cast<struct sockaddr *>(__arg2),
        reinterpret_cast<socklen_t *>(__arg3));
  case SYS_getpeername:
    return internal::getpeername(
        static_cast<int>(__arg1),
        reinterpret_cast<struct sockaddr *>(__arg2),
        reinterpret_cast<socklen_t *>(__arg3));
  case SYS_setsockopt:
    return internal::setsockopt(static_cast<int>(__arg1),
                                static_cast<int>(__arg2),
                                static_cast<int>(__arg3),
                                reinterpret_cast<const void *>(__arg4),
                                static_cast<socklen_t>(__arg5));
  case SYS_getsockopt:
    return internal::getsockopt(static_cast<int>(__arg1),
                                static_cast<int>(__arg2),
                                static_cast<int>(__arg3),
                                reinterpret_cast<void *>(__arg4),
                                reinterpret_cast<socklen_t *>(__arg5));

  // --- Multiplexing -----------------------------------------------------
  case SYS_poll:
    return internal::poll(reinterpret_cast<struct pollfd *>(__arg1),
                          static_cast<nfds_t>(__arg2),
                          static_cast<int>(__arg3));
  case SYS_ppoll:
    // 5th arg (sigsetsize) ignored — sigset_t is fixed-size here.
    return internal::ppoll(reinterpret_cast<struct pollfd *>(__arg1),
                           static_cast<nfds_t>(__arg2),
                           reinterpret_cast<const struct timespec *>(__arg3),
                           reinterpret_cast<const sigset_t *>(__arg4));
  case SYS_select:
    return internal::select(static_cast<int>(__arg1),
                            reinterpret_cast<fd_set *>(__arg2),
                            reinterpret_cast<fd_set *>(__arg3),
                            reinterpret_cast<fd_set *>(__arg4),
                            reinterpret_cast<struct timeval *>(__arg5));
  case SYS_pselect6: {
    // Linux packs sigmask as `{const sigset_t *, size_t}` in arg6; extract
    // the pointer and ignore the size (fixed sigset_t).
    const sigset_t *sigmask = nullptr;
    if (__arg6) {
      struct LinuxSigmaskArg {
        const sigset_t *ss;
        size_t ss_len;
      };
      sigmask =
          reinterpret_cast<const LinuxSigmaskArg *>(__arg6)->ss;
    }
    return internal::pselect(
        static_cast<int>(__arg1), reinterpret_cast<fd_set *>(__arg2),
        reinterpret_cast<fd_set *>(__arg3),
        reinterpret_cast<fd_set *>(__arg4),
        reinterpret_cast<const struct timespec *>(__arg5), sigmask);
  }
  case SYS_epoll_create:
    // Since 2.6.8 the `size` hint is ignored; == epoll_create1(0).
    return internal::epoll_create1(0);
  case SYS_epoll_create1:
    return internal::epoll_create1(static_cast<int>(__arg1));
  case SYS_epoll_ctl:
    return internal::epoll_ctl(static_cast<int>(__arg1),
                               static_cast<int>(__arg2),
                               static_cast<int>(__arg3),
                               reinterpret_cast<struct epoll_event *>(__arg4));
  case SYS_epoll_wait:
    return internal::epoll_wait(static_cast<int>(__arg1),
                                reinterpret_cast<struct epoll_event *>(__arg2),
                                static_cast<int>(__arg3),
                                static_cast<int>(__arg4));

  // --- SysV semaphores --------------------------------------------------
  case SYS_semget:
    return internal::semget(static_cast<key_t>(__arg1),
                            static_cast<int>(__arg2),
                            static_cast<int>(__arg3));
  case SYS_semop:
    return internal::semop(static_cast<int>(__arg1),
                           reinterpret_cast<struct sembuf *>(__arg2),
                           static_cast<size_t>(__arg3));
  case SYS_semctl:
    return internal::semctl(static_cast<int>(__arg1),
                            static_cast<int>(__arg2),
                            static_cast<int>(__arg3), __arg4);

  // --- Time -------------------------------------------------------------
  case SYS_gettimeofday: {
    // gettimeofday(tv, tz). tz is legacy/unused. Read CLOCK_REALTIME and
    // convert to timeval. Matches windows_syscalls::gettimeofday.
    struct timeval *tv = reinterpret_cast<struct timeval *>(__arg1);
    if (tv == nullptr)
      return 0;
    timespec ts;
    auto r = internal::clock_gettime(CLOCK_REALTIME, &ts);
    if (!r.has_value())
      return -static_cast<intptr_t>(r.error());
    // Reach into struct timeval fields by name — assume tv_sec/tv_usec.
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = static_cast<decltype(tv->tv_usec)>(ts.tv_nsec / 1000);
    return 0;
  }
  case SYS_clock_settime: {
    auto r = internal::clock_settime(
        static_cast<clockid_t>(__arg1),
        reinterpret_cast<const timespec *>(__arg2));
    return r.has_value() ? intptr_t{0}
                         : -static_cast<intptr_t>(r.error());
  }
  case SYS_clock_getres:
    return internal::clock_getres(
        static_cast<clockid_t>(__arg1),
        reinterpret_cast<struct timespec *>(__arg2));

  // --- POSIX timers -----------------------------------------------------
  case SYS_timer_create:
    return internal::timer_create(
        static_cast<clockid_t>(__arg1),
        reinterpret_cast<struct sigevent *>(__arg2),
        reinterpret_cast<timer_t *>(__arg3));
  case SYS_timer_settime:
    return internal::timer_settime(
        reinterpret_cast<timer_t>(__arg1), static_cast<int>(__arg2),
        reinterpret_cast<const struct itimerspec *>(__arg3),
        reinterpret_cast<struct itimerspec *>(__arg4));
  case SYS_timer_gettime:
    return internal::timer_gettime(
        reinterpret_cast<timer_t>(__arg1),
        reinterpret_cast<struct itimerspec *>(__arg2));
  case SYS_timer_getoverrun:
    return internal::timer_getoverrun(reinterpret_cast<timer_t>(__arg1));
  case SYS_timer_delete:
    return internal::timer_delete(reinterpret_cast<timer_t>(__arg1));

  // --- Interval timers --------------------------------------------------
  case SYS_setitimer:
    return internal::setitimer(
        static_cast<int>(__arg1),
        reinterpret_cast<const struct itimerval *>(__arg2),
        reinterpret_cast<struct itimerval *>(__arg3));
  case SYS_getitimer:
    return internal::getitimer(
        static_cast<int>(__arg1),
        reinterpret_cast<struct itimerval *>(__arg2));

  // --- Scheduling -------------------------------------------------------
  case SYS_sched_setscheduler:
    return internal::sched_setscheduler(
        static_cast<pid_t>(__arg1), static_cast<int>(__arg2),
        reinterpret_cast<const struct sched_param *>(__arg3));
  case SYS_sched_getscheduler:
    return internal::sched_getscheduler(static_cast<pid_t>(__arg1));
  case SYS_sched_setparam:
    return internal::sched_setparam(
        static_cast<pid_t>(__arg1),
        reinterpret_cast<const struct sched_param *>(__arg2));
  case SYS_sched_getparam:
    return internal::sched_getparam(
        static_cast<pid_t>(__arg1),
        reinterpret_cast<struct sched_param *>(__arg2));
  case SYS_sched_get_priority_max:
    return internal::sched_get_priority_max(static_cast<int>(__arg1));
  case SYS_sched_get_priority_min:
    return internal::sched_get_priority_min(static_cast<int>(__arg1));
  case SYS_sched_rr_get_interval:
    return internal::sched_rr_get_interval(
        static_cast<pid_t>(__arg1),
        reinterpret_cast<struct timespec *>(__arg2));
  case SYS_sched_setaffinity:
    return internal::sched_setaffinity(
        static_cast<pid_t>(__arg1), static_cast<size_t>(__arg2),
        reinterpret_cast<const cpu_set_t *>(__arg3));
  case SYS_sched_getaffinity:
    return internal::sched_getaffinity(
        static_cast<pid_t>(__arg1), static_cast<size_t>(__arg2),
        reinterpret_cast<cpu_set_t *>(__arg3));

  // --- Futex (FUTEX_WAIT / FUTEX_WAKE only; other ops return ENOSYS) ---
  case SYS_futex: {
    constexpr int FUTEX_WAIT = 0;
    constexpr int FUTEX_WAKE = 1;
    constexpr int FUTEX_PRIVATE_FLAG = 128;
    int op = static_cast<int>(__arg2) & ~FUTEX_PRIVATE_FLAG;
    volatile uint32_t *uaddr =
        reinterpret_cast<volatile uint32_t *>(__arg1);
    uint32_t val = static_cast<uint32_t>(__arg3);
    const struct timespec *to =
        reinterpret_cast<const struct timespec *>(__arg4);
    if (op == FUTEX_WAIT) {
      if (to && (to->tv_nsec < 0 || to->tv_nsec >= 1000000000L ||
                 to->tv_sec < 0))
        return -static_cast<intptr_t>(EINVAL);
      return futex_addr::wait(uaddr, val, to);
    }
    if (op == FUTEX_WAKE)
      return static_cast<intptr_t>(futex_addr::wake(uaddr, val));
    return -static_cast<intptr_t>(ENOSYS);
  }

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
