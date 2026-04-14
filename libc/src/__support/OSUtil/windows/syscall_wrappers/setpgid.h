//===-- windows_syscalls::setpgid/getpgid/getpgrp wrapper --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX process group management.
//
// Process groups are tracked as metadata in the child table — no NT kernel
// objects (job objects) are used. This avoids the fundamental POSIX vs NT
// mismatch where NT jobs are permanent (a process cannot be removed from a
// job), while POSIX pgids are freely mutable via setpgid.
//
// The child table's per-group intrusive chain provides O(k) enumeration
// for waitpid(-pgid) and killpg. The process-global self_pgid atomic
// tracks the calling process's own group membership.
//
// Scope:
//   - setpgid/getpgid on self and direct children (full POSIX compliance)
//   - getpgid on arbitrary PIDs returns EPERM (requires future ALPC or
//     shared-memory extension for cross-process queries)
//
// TODO(ntposix): Cross-process pgid queries (getpgid for arbitrary PIDs)
// can be implemented via one of two mechanisms:
//   Option B — ALPC query: Add a QUERY_PGID message type to the existing
//     signal ALPC transport. Target process receives the query, reads its
//     own self_pgid, and responds. Same three-tier security model. Adds
//     round-trip latency (~20µs) but full POSIX compliance.
//   Option C — Shared-memory state block: Extend the existing per-child
//     ChildStateBlock (anonymous section mapped between parent and child)
//     to include the child's current pgid. Child atomically updates it on
//     setpgid(0, ...). Parent reads directly — zero IPC latency. Already
//     secure: section is mapped only between parent and child.
// Either extension is additive — no design changes needed to the metadata
// model. Option C is preferred for latency; Option B for generality.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_SETPGID_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_SETPGID_H

#include "hdr/errno_macros.h"
#include "hdr/types/pid_t.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/syscall_wrappers/getsid.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/process/windows/child_table.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_syscalls {

// -------------------------------------------------------------------------
// setpgid — set process group ID (POSIX XSH setpgid)
//
// POSIX rules enforced:
//   - pid == 0 means the calling process
//   - pgid == 0 means use the target's PID as the new pgid
//   - Can only target self or a direct child
//   - Child must not have exec'd (EACCES)
//   - Target group must already exist OR pgid == target_pid (EPERM)
//   - Cannot change a session leader's pgid (EPERM)
//   - Target must be in the same session as the caller (EPERM)
// -------------------------------------------------------------------------
LIBC_INLINE ErrorOr<int> setpgid(pid_t pid, pid_t pgid) {
  pid_t self = static_cast<pid_t>(NtCurrentProcessId());

  // Normalize: pid==0 → self, pgid==0 → target's pid.
  pid_t target_pid = (pid == 0) ? self : pid;
  pid_t new_pgid = (pgid == 0) ? target_pid : pgid;

  // Validate: new_pgid must be positive.
  if (new_pgid < 0)
    return Error(EINVAL);

  // --- Self case ---
  if (target_pid == self) {
    // Cannot change a session leader's pgid.
    pid_t sid = get_session_id();
    if (sid == self)
      return Error(EPERM);

    // Creating a new group (pgid == self) is always allowed.
    // Joining an existing group requires that group to exist in our
    // child table. For self-targeted setpgid, we also accept the case
    // where the group leader is the caller itself (new group creation).
    if (new_pgid != self) {
      g_pcb.child_table.lock.lock();
      bool group_exists = process::has_process_group(new_pgid);
      g_pcb.child_table.lock.unlock();
      if (!group_exists)
        return Error(EPERM);
    }

    process::set_self_pgid(new_pgid);
    return 0;
  }

  // --- Child case ---
  g_pcb.child_table.lock.lock();

  process::ChildEntry *entry = process::find_child(target_pid);
  if (!entry) {
    g_pcb.child_table.lock.unlock();
    return Error(ESRCH);
  }

  // POSIX: setpgid on a child is only valid before the child calls exec.
  // Determined via the shared ChildStateBlock::exec_count field — the child
  // atomically increments it on exec, and has_execd() reads it with ACQUIRE
  // ordering. For posix_spawn children, exec_count is set to 1 at child
  // startup (init_inherited_child_state). For future fork children, it
  // starts at 0 and increments in the execve engine.
  if (entry->has_execd()) {
    g_pcb.child_table.lock.unlock();
    return Error(EACCES);
  }

  // POSIX: cannot move a child to a different session's group.
  // Since we don't track per-child session IDs and all our children
  // are in the same session (posix_spawn inherits session), this check
  // is satisfied by construction. If cross-session spawn is added later,
  // a per-entry session ID would be needed here.

  // change_pgid validates that the target group exists (or is a new group).
  int err = process::change_child_pgid(entry, new_pgid);
  g_pcb.child_table.lock.unlock();

  if (err)
    return Error(err);
  return 0;
}

// -------------------------------------------------------------------------
// getpgid — get process group ID (POSIX XSH getpgid)
//
// Works for:
//   - pid == 0 or pid == self: returns self_pgid
//   - pid == direct child: returns child's pgid from child table
//   - pid == arbitrary other: EPERM (cross-process query not yet supported)
// -------------------------------------------------------------------------
LIBC_INLINE ErrorOr<pid_t> getpgid(pid_t pid) {
  pid_t self = static_cast<pid_t>(NtCurrentProcessId());

  // Self query.
  if (pid == 0 || pid == self)
    return process::get_self_pgid();

  // Child query — look up in child table.
  g_pcb.child_table.lock.lock();
  process::ChildEntry *entry = process::find_child(pid);
  if (entry) {
    pid_t result = entry->pgid;
    g_pcb.child_table.lock.unlock();
    return result;
  }
  g_pcb.child_table.lock.unlock();

  // TODO(ntposix): Cross-process pgid query via ALPC (Option B) or
  // shared-memory state block (Option C). See file header for design.
  return Error(EPERM);
}

// -------------------------------------------------------------------------
// getpgrp — get process group ID of calling process (POSIX XSH getpgrp)
// Equivalent to getpgid(0).
// -------------------------------------------------------------------------
LIBC_INLINE pid_t getpgrp() { return process::get_self_pgid(); }

// -------------------------------------------------------------------------
// setpgrp — set process group ID (POSIX XSH setpgrp)
// Equivalent to setpgid(0, 0).
// -------------------------------------------------------------------------
LIBC_INLINE ErrorOr<int> setpgrp() { return setpgid(0, 0); }

} // namespace windows_syscalls
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SYSCALL_WRAPPERS_SETPGID_H
