//===-- Windows wait family engine implementation -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine functions for the POSIX wait family: waitid, waitpid, wait4.
// All return 0/-errno (waitid) or positive-pid/-errno (waitpid/wait4).
// No libc_errno usage — callers convert to errno at the entry-point layer.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/process/wait_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/types/struct_rusage.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/process/windows/child_table.h"
#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/process/wait_utils.h"

#include "hdr/signal_macros.h"
#include "hdr/sys_wait_macros.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

namespace {

enum class WaitEventKind {
  EXITED,
  STOPPED,
  CONTINUED,
};

void zero_outputs(siginfo_t *infop, struct rusage *usage) {
  __builtin_memset(infop, 0, sizeof(*infop));
  if (usage != nullptr)
    __builtin_memset(usage, 0, sizeof(*usage));
}

bool matches_selector(const process::ChildEntry &entry, idtype_t idtype,
                      id_t id) {
  switch (idtype) {
  case P_ALL:
    return true;
  case P_PID:
    return static_cast<id_t>(entry.pid) == id;
  case P_PGID:
    return static_cast<id_t>(entry.pgid) == id;
  default:
    return false;
  }
}

bool has_matching_child(process::ChildTableState &table, idtype_t idtype,
                        id_t id) {
  for (process::ChildEntry *entry = table.active; entry; entry = entry->next) {
    if (matches_selector(*entry, idtype, id))
      return true;
  }
  return false;
}

process::ChildEntry *find_ready_child(process::ChildTableState &table,
                                      idtype_t idtype, id_t id, int options,
                                      WaitEventKind &kind) {
  if (options & WEXITED) {
    for (process::ChildEntry *entry = table.active; entry; entry = entry->next) {
      if (matches_selector(*entry, idtype, id) && entry->exited.load()) {
        kind = WaitEventKind::EXITED;
        return entry;
      }
    }
  }

  if (options & WSTOPPED) {
    for (process::ChildEntry *entry = table.active; entry; entry = entry->next) {
      if (!matches_selector(*entry, idtype, id))
        continue;
      if (entry->child_state.load() > 0) {
        kind = WaitEventKind::STOPPED;
        return entry;
      }
    }
  }

  if (options & WCONTINUED) {
    for (process::ChildEntry *entry = table.active; entry; entry = entry->next) {
      if (!matches_selector(*entry, idtype, id))
        continue;
      if (entry->child_state.load() == signal_state::CHILD_STATE_CONTINUED) {
        kind = WaitEventKind::CONTINUED;
        return entry;
      }
    }
  }

  return nullptr;
}

void fill_siginfo_from_entry(process::ChildEntry &entry, WaitEventKind kind,
                             siginfo_t &info) {
  __builtin_memset(&info, 0, sizeof(info));
  info.si_signo = SIGCHLD;
  info.si_pid = static_cast<pid_t>(entry.pid);

  switch (kind) {
  case WaitEventKind::EXITED: {
    int status = entry.wait_status.load();
    if (WIFEXITED(status)) {
      info.si_code = CLD_EXITED;
      info.si_status = WEXITSTATUS(status);
    } else if (WCOREDUMP(status)) {
      info.si_code = CLD_DUMPED;
      info.si_status = WTERMSIG(status);
    } else {
      info.si_code = CLD_KILLED;
      info.si_status = WTERMSIG(status);
    }
    break;
  }
  case WaitEventKind::STOPPED: {
    int state = entry.child_state.load();
    info.si_code = CLD_STOPPED;
    info.si_status = state > 0 ? state : 0;
    break;
  }
  case WaitEventKind::CONTINUED:
    info.si_code = CLD_CONTINUED;
    info.si_status = SIGCONT;
    break;
  }
}

/// Internal waitid core — shared by waitid(), waitpid(), and wait4().
/// Returns 0 on success, -errno on failure.
intptr_t waitid_core(idtype_t idtype, id_t id, siginfo_t *infop, int options,
                 struct rusage *usage) {
  if (infop == nullptr)
    return -EINVAL;

  int option_error = validate_waitid_options(options);
  if (option_error != 0)
    return -option_error;

  switch (idtype) {
  case P_ALL:
  case P_PID:
  case P_PGID:
    break;
  default:
    return -EINVAL;
  }

  auto &table = g_pcb.child_table;

  for (;;) {
    ::NtResetEvent(table.any_child_event, nullptr);

    table.lock.lock();

    if (!has_matching_child(table, idtype, id)) {
      table.lock.unlock();
      return -ECHILD;
    }

    WaitEventKind kind = WaitEventKind::EXITED;
    process::ChildEntry *entry = find_ready_child(table, idtype, id, options, kind);
    if (entry != nullptr) {
      siginfo_t info = {};
      fill_siginfo_from_entry(*entry, kind, info);

      if (usage != nullptr) {
        __builtin_memset(usage, 0, sizeof(*usage));
        if (kind == WaitEventKind::EXITED)
          *usage = entry->exit_rusage;
      }

      if (!(options & WNOWAIT)) {
        if (kind == WaitEventKind::EXITED) {
          // Two-phase reap: unlink under lock, then finalize without lock.
          // finalize_reap calls reactor::unwatch() which may spin-wait on
          // the drain thread's dispatching flag — holding table.lock during
          // that spin would deadlock if a state callback is in-flight.
          auto tokens = process::unlink_child_for_reap(entry);
          table.lock.unlock();
          process::finalize_child_reap(entry, tokens);
          *infop = info;
          return 0;
        } else {
          entry->child_state.store(signal_state::CHILD_STATE_RUNNING);
        }
      }

      table.lock.unlock();
      *infop = info;
      return 0;
    }

    table.lock.unlock();

    if (options & WNOHANG) {
      zero_outputs(infop, usage);
      return 0;
    }

    if (idtype == P_PID && (options & (WSTOPPED | WCONTINUED)) == 0) {
      // Per-child targeted wait via exit_futex. No handle limits, no
      // handle lifetime issues — the futex parks on NtWaitForAlertByThreadId
      // and the reactor wakes via NtAlertMultipleThreadByThreadId.
      process::ChildEntry *specific = nullptr;

      table.lock.lock();
      specific = process::find_child(static_cast<pid_t>(id));
      if (specific != nullptr)
        specific->wait_refs.fetch_add(1, cpp::MemoryOrder::RELAXED);
      table.lock.unlock();

      if (specific != nullptr) {
        long wait_ret = specific->exit_futex.wait<true>(0);
        specific->wait_refs.fetch_sub(1, cpp::MemoryOrder::RELEASE);

        if (wait_ret == -EINTR &&
            !signal_state::should_restart_syscall()) {
          return -EINTR;
        }
        // Fatal wait failure (e.g. -ENOMEM on wait-slot pool
        // exhaustion). Looping would re-fail identically and burn
        // a core; surface the errno to the caller so waitid() fails
        // cleanly rather than hanging.
        if (wait_ret < 0 && wait_ret != -EINTR && wait_ret != -ETIMEDOUT)
          return wait_ret;
      } else {
        // Entry disappeared between has_matching_child and here
        // (SA_NOCLDWAIT auto-reap). Next iteration will return ECHILD.
      }
    } else {
      NTSTATUS wait_status =
          ::NtWaitForSingleObject(table.any_child_event, TRUE, nullptr);

      if (wait_status == STATUS_USER_APC || wait_status == STATUS_ALERTED) {
        if (wait_status == STATUS_USER_APC &&
            !signal_state::should_restart_syscall()) {
          return -EINTR;
        }
      }
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Public engine functions
// ---------------------------------------------------------------------------

intptr_t waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options) {
  return waitid_core(idtype, id, infop, options, nullptr);
}

intptr_t waitpid(pid_t pid, int *wstatus, int options) {
  idtype_t idtype;
  id_t id;
  int selector_error = waitpid_pid_to_waitid_selector(pid, idtype, id);
  if (selector_error != 0)
    return -selector_error;

  int waitid_options = 0;
  int option_error = waitpid_options_to_waitid_options(options, waitid_options);
  if (option_error != 0)
    return -option_error;

  siginfo_t info = {};
  intptr_t ret = waitid_core(idtype, id, &info, waitid_options, nullptr);
  if (ret < 0)
    return ret;

  // WNOHANG with no child ready: siginfo zeroed, return 0.
  if (info.si_pid == 0)
    return 0;

  if (wstatus != nullptr)
    *wstatus = siginfo_to_waitstatus(info);
  return static_cast<intptr_t>(info.si_pid);
}

intptr_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage) {
  idtype_t idtype;
  id_t id;
  int selector_error = waitpid_pid_to_waitid_selector(pid, idtype, id);
  if (selector_error != 0)
    return -selector_error;

  int waitid_options = 0;
  int option_error = waitpid_options_to_waitid_options(options, waitid_options);
  if (option_error != 0)
    return -option_error;

  siginfo_t info = {};
  struct rusage local_usage = {};
  intptr_t ret = waitid_core(idtype, id, &info, waitid_options, &local_usage);
  if (ret < 0)
    return ret;

  if (info.si_pid == 0) {
    if (rusage != nullptr)
      *rusage = local_usage;
    return 0;
  }

  if (wstatus != nullptr)
    *wstatus = siginfo_to_waitstatus(info);
  // For exit notifications this is the waited child's final usage snapshot.
  // For stop/continue notifications the child table does not currently track a
  // POSIX-style interim rusage, so the snapshot remains zeroed.
  if (rusage != nullptr)
    *rusage = local_usage;
  return static_cast<intptr_t>(info.si_pid);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
