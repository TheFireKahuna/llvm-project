//===-- Windows getpriority/setpriority engine -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX getpriority()/setpriority() on NT kernel primitives.
//
// Nice values (-20..19) map to NT process priority classes (5 non-realtime
// levels). The mapping is intentionally coarse — POSIX permits this. The
// canonical nice values for each class are consistent with the RLIMIT_NICE
// mapping in resource_ops.cpp.
//
// PRIO_PGRP and PRIO_USER use NtGetNextProcess for system-wide enumeration.
// Processes that deny the requested access are silently skipped, matching
// Linux behavior when /proc/<pid> is unreadable.
//
//===----------------------------------------------------------------------===//

#include "priority_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/types/id_t.h"
#include "hdr/types/uid_t.h"
#include "include/llvm-libc-macros/sys-resource-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process/sid_utils.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// ============================================================================
// Nice ↔ NT priority class mapping
// ============================================================================
//
// NT has 5 non-realtime process priority classes. We map the 40-value nice
// range into 5 bins. The reverse mapping returns canonical nice values that
// are consistent with the RLIMIT_NICE table in resource_ops.cpp:
//
//   Class          Nice     RLIMIT_NICE (20 - nice)
//   REALTIME       -20      40
//   HIGH           -20      40
//   ABOVE_NORMAL    -5      25
//   NORMAL           0      20
//   BELOW_NORMAL     5      15
//   IDLE            15       5
//
// Round-trip fidelity: setpriority(canonical) → getpriority == canonical.
// Non-canonical values quantize to the bin's canonical value.

namespace {

UCHAR nice_to_priority_class(int nice) {
  if (nice <= -10)
    return PROCESS_PRIORITY_CLASS_HIGH;
  if (nice < 0)
    return PROCESS_PRIORITY_CLASS_ABOVE_NORMAL;
  if (nice == 0)
    return PROCESS_PRIORITY_CLASS_NORMAL;
  if (nice <= 10)
    return PROCESS_PRIORITY_CLASS_BELOW_NORMAL;
  return PROCESS_PRIORITY_CLASS_IDLE;
}

int priority_class_to_nice(UCHAR pclass) {
  switch (pclass) {
  case PROCESS_PRIORITY_CLASS_REALTIME:
    return -20;
  case PROCESS_PRIORITY_CLASS_HIGH:
    return -20;
  case PROCESS_PRIORITY_CLASS_ABOVE_NORMAL:
    return -5;
  case PROCESS_PRIORITY_CLASS_NORMAL:
    return 0;
  case PROCESS_PRIORITY_CLASS_BELOW_NORMAL:
    return 5;
  case PROCESS_PRIORITY_CLASS_IDLE:
    return 15;
  default:
    return 0;
  }
}

// ============================================================================
// Per-process queries
// ============================================================================

// Sentinel: no valid nice value equals this.
inline constexpr int NICE_QUERY_FAILED = -128;

// Query nice value of a process. Returns NICE_QUERY_FAILED on error.
int query_process_nice(HANDLE process) {
  PROCESS_PRIORITY_CLASS ppc = {};
  NTSTATUS st = ::NtQueryInformationProcess(process, ProcessPriorityClass, &ppc,
                                            sizeof(ppc), nullptr);
  if (!NT_SUCCESS(st))
    return NICE_QUERY_FAILED;
  return priority_class_to_nice(ppc.PriorityClass);
}

// Set nice value on a process. Returns 0 or -errno.
int set_process_nice(HANDLE process, int nice) {
  PROCESS_PRIORITY_CLASS ppc = {};
  ppc.Foreground = 0;
  ppc.PriorityClass = nice_to_priority_class(nice);
  NTSTATUS st = ::NtSetInformationProcess(process, ProcessPriorityClass, &ppc,
                                          sizeof(ppc));
  if (!NT_SUCCESS(st))
    return -EPERM;
  return 0;
}

// ============================================================================
// Process identity helpers for PRIO_USER and PRIO_PGRP
// ============================================================================

// Get the effective UID of a process via its token.
// Returns (uid_t)-1 on failure (access denied, zombie, etc.).
uid_t query_process_uid(HANDLE process) {
  windows::ScopedNtHandle token;
  NTSTATUS st = ::NtOpenProcessTokenEx(process, TOKEN_QUERY, 0, token.put());
  if (!NT_SUCCESS(st))
    return static_cast<uid_t>(-1);

  alignas(8) UCHAR buf[128];
  ULONG needed = 0;
  st = ::NtQueryInformationToken(token.get(), TokenUser, buf, sizeof(buf), &needed);
  if (!NT_SUCCESS(st))
    return static_cast<uid_t>(-1);

  auto *tu = reinterpret_cast<TOKEN_USER *>(buf);
  return sid_to_uid(reinterpret_cast<const SID *>(tu->User.Sid));
}

// Read the ProcessGroupId from a remote process's RTL_USER_PROCESS_PARAMETERS.
// Requires PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ on the handle.
// Returns (id_t)-1 on failure.
id_t query_process_pgid(HANDLE process) {
  // Step 1: get the remote PEB address.
  PROCESS_BASIC_INFORMATION pbi;
  NTSTATUS st = ::NtQueryInformationProcess(
      process, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
  if (!NT_SUCCESS(st) || !pbi.PebBaseAddress)
    return static_cast<id_t>(-1);

  // Step 2: read the ProcessParameters pointer from the remote PEB.
  // PEB::ProcessParameters is at offset 0x20 (x64), verified by static_assert
  // in nt_peb.h.
  auto *remote_peb = reinterpret_cast<PEB *>(pbi.PebBaseAddress);
  PRTL_USER_PROCESS_PARAMETERS params = nullptr;
  SIZE_T bytes_read = 0;
  st = ::NtReadVirtualMemoryEx(process, &remote_peb->ProcessParameters, &params,
                               sizeof(params), &bytes_read, 0);
  if (!NT_SUCCESS(st) || !params)
    return static_cast<id_t>(-1);

  // Step 3: read ProcessGroupId from the remote parameters.
  ULONG pgid = 0;
  st = ::NtReadVirtualMemoryEx(process, &params->ProcessGroupId, &pgid,
                               sizeof(pgid), &bytes_read, 0);
  if (!NT_SUCCESS(st))
    return static_cast<id_t>(-1);

  return static_cast<id_t>(pgid);
}

// Get the PID of a process from its handle.
ULONG_PTR query_process_pid(HANDLE process) {
  PROCESS_BASIC_INFORMATION pbi;
  NTSTATUS st = ::NtQueryInformationProcess(
      process, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
  if (!NT_SUCCESS(st))
    return 0;
  return pbi.UniqueProcessId;
}

// ============================================================================
// System-wide process iteration
// ============================================================================
//
// NtGetNextProcess walks the kernel's process list. When a process doesn't
// grant the requested DesiredAccess, the kernel silently skips it — no error,
// just the next eligible process. This is ideal: unprivileged callers see
// their own and other unprivileged processes, while elevated callers see
// everything. Protected processes (anti-malware, DRM) are always skipped.
//
// Each returned handle must be closed by the caller (or by this iterator).

template <typename Fn> void for_each_process(ACCESS_MASK access, Fn &&visitor) {
  windows::ScopedNtHandle prev;
  for (;;) {
    windows::ScopedNtHandle next;
    NTSTATUS st = ::NtGetNextProcess(prev.get(), access, 0, 0, next.put());
    prev.reset();
    if (st == STATUS_NO_MORE_ENTRIES || !NT_SUCCESS(st))
      break;
    if (visitor(next.get()))
      break;
    prev = static_cast<windows::ScopedNtHandle &&>(next);
  }
}

// ============================================================================
// PRIO_PROCESS helpers
// ============================================================================

intptr_t getpriority_process(id_t who) {
  if (who == 0) {
    int nice = query_process_nice(NtCurrentProcess());
    if (nice == NICE_QUERY_FAILED)
      return -ESRCH;
    return kNZero - nice;
  }

  windows::ScopedNtHandle proc;
  NTSTATUS st =
      NtOpenProcessById(proc.put(), PROCESS_QUERY_LIMITED_INFORMATION,
                        static_cast<DWORD>(who));
  if (!NT_SUCCESS(st))
    return -ESRCH;
  int nice = query_process_nice(proc.get());
  if (nice == NICE_QUERY_FAILED)
    return -ESRCH;
  return kNZero - nice;
}

intptr_t setpriority_process(id_t who, int nice) {
  if (who == 0)
    return set_process_nice(NtCurrentProcess(), nice);

  windows::ScopedNtHandle proc;
  NTSTATUS st =
      NtOpenProcessById(proc.put(), PROCESS_SET_INFORMATION,
                        static_cast<DWORD>(who));
  if (!NT_SUCCESS(st))
    return -ESRCH;
  int err = set_process_nice(proc.get(), nice);
  return err;
}

// ============================================================================
// PRIO_PGRP / PRIO_USER — system-wide get
// ============================================================================
//
// Walk all visible processes, match by pgid or uid, return the lowest nice
// value (highest priority) among matches. If no matches found but matching
// processes exist that we can't query, the result is ESRCH — same as Linux
// when PID-namespace isolation hides processes.

intptr_t getpriority_pgrp(id_t who) {
  id_t target_pgid = who;
  if (target_pgid == 0)
    target_pgid =
        static_cast<id_t>(g_pcb.identity.pgid.load(cpp::MemoryOrder::RELAXED));

  int min_nice = NICE_QUERY_FAILED;
  ULONG_PTR self_pid =
      reinterpret_cast<ULONG_PTR>(NtCurrentTeb()->ClientId.UniqueProcess);

  // NtGetNextProcess with VM_READ lets us read remote PEB for pgid.
  ACCESS_MASK access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
  for_each_process(access, [&](HANDLE proc) -> bool {
    // For the current process, use the cached pgid (avoids VM read on self).
    ULONG_PTR pid = query_process_pid(proc);
    id_t pgid;
    if (pid == self_pid)
      pgid =
          static_cast<id_t>(g_pcb.identity.pgid.load(cpp::MemoryOrder::RELAXED));
    else
      pgid = query_process_pgid(proc);

    if (pgid == target_pgid) {
      int nice = query_process_nice(proc);
      if (nice != NICE_QUERY_FAILED) {
        if (min_nice == NICE_QUERY_FAILED || nice < min_nice)
          min_nice = nice;
      }
    }
    return false; // Continue.
  });

  if (min_nice == NICE_QUERY_FAILED)
    return -ESRCH;
  return kNZero - min_nice;
}

intptr_t getpriority_user(id_t who) {
  uid_t target_uid = static_cast<uid_t>(who);
  if (who == 0)
    target_uid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);

  int min_nice = NICE_QUERY_FAILED;

  for_each_process(PROCESS_QUERY_LIMITED_INFORMATION, [&](HANDLE proc) -> bool {
    uid_t uid = query_process_uid(proc);
    if (uid == target_uid) {
      int nice = query_process_nice(proc);
      if (nice != NICE_QUERY_FAILED) {
        if (min_nice == NICE_QUERY_FAILED || nice < min_nice)
          min_nice = nice;
      }
    }
    return false;
  });

  if (min_nice == NICE_QUERY_FAILED)
    return -ESRCH;
  return kNZero - min_nice;
}

// ============================================================================
// PRIO_PGRP / PRIO_USER — system-wide set
// ============================================================================
//
// Two concerns for set operations:
//
// 1. We need to MATCH processes (requires QUERY access + VM_READ for pgid)
//    and SET their priority (requires SET_INFORMATION). Using a combined
//    access mask in NtGetNextProcess means processes that grant QUERY but
//    not SET are invisible — we can't distinguish "not found" from "found
//    but permission denied".
//
// 2. POSIX requires: ESRCH if no matching process exists, EPERM if matching
//    processes exist but the caller lacks permission to modify them.
//
// Strategy: enumerate with QUERY-only access for matching. For each match,
// re-open with SET_INFORMATION to modify. Track both "found any match" and
// "any permission error" separately.

intptr_t setpriority_pgrp(id_t who, int nice) {
  id_t target_pgid = who;
  if (target_pgid == 0)
    target_pgid =
        static_cast<id_t>(g_pcb.identity.pgid.load(cpp::MemoryOrder::RELAXED));

  bool found = false;
  int last_err = 0;
  ULONG_PTR self_pid =
      reinterpret_cast<ULONG_PTR>(NtCurrentTeb()->ClientId.UniqueProcess);

  ACCESS_MASK access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ;
  for_each_process(access, [&](HANDLE proc) -> bool {
    ULONG_PTR pid = query_process_pid(proc);
    id_t pgid;
    if (pid == self_pid)
      pgid =
          static_cast<id_t>(g_pcb.identity.pgid.load(cpp::MemoryOrder::RELAXED));
    else
      pgid = query_process_pgid(proc);

    if (pgid != target_pgid)
      return false;

    found = true;

    // For self, set directly. For others, re-open with SET_INFORMATION.
    if (pid == self_pid) {
      int err = set_process_nice(NtCurrentProcess(), nice);
      if (err != 0)
        last_err = err;
    } else {
      windows::ScopedNtHandle set_proc;
      NTSTATUS st = NtOpenProcessById(set_proc.put(), PROCESS_SET_INFORMATION,
                                      static_cast<DWORD>(pid));
      if (NT_SUCCESS(st)) {
        int err = set_process_nice(set_proc.get(), nice);
        if (err != 0)
          last_err = err;
      } else {
        last_err = -EPERM;
      }
    }
    return false;
  });

  if (!found)
    return -ESRCH;
  return last_err;
}

intptr_t setpriority_user(id_t who, int nice) {
  uid_t target_uid = static_cast<uid_t>(who);
  if (who == 0)
    target_uid = g_pcb.identity.eff_uid.load(cpp::MemoryOrder::RELAXED);

  bool found = false;
  int last_err = 0;
  ULONG_PTR self_pid =
      reinterpret_cast<ULONG_PTR>(NtCurrentTeb()->ClientId.UniqueProcess);

  for_each_process(PROCESS_QUERY_LIMITED_INFORMATION, [&](HANDLE proc) -> bool {
    uid_t uid = query_process_uid(proc);
    if (uid != target_uid)
      return false;

    found = true;
    ULONG_PTR pid = query_process_pid(proc);

    if (pid == self_pid) {
      int err = set_process_nice(NtCurrentProcess(), nice);
      if (err != 0)
        last_err = err;
    } else {
      windows::ScopedNtHandle set_proc;
      NTSTATUS st = NtOpenProcessById(set_proc.put(), PROCESS_SET_INFORMATION,
                                      static_cast<DWORD>(pid));
      if (NT_SUCCESS(st)) {
        int err = set_process_nice(set_proc.get(), nice);
        if (err != 0)
          last_err = err;
      } else {
        last_err = -EPERM;
      }
    }
    return false;
  });

  if (!found)
    return -ESRCH;
  return last_err;
}

} // anonymous namespace

// ============================================================================
// Public engine API
// ============================================================================

intptr_t getpriority(int which, id_t who) {
  switch (which) {
  case PRIO_PROCESS:
    return getpriority_process(who);
  case PRIO_PGRP:
    return getpriority_pgrp(who);
  case PRIO_USER:
    return getpriority_user(who);
  default:
    return -EINVAL;
  }
}

intptr_t setpriority(int which, id_t who, int nice) {
  // POSIX: implementations may clamp to the supported range.
  if (nice < -kNZero)
    nice = -kNZero;
  if (nice > kNZero - 1)
    nice = kNZero - 1;

  switch (which) {
  case PRIO_PROCESS:
    return setpriority_process(who, nice);
  case PRIO_PGRP:
    return setpriority_pgrp(who, nice);
  case PRIO_USER:
    return setpriority_user(who, nice);
  default:
    return -EINVAL;
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
