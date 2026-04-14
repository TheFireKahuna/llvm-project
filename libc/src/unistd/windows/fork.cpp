//===-- Windows implementation of fork -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// fork() on NT-POSIX via NtCreateUserProcess (clone mode).
//
// Strategy: NtCreateUserProcess with no filename and no ProcessParameters
// clones the calling process's address space AND the calling thread in a
// single atomic syscall.  The parent receives STATUS_SUCCESS plus handles
// to the child process and thread.  The child's cloned thread receives
// STATUS_PROCESS_CLONED (0x00000129) and continues executing from the
// same point — true fork semantics with zero manual context save/restore.
//
// NtCreateProcessEx (the legacy API used prior to this rewrite) does NOT
// work for fork-with-execution on Windows >= 8.1: the kernel marks
// threadless cloned processes as "waiting for deletion" and refuses
// NtCreateThreadEx with STATUS_PROCESS_IS_TERMINATING (0xC000010A).
// NtCreateUserProcess avoids this entirely by creating the initial thread
// atomically during process creation.
//
//===----------------------------------------------------------------------===//

#include "src/unistd/fork.h"

#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/OSUtil/windows/nt/nt_peb.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/process/fork_reinit.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/process/windows/child_table.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(pid_t, fork, (void)) {
  // =====================================================================
  // Phase 1: Pre-fork — flush streams, acquire critical locks, run
  //          pthread_atfork prepare handlers.
  // =====================================================================
  internal::fork_prepare();

  // =====================================================================
  // Phase 2: Clone via NtCreateUserProcess.
  // =====================================================================
  // No filename + no ProcessParameters = fork mode.  The kernel clones
  // the address space (copy-on-write) and the calling thread.  The child
  // thread returns STATUS_PROCESS_CLONED; the parent returns SUCCESS.
  HANDLE child_process = nullptr;
  HANDLE child_thread = nullptr;

  PS_CREATE_INFO create_info = {};
  create_info.Size = sizeof(create_info);
  create_info.State = PsCreateInitialState;

  // Save the ThreadLocalStoragePointer from the TEB before cloning.
  // NtCreateUserProcess creates a new TEB for the child's cloned thread
  // but does NOT copy ThreadLocalStoragePointer (gs:0x58 on x64), leaving
  // it NULL.  Every __declspec(thread) / thread_local access would crash.
  //
  // The saved value MUST live on the stack, not in a register. The child's
  // cloned thread may not preserve callee-saved registers (the kernel sets
  // up a fresh CONTEXT), but the stack IS CoW-shared with the parent.
  // Using volatile prevents the compiler from hoisting this into a register
  // that might not survive the fork.
  volatile void *saved_tls_array;
#if defined(__x86_64__)
  __asm__ volatile("movq %%gs:0x58, %0" : "=r"(saved_tls_array));
#elif defined(__aarch64__)
  __asm__ volatile("ldr %0, [x18, #0x58]" : "=r"(saved_tls_array));
#endif

  NTSTATUS status = ::NtCreateUserProcess(
      &child_process,
      &child_thread,
      PROCESS_ALL_ACCESS,                   // ProcessDesiredAccess
      THREAD_ALL_ACCESS,                    // ThreadDesiredAccess
      nullptr,                              // ProcessObjectAttributes
      nullptr,                              // ThreadObjectAttributes
      PROCESS_CREATE_FLAGS_INHERIT_HANDLES, // ProcessFlags
      0,                                    // ThreadFlags
      nullptr,                              // ProcessParameters (nullptr = fork)
      &create_info,                         // CreateInfo
      nullptr);                             // AttributeList

  // =====================================================================
  // Child path — cloned thread continues here.
  // =====================================================================
  if (status == STATUS_PROCESS_CLONED) {
    // Restore ThreadLocalStoragePointer FIRST — before any code that
    // might touch thread_local variables (including the PEB access below
    // if it were to go through TLS-backed helpers). Read from the volatile
    // stack variable (CoW-safe), not from a register.
    {
      void *tls_ptr = const_cast<void *>(saved_tls_array);
#if defined(__x86_64__)
      __asm__ volatile("movq %0, %%gs:0x58" : : "r"(tls_ptr) : "memory");
#elif defined(__aarch64__)
      __asm__ volatile("str %0, [x18, #0x58]" : : "r"(tls_ptr) : "memory");
#endif
    }

    // The kernel already set our TEB.ClientId to the new PID/TID and
    // set PEB.InheritedAddressSpace + TEB.ClonedThread flags.
    //
    // Critical: the cloned PEB_LDR_DATA has ShutdownInProgress=TRUE,
    // which makes ntdll think the process is exiting. This causes
    // crashes in any ntdll function that checks this flag. Clear it
    // immediately before doing anything else.
    NtCurrentPeb()->Ldr->ShutdownInProgress = FALSE;

    // Run the full reinit chain: reset subsystem state (locks, reactor,
    // ALPC port, thread registry, etc.) then invoke pthread_atfork
    // child handlers.
    internal::fork_child();
    return 0;
  }

  // =====================================================================
  // Parent path — handle errors or success.
  // =====================================================================
  if (!NT_SUCCESS(status)) {
    internal::fork_parent(); // Unwind prepare handlers even on failure.
    libc_errno = windows_util::ntstatus_to_errno(status);
    return -1;
  }

  // =====================================================================
  // Phase 3: Parent post-fork — invoke pthread_atfork parent handlers,
  //          release critical locks.
  // =====================================================================
  internal::fork_parent();

  // =====================================================================
  // Phase 4: Get the child's PID via NtQueryInformationProcess.
  // =====================================================================
  PROCESS_BASIC_INFORMATION pbi = {};
  ULONG rlen = 0;
  ::NtQueryInformationProcess(child_process, ProcessBasicInformation,
                              &pbi, sizeof(pbi), &rlen);
  pid_t child_pid = static_cast<pid_t>(pbi.UniqueProcessId);

  // Close the child thread handle — we don't need it.
  ::NtClose(child_thread);

  // =====================================================================
  // Phase 5: Track the child in the child table for waitpid/SIGCHLD.
  // =====================================================================
  pid_t child_pgid = process::get_self_pgid();
  int track_err = process::track_child(child_process, child_pid, child_pgid);
  if (track_err) {
    // Child is already running — we can't un-fork it. The child will
    // become orphaned (reparented to init-equivalent). This is not
    // fatal: fork succeeded, we just can't wait for the child.
    // Close the handle to avoid leaking it in the parent.
    ::NtClose(child_process);
  }

  return static_cast<pid_t>(child_pid);
}

} // namespace LIBC_NAMESPACE_DECL
