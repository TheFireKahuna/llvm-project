//===-- Child process tracking table for Windows -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tracks child processes created by posix_spawn. Each child gets a
// ChildEntry holding its process handle, PID, exit status, and a reactor
// watch that fires when the child exits (for SIGCHLD delivery and waitpid
// readiness).
//
// Stop/continue notification: posix_spawn creates a shared ChildStateBlock
// (anonymous section) and manual-reset event per child. The child writes
// its state on stop/continue and signals the event. The parent arms a
// second reactor watch to monitor state changes for waitpid(WUNTRACED/
// WCONTINUED) and SIGCHLD(CLD_STOPPED/CLD_CONTINUED).
//
// Entries are SlabPool-allocated (64KB slabs, hardened freelists). Reaped
// entries return to the pool. Slabs are never freed — child tracking
// is a process-lifetime resource like the signal thread registry.
//
// All access requires the lock. RawMutex (futex-based) for fork safety.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_PROCESS_WINDOWS_CHILD_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_PROCESS_WINDOWS_CHILD_TABLE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/section_view.h"
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"

#include "hdr/types/pid_t.h"
#include "hdr/types/struct_rusage.h"

// Forward-declare ChildStateBlock (defined in signal_internal.h) to avoid
// pulling signal headers into every child_table consumer.
namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
struct ChildStateBlock;
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

namespace LIBC_NAMESPACE_DECL {
namespace process {

namespace reactor = internal::reactor;

struct ChildEntry {
  HANDLE process_handle;        // native child process handle
  reactor::ReactorToken exit_token{reactor::INVALID_TOKEN};
  ChildEntry *next;             // active list forward link
  ChildEntry *prev;             // active list backward link
  DWORD pid;
  pid_t pgid;                   // POSIX process group ID
  cpp::Atomic<int> wait_status; // encoded per sys/wait macros, -1 = running
  cpp::Atomic<bool> exited;
  struct rusage exit_rusage;

  // Per-entry exit notification futex. Value 0 = alive, 1 = exited.
  // waitid_core uses exit_futex.wait<true>(0) for targeted per-child
  // park via NtWaitForAlertByThreadId — no handle limits, no handle
  // lifetime issues. The reactor callback does store_and_notify_all(1)
  // on child exit to wake all waiters via NtAlertMultipleThreadByThreadId.
  Futex exit_futex{0};

  // Lifetime guard for concurrent waiters. Incremented under table.lock
  // before parking on exit_futex, decremented after the wait returns.
  // finalize_child_reap spins until wait_refs == 0 before closing
  // process_handle. Bounded: child is exited → futex value is 1 →
  // wait returns in ~1 instruction (CAS-64 fast-path value mismatch).
  cpp::Atomic<uint32_t> wait_refs{0};

  // Per-group intrusive chain for O(k) group enumeration.
  // All group chain mutations are serialized by the process child-table lock.
  ChildEntry *group_next;       // next entry in same process group
  ChildEntry *group_prev;       // prev entry in same process group

  // Stop/continue notification from child (null for non-llvm-libc children).
  HANDLE state_change_event;    // manual-reset event, signaled by child
  reactor::ReactorToken state_token{reactor::INVALID_TOKEN};
  windows::SectionHandle state_section;  // anonymous section backing state_view
  windows::SectionView state_view;       // mapped shared memory

  // Typed accessor for the mapped ChildStateBlock.
  signal_state::ChildStateBlock *state_block() const {
    return state_view.as<signal_state::ChildStateBlock>();
  }

  // True if the child has called exec. Determined by reading exec_count
  // from the shared ChildStateBlock (mapped between parent and child).
  // posix_spawn children write exec_count=1 at startup; future fork
  // children would start at 0 and increment on execve.
  // Returns true if no state block exists (conservative — assume exec'd).
  bool has_execd() const;

  // Child lifecycle state for waitpid(WUNTRACED/WCONTINUED). Consumed
  // (reset to 0) by waitpid after reporting. Values:
  //   0     — running (or state already consumed)
  //   > 0   — stopped, value is the stop signal number
  //   -1    — continued (CHILD_STATE_CONTINUED)
  cpp::Atomic<int> child_state;
};

// ---------------------------------------------------------------------------
// Process group index — O(k) group enumeration
//
// Fixed hash table mapping pgid → head of per-group intrusive chain.
// 64 buckets covers typical workloads (shells, build systems). Collisions
// are resolved by linear probing. The index is maintained under the same
// child table lock that protects the active list — no additional
// synchronization needed.
// ---------------------------------------------------------------------------

struct PgidBucket {
  pid_t pgid;           // 0 = empty slot
  ChildEntry *head;     // head of per-group chain (nullptr if empty)
  int count;            // number of entries in this group
};

inline constexpr int PGID_BUCKET_COUNT = 64;
inline constexpr int PGID_BUCKET_MASK = PGID_BUCKET_COUNT - 1;

struct ChildReapTokens {
  reactor::ReactorToken exit_token;
  reactor::ReactorToken state_token;
};

// Initialize the child table. Called from startup code.
void init_child_table();
// Reset the child table in the fork child.
void fork_reinit_child_table();

// Track a new child. Arms reactor watches for exit and (if provided)
// state-change notification. Takes ownership of state_section via move.
// pgid is the initial process group ID for the child.
int track_child(HANDLE process_handle, DWORD pid, pid_t pgid,
                HANDLE state_event = nullptr,
                windows::SectionHandle state_section = {});

// The following operations act on the process child-table state directly.
// Callers must hold the child-table lock unless documented otherwise.
ChildEntry *find_child(pid_t pid);
ChildReapTokens unlink_child_for_reap(ChildEntry *entry);
void finalize_child_reap(ChildEntry *entry, ChildReapTokens tokens);
bool has_process_group(pid_t pgid);
int change_child_pgid(ChildEntry *entry, pid_t new_pgid);
int collect_group_pids(pid_t pgid, pid_t *out, int max_out);

// ---------------------------------------------------------------------------
// Process-global process group ID
//
// Every process has a single pgid (the group it belongs to). This is
// independent of the child table — the child table tracks children's
// pgids, while self_pgid is our own group membership.
//
// Initialized at startup from the parent's ProcessGroupId in the PEB.
// Updated by setpgid(0, ...) or setsid(). Read by getpgrp()/getpgid(0)
// and waitpid(0, ...) to identify the caller's group.
// ---------------------------------------------------------------------------
pid_t get_self_pgid();
void set_self_pgid(pid_t pgid);

} // namespace process
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_PROCESS_WINDOWS_CHILD_TABLE_H
