//===-- NT thread, process, and system API declarations --------- *- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PROCESS_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PROCESS_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Process State Change (safe suspend/resume) - Windows 11+
//===----------------------------------------------------------------------===//

// Deadlock-free process suspension. The kernel captures required state at
// handle creation, so NtChangeProcessState never blocks on locks held by the
// target. Closing the handle auto-resumes the process if still suspended.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateProcessStateChange(
    HANDLE *StateChangeHandle, ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes, HANDLE ProcessHandle, ULONG_PTR Reserved);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtChangeProcessState(HANDLE StateChangeHandle, HANDLE ProcessHandle,
                     ULONG Action, PVOID ExtendedInformation,
                     SIZE_T ExtendedInformationLength, ULONG_PTR Reserved);

// Resume all threads — reverses both state-change and external suspension.
// Retained for SIGCONT (reverses suspension from debuggers, other processes).
// Prefer NtCreateProcessStateChange/NtChangeProcessState for new code.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtResumeProcess(HANDLE ProcessHandle);

//===----------------------------------------------------------------------===//
// Thread Alerting (futex-like primitives)
//===----------------------------------------------------------------------===//

// Efficient thread wake/wait without kernel objects.
// NtAlertThreadByThreadId takes a TID (cast to HANDLE), not a real handle —
// no THREAD_ALERT access right needed. Used after special APC queueing to
// wake threads in alert waits for prompt signal delivery.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtAlertThreadByThreadId(HANDLE ThreadId);

// NtAlertThreadByThreadIdEx (24H2+) — atomically releases an SRW lock and
// alerts the thread. Soft-resolved at init via nt_capabilities.h; call
// through g_pcb.zone0.optional().alert_thread_ex().

// NtAlertMultipleThreadByThreadId (24H2+) — batch wake multiple threads
// parked in NtWaitForAlertByThreadId. Soft-resolved at init via
// nt_capabilities.h; call through g_pcb.zone0.optional().alert_multiple().
//
// Extended parameter (Windows 11 24H2+): PsAlertMultipleExtendedParameter-
// AutoBoostContext (Type=0). Payload is an opaque 64-bit token associated
// with the synchronization/ownership handoff context. Passing the address
// of the synchronization object (mutex, futex, etc.) engages the NT
// kernel's AutoBoost (Ab) subsystem, which:
//   - Boosts woken threads' priority for their next quantum (improves
//     preemption latency at oversubscription)
//   - Associates the wake with the sync object for wait-chain analysis
//
// This is the same mechanism ntdll uses internally for SRWLock /
// WaitOnAddress wakes. Only the *last* extended parameter's payload is
// forwarded to internal alert logic; passing count=1 is the norm.


// Block until NtAlertThreadByThreadId wakes this thread. Address is an
// arbitrary user-mode address (not dereferenced — used only as a key).
// Timeout is negative relative 100ns units; nullptr = wait forever.
// Returns STATUS_ALERTED on wake, STATUS_TIMEOUT on expiry.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtWaitForAlertByThreadId(PVOID Address, const LARGE_INTEGER *Timeout);

// NtAlertThread — deliver an alert to a thread. If the thread is in an
// alertable wait, it returns STATUS_ALERTED. If not, the alert is queued
// and delivered when the thread next enters an alertable state or calls
// NtTestAlert. Used for pthread_cancel implementation — the "alerted"
// flag acts as the cancellation-pending indicator.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtAlertThread(HANDLE ThreadHandle);

// NtTestAlert — drain and deliver any pending alert to the calling thread
// without entering a wait. Returns STATUS_ALERTED if an alert was pending
// (and clears it), STATUS_SUCCESS otherwise. Used at the start of a
// critical region that must not be interrupted by APC delivery: any
// queued user APC is dispatched here, before the region opens, so the
// region itself runs to completion without an APC firing inside it.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtTestAlert(void);

// NtAlertResumeThread — atomically alert a thread and resume it if
// suspended. PreviousSuspendCount receives the suspend count before the
// resume (may be nullptr). Used for signal delivery: suspend the thread
// with NtCreateThreadStateChange, modify its context with NtSetContextThread,
// then NtAlertResumeThread to resume + deliver the alert atomically.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlertResumeThread(HANDLE ThreadHandle, ULONG *PreviousSuspendCount);

// Rtl address-wait primitives (ntdll.dll, Win8+). These are the underlying
// implementation of the kernel32.dll WaitOnAddress/WakeByAddress* family.
// Preferred over the Win32 wrappers in VEH context to avoid SetLastError
// side effects and kernel32.dll dependencies.
//
// RtlWaitOnAddress — wait for the value at Address to differ from
// CompareAddress. AddressSize must be 1, 2, 4, or 8. May return
// spuriously — caller must re-check the value in a loop.
// Timeout: negative relative 100ns units, nullptr = infinite.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlWaitOnAddress(volatile void *Address, const void *CompareAddress,
                 SIZE_T AddressSize, LARGE_INTEGER *Timeout);
// RtlWakeAddressSingle — wake one thread waiting on Address.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlWakeAddressSingle(PVOID Address);
// RtlWakeAddressAll — wake all threads waiting on Address.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlWakeAddressAll(PVOID Address);

// Flush CFG/ACG secure memory cache. Called by VirtualProtectEx on
// STATUS_INVALID_PAGE_PROTECTION (0xc0000045) before retrying.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOLEAN RtlFlushSecureMemoryCache(PVOID Address,
                                                              SIZE_T Size);

//===----------------------------------------------------------------------===//
// Thread State Change (safe suspend/resume) - Windows 11+
//===----------------------------------------------------------------------===//

// Provides deadlock-free thread suspension. If the caller crashes between
// suspend and resume, closing the state change handle auto-resumes the target.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateThreadStateChange(
    HANDLE *StateChangeHandle, DWORD DesiredAccess, PVOID ObjectAttributes,
    HANDLE ThreadHandle, ULONG_PTR Reserved);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtChangeThreadState(HANDLE StateChangeHandle, HANDLE ThreadHandle, ULONG Action,
                    PVOID ExtendedInformation, SIZE_T ExtendedInformationLength,
                    ULONG_PTR Reserved);

// Close a kernel handle. Returns STATUS_INVALID_HANDLE on failure.
// Safe to call on pseudo-handles (NtCurrentProcess, NtCurrentThread) — no-op.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtClose(HANDLE Handle);

// Duplicate a handle from SourceProcess into TargetProcess.
// TargetProcess may be nullptr if DUPLICATE_CLOSE_SOURCE is the only goal.
// TargetHandle may be nullptr if no duplicate is needed (close-only).
// HandleAttributes: OBJ_INHERIT (0x2) makes the result inheritable.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtDuplicateObject(
    HANDLE SourceProcess, HANDLE SourceHandle, HANDLE TargetProcess,
    PHANDLE TargetHandle, ACCESS_MASK DesiredAccess, ULONG HandleAttributes,
    ULONG Options);

// RtlCreateHeap — create a heap object. Returns a heap handle or nullptr.
// HeapBase: nullptr = system allocates, else caller-provided memory block.
// ReserveSize: initial address space reservation (0 = 1MB default).
// CommitSize: initial committed memory (0 = one page).
// Lock: nullptr = heap provides its own; else caller-owned opaque lock.
// Parameters: nullptr or RTL_HEAP_PARAMETERS for advanced tuning.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID RtlCreateHeap(ULONG Flags, PVOID HeapBase,
                                                SIZE_T ReserveSize,
                                                SIZE_T CommitSize, PVOID Lock,
                                                PVOID Parameters);

// RtlAllocateHeap — allocate Size bytes from HeapHandle.
// Returns nullptr on failure (or raises exception if HEAP_GENERATE_EXCEPTIONS).
// HEAP_ZERO_MEMORY in Flags zero-fills the allocation.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID RtlAllocateHeap(PVOID HeapHandle, ULONG Flags,
                                                  SIZE_T Size);

// RtlReAllocateHeap — resize an existing allocation. Returns the new base
// (may differ from BaseAddress). Returns nullptr on failure.
// HEAP_REALLOC_IN_PLACE_ONLY prevents moving the block.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID RtlReAllocateHeap(PVOID HeapHandle,
                                                    ULONG Flags,
                                                    PVOID BaseAddress,
                                                    SIZE_T Size);

// RtlSizeHeap — return the usable size of an allocation. Returns (SIZE_T)-1
// on invalid BaseAddress.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR SIZE_T RtlSizeHeap(PVOID HeapHandle, ULONG Flags,
                                               PVOID BaseAddress);

// RtlFreeHeap — free an allocation. Returns TRUE on success.
// BaseAddress nullptr is a valid no-op (like free(NULL)).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOL RtlFreeHeap(PVOID HeapHandle, ULONG Flags,
                                             PVOID BaseAddress);

// SRW (Slim Reader/Writer) lock — ntdll entry points backing
// kernel32 AcquireSRWLockExclusive/ReleaseSRWLockExclusive.
// Non-recursive. SRWLock must be initialized to RTL_SRWLOCK_INIT (0).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlAcquireSRWLockExclusive(PRTL_SRWLOCK SRWLock);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlReleaseSRWLockExclusive(PRTL_SRWLOCK SRWLock);

//===----------------------------------------------------------------------===//
// Process/Thread Enumeration
//===----------------------------------------------------------------------===//

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtGetNextProcess(HANDLE ProcessHandle,
                                                      ACCESS_MASK DesiredAccess,
                                                      ULONG HandleAttributes,
                                                      ULONG Flags,
                                                      HANDLE *NewProcessHandle);

// Iterate threads of a process without a snapshot. Pass nullptr for
// ThreadHandle to get the first thread; pass the previous handle to get the
// next. The returned handle has DesiredAccess rights and must be closed by
// the caller. Returns STATUS_NO_MORE_ENTRIES when iteration is complete.
// HandleAttributes: 0 or OBJ_INHERIT. Flags: unused, must be 0.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtGetNextThread(HANDLE ProcessHandle, HANDLE ThreadHandle, DWORD DesiredAccess,
                ULONG HandleAttributes, ULONG Flags, HANDLE *NewThreadHandle);

//===----------------------------------------------------------------------===//
// APCs — Special user APCs can interrupt alertable waits (Win11+)
//===----------------------------------------------------------------------===//

// Queue an APC to ThreadHandle. ReserveHandle is an optional reserve object
// from NtAllocateReserveObject (or QUEUE_USER_APC_SPECIAL_USER_APC = 0x1).
// With QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC, the APC fires immediately
// when the thread calls NtTestAlert, NtAlertThread, NtAlertResumeThread,
// or NtAlertThreadByThreadId — without requiring an alertable wait.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueueApcThreadEx2(HANDLE ThreadHandle, HANDLE ReserveHandle, ULONG ApcFlags,
                    PPS_APC_ROUTINE ApcRoutine, PVOID ApcArgument1,
                    PVOID ApcArgument2, PVOID ApcArgument3);

//===----------------------------------------------------------------------===//
// Thread Lifecycle
//===----------------------------------------------------------------------===//

// NtOpenProcess — open a handle to an existing process by CLIENT_ID.
// ClientId->UniqueProcess identifies the target process; UniqueThread may
// be 0 (ignored for process open). ObjectAttributes may be nullptr.
// DesiredAccess: PROCESS_QUERY_INFORMATION, PROCESS_VM_READ,
// PROCESS_TERMINATE, PROCESS_SUSPEND_RESUME, etc.
// Required for kill() (sending signals to other processes), waitpid(),
// and any cross-process management.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenProcess(HANDLE *ProcessHandle, ACCESS_MASK DesiredAccess,
              PCOBJECT_ATTRIBUTES ObjectAttributes, PCLIENT_ID ClientId);

// Convenience wrapper: open a process by numeric PID.
// Equivalent to Win32 OpenProcess but returns NTSTATUS.
inline NTSTATUS NtOpenProcessById(HANDLE *out, ACCESS_MASK access, DWORD pid) {
  CLIENT_ID cid = {reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid)),
                   nullptr};
  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, nullptr, 0, nullptr, nullptr);
  return ::NtOpenProcess(out, access, &oa, &cid);
}

// Open a thread by CLIENT_ID. ObjectAttributes may be nullptr.
// ClientId->UniqueThread identifies the target thread; UniqueProcess
// is optional (0 = any process). DesiredAccess varies by intended use:
// THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION for priority ops,
// THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT for
// register manipulation.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenThread(HANDLE *ThreadHandle, ACCESS_MASK DesiredAccess,
             PCOBJECT_ATTRIBUTES ObjectAttributes, PCLIENT_ID ClientId);

// Terminate a process and all its threads. ProcessHandle: nullptr
// terminates the calling process; (HANDLE)-1 (NtCurrentProcess) also
// works. ExitStatus becomes the process exit code (GetExitCodeProcess).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtTerminateProcess(HANDLE ProcessHandle,
                                                        NTSTATUS ExitStatus);

// Terminate a thread. ThreadHandle: nullptr terminates the calling thread.
// ExitStatus becomes the thread exit code (GetExitCodeThread).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtTerminateThread(HANDLE ThreadHandle,
                                                       NTSTATUS ExitStatus);

// Resume a suspended thread. PreviousSuspendCount receives the thread's
// suspend count before the decrement (0 = was not suspended, >0 = still
// suspended after this call). Used after NtCreateThreadEx with
// CREATE_SUSPENDED to start execution.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtResumeThread(HANDLE ThreadHandle,
                                                     PULONG PreviousSuspendCount);

// Capture/restore a thread's full register state. The thread must be
// suspended via NtChangeThreadState first. Used by the SIGTSTP
// context-redirect mechanism.
// ThreadContext->ContextFlags must be set to specify which register groups
// to capture/restore (CONTEXT_CONTROL, CONTEXT_INTEGER, CONTEXT_FULL, etc.).
// Requires THREAD_GET_CONTEXT / THREAD_SET_CONTEXT access rights.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtGetContextThread(HANDLE ThreadHandle,
                                                        CONTEXT *ThreadContext);
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetContextThread(HANDLE ThreadHandle,
                                                        CONTEXT *ThreadContext);

// Yield the current thread's remaining quantum. Returns
// STATUS_NO_YIELD_PERFORMED if no other runnable thread was available.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtYieldExecution(void);

//===----------------------------------------------------------------------===//
// Thread Creation — NtCreateThreadEx
//===----------------------------------------------------------------------===//

// NtCreateThreadEx — modern thread creation with full control over stack
// layout, creation flags, and attribute list. Superior to CreateThread for
// libc: gives direct control over stack sizes, ZeroBits (address space
// placement), and thread attributes without the kernel32 layer.
//
// ProcessHandle: target process (NtCurrentProcess() for same-process).
// StartRoutine: thread entry point (returns NTSTATUS).
// Argument: opaque parameter passed to StartRoutine.
// CreateFlags: THREAD_CREATE_FLAGS_* bitmask.
// ZeroBits: high-order address bits that must be zero in the stack address
//           (0 = default placement, higher values constrain to lower
//           addresses).
// StackSize: initial committed stack size (0 = default from PE header).
// MaximumStackSize: reserved stack size (0 = default from PE header).
// AttributeList: optional PS_ATTRIBUTE_LIST for TEB address, group affinity,
//                ideal processor, etc. May be nullptr.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateThreadEx(
    HANDLE *ThreadHandle, ACCESS_MASK DesiredAccess,
    PCOBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
    PVOID StartRoutine, PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits,
    SIZE_T StackSize, SIZE_T MaximumStackSize, PVOID AttributeList);

//===----------------------------------------------------------------------===//
// Process Creation — NtCreateProcessEx / NtCreateUserProcess
//===----------------------------------------------------------------------===//

// NtCreateProcessEx — create a process from a section handle with explicit
// control over parent, flags, debug port, and token. This is the lower-level
// process creation syscall that separates process creation from thread
// creation — the caller must manually create the initial thread with
// NtCreateThreadEx after setting up the PEB and process parameters.
//
// This is the essential primitive for fork() emulation on Windows:
// 1. NtCreateProcessEx with PROCESS_CREATE_FLAGS_INHERIT_HANDLES clones
//    the parent's handle table.
// 2. The new process shares the parent's section (image) and address space
//    layout can be configured before the first thread runs.
// 3. The caller sets up RTL_USER_PROCESS_PARAMETERS in the child's address
//    space, then creates the initial thread with NtCreateThreadEx.
//
// ParentProcess: handle to the parent process (whose handle table, token,
//                and address space layout influence the child).
// Flags: PROCESS_CREATE_FLAGS_* bitmask. Notable for fork():
//   - PROCESS_CREATE_FLAGS_INHERIT_HANDLES: inherit parent's handle table.
//   - PROCESS_CREATE_FLAGS_CLONE_MINIMAL: minimal process clone (Win8.1+).
//   - PROCESS_CREATE_FLAGS_CREATE_SUSPENDED: start with suspended initial
//   thread.
// SectionHandle: image section to map as the process executable. nullptr to
//                inherit the parent's image section (fork-like behavior).
// DebugPort: debug port handle, or nullptr for no debugging.
// TokenHandle: primary token for the new process. nullptr to inherit from
// parent. Reserved: must be 0 (reserved for JobMemberLevel).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateProcessEx(
    HANDLE *ProcessHandle, ACCESS_MASK DesiredAccess,
    PCOBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess, ULONG Flags,
    HANDLE SectionHandle, HANDLE DebugPort, HANDLE TokenHandle, ULONG Reserved);

// NtCreateUserProcess — create a process and its initial thread in a single
// syscall. This is the modern process creation path (Vista+) that replaces
// the NtCreateProcess + NtCreateThread two-step. Implements posix_spawn()
// and the exec() family.
//
// ProcessParameters: RTL_USER_PROCESS_PARAMETERS (image path, command line,
//                    environment, working directory, std handles).
// CreateInfo: in/out — set Size and InitState on input; State and
//             SuccessState populated on output.
// AttributeList: PS_ATTRIBUTE_LIST specifying parent process, image name,
//                handle list, std handle duplication, etc.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateUserProcess(
    HANDLE *ProcessHandle, HANDLE *ThreadHandle,
    ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess,
    PCOBJECT_ATTRIBUTES ProcessObjectAttributes,
    PCOBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ProcessFlags,
    ULONG ThreadFlags, RTL_USER_PROCESS_PARAMETERS *ProcessParameters,
    PS_CREATE_INFO *CreateInfo, PS_ATTRIBUTE_LIST *AttributeList);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS RtlCreateProcessParametersEx(
    RTL_USER_PROCESS_PARAMETERS **ProcessParameters,
    const UNICODE_STRING *ImagePathName, const UNICODE_STRING *DllPath,
    const UNICODE_STRING *CurrentDirectory, const UNICODE_STRING *CommandLine,
    PVOID Environment, const UNICODE_STRING *WindowTitle,
    const UNICODE_STRING *DesktopInfo, const UNICODE_STRING *ShellInfo,
    const UNICODE_STRING *RuntimeData, ULONG Flags);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlDestroyProcessParameters(RTL_USER_PROCESS_PARAMETERS *ProcessParameters);

// RtlCloneUserProcess — fork() primitive.  Wraps NtCreateUserProcess in
// clone mode with ntdll-internal state protection:
//
//   1. RtlPrepareForProcessCloning — acquires ntdll-internal locks
//      (loader, PEB, TLS, FLS, heap) and drains the thread pool.
//   2. NtCreateUserProcess(nullptr, nullptr) — CoW clone.
//   3. RtlCompleteProcessCloning — parent: releases locks.
//      Child: resets all cloned ntdll critical sections and SRWLocks
//      (loader lock, PEB lock, heap locks, FLS, TLS) to unlocked state.
//
// Returns STATUS_PROCESS_CLONED (0x129) in the child, STATUS_SUCCESS in
// the parent.  ProcessInformation is filled only in the parent.
//
// ProcessFlags: RTL_CLONE_PROCESS_FLAGS_* bitmask.
//   INHERIT_HANDLES (0x2) — inherit parent's handle table.
//   NO_SYNCHRONIZE  (0x8) — skip Prepare/Complete (unsafe, for debugging).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS RtlCloneUserProcess(
    ULONG ProcessFlags, PSECURITY_DESCRIPTOR ProcessSecurityDescriptor,
    PSECURITY_DESCRIPTOR ThreadSecurityDescriptor, HANDLE DebugPort,
    RTL_USER_PROCESS_INFORMATION *ProcessInformation);

// Exposed for advanced use (e.g., custom clone protocols).  Not needed when
// using RtlCloneUserProcess, which calls these internally.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS RtlPrepareForProcessCloning(void);
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS RtlCompleteProcessCloning(ULONG Completed);

//===----------------------------------------------------------------------===//
// Process/Thread Information Queries
//===----------------------------------------------------------------------===//

// NtQueryInformationThread — retrieve thread attributes by information class.
// ThreadHandle: thread handle with THREAD_QUERY_INFORMATION or
//   THREAD_QUERY_LIMITED_INFORMATION access (class-dependent).
// ReturnLength: optional; receives required buffer size on
//   STATUS_INFO_LENGTH_MISMATCH. Pass nullptr if not needed.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtQueryInformationThread(
    HANDLE ThreadHandle, ULONG ThreadInformationClass, PVOID ThreadInformation,
    ULONG ThreadInformationLength, ULONG *ReturnLength);

// NtSetInformationThread — modify thread attributes by information class.
// ThreadHandle: thread handle with THREAD_SET_INFORMATION or
//   THREAD_SET_LIMITED_INFORMATION access (class-dependent).
// Buffer layout is class-specific: ThreadBasePriority (3) takes KPRIORITY,
// ThreadGroupInformation (30) takes GROUP_AFFINITY, ThreadSelectedCpuSets (39)
// takes ULONG[] of CPU set IDs, etc.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetInformationThread(HANDLE ThreadHandle, ULONG ThreadInformationClass,
                       PVOID ThreadInformation, ULONG ThreadInformationLength);

// NtGetCurrentProcessorNumberEx — identify the current logical processor.
// Returns the system-wide processor number (kernel-assigned, not necessarily
// contiguous). ProcessorNumber optionally receives a PROCESSOR_NUMBER with
// the group-relative identity: Group (0..n-1) and Number (0..m-1 within
// group). Pass nullptr if only the system-wide number is needed.
// cpu_set_t mapping: bit = Group * 64 + Number.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR ULONG
NtGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcessorNumber);

// NtQueryInformationProcess — retrieve process attributes by information class.
// ProcessHandle: process handle with PROCESS_QUERY_INFORMATION or
//   PROCESS_QUERY_LIMITED_INFORMATION access (class-dependent).
// ReturnLength: optional; receives actual/required buffer size.
// Returns STATUS_INFO_LENGTH_MISMATCH if buffer too small.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                          PVOID ProcessInformation,
                          ULONG ProcessInformationLength, ULONG *ReturnLength);

// NtSetInformationProcess — modify process attributes by information class.
// ProcessHandle: process handle with PROCESS_SET_INFORMATION access
//   (class-dependent).
// Buffer layout is class-specific: ProcessAccessToken takes a
//   PROCESS_ACCESS_TOKEN, ProcessQuotaLimits takes QUOTA_LIMITS_EX,
//   ProcessPriorityClass takes PROCESS_PRIORITY_CLASS, etc.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                        PVOID ProcessInformation,
                        ULONG ProcessInformationLength);

//===----------------------------------------------------------------------===//
// System Information — NtQuerySystemInformation
//===----------------------------------------------------------------------===//

// NtQuerySystemInformation — retrieve system-wide information.
// Pass nullptr / 0 for the buffer to probe the required size (returns
// STATUS_INFO_LENGTH_MISMATCH with ReturnLength filled in).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation,
                         ULONG SystemInformationLength, PULONG ReturnLength);

// NtQuerySystemInformationEx — extended system query with a class-specific
// input buffer. Input meaning varies by class:
//   SystemLogicalProcessorInformationEx (73): LOGICAL_PROCESSOR_RELATIONSHIP
//     enum value to filter by (RelationGroup, RelationNumaNode, etc.)
//   SystemCpuSetInformation (175): HANDLE to the target process
//     (NtCurrentProcess() for the calling process)
// Same probe pattern as NtQuerySystemInformation for buffer sizing.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQuerySystemInformationEx(ULONG SystemInformationClass, PVOID InputBuffer,
                           ULONG InputBufferLength, PVOID SystemInformation,
                           ULONG SystemInformationLength, PULONG ReturnLength);

// NtQueryTimerResolution — system timer resolution in 100-ns units.
// MinimumResolution: longest interval (lowest resolution, ~156250 = 15.625ms).
// MaximumResolution: shortest interval (highest resolution, ~5000 = 0.5ms).
// ActualResolution: currently active interval.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryTimerResolution(PULONG MinimumResolution, PULONG MaximumResolution,
                       PULONG ActualResolution);

// NtSetTimerResolution — request a change to the system timer resolution.
// DesiredTime: requested interval in 100-ns units (clamped to min/max range).
// SetResolution: TRUE = request higher resolution, FALSE = release a prior
// request (system reverts to lowest requested resolution among all callers).
// ActualTime: receives the resolution actually set.
// Needed for high-precision clock_nanosleep() implementation.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetTimerResolution(ULONG DesiredTime,
                                                          BOOLEAN SetResolution,
                                                          ULONG *ActualTime);

//===----------------------------------------------------------------------===//
// Event and Wait APIs
//===----------------------------------------------------------------------===//

// NtCreateEvent - create a kernel event object.
// SynchronizationEvent = auto-reset (consumed by wait).
// NotificationEvent = manual-reset (stays signaled until explicit reset).
// Named events (via ObjectAttributes) are cross-process.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
              PCOBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType,
              BOOLEAN InitialState);

// NtOpenEvent — open an existing named event object.
// Used for cross-process synchronization with sem_open-style semantics.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenEvent(HANDLE *EventHandle, ACCESS_MASK DesiredAccess,
            PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtSetEvent - signal an event. Returns the previous state via PreviousState
// (may be nullptr). Wakes one waiter (auto-reset) or all (manual-reset).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetEvent(HANDLE EventHandle,
                                                PLONG PreviousState);

// NtSetEventEx — signal an event and optionally release an SRWLOCK atomically.
// Eliminates the race window between releasing a lock and signaling a waiter,
// which is the core pattern for condition variable implementations.
// Windows 11 23H2+ (syscall 0x01a0 on 23H2, 0x01a1 on 24H2+).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetEventEx(HANDLE EventHandle,
                                                  PRTL_SRWLOCK Lock);

// NtResetEvent - unsignal a manual-reset event.
// PreviousState receives the state before the reset (may be nullptr).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtResetEvent(HANDLE EventHandle,
                                                  PLONG PreviousState);

// NtClearEvent — unsignal an event without returning the previous state.
// Simpler than NtResetEvent when the previous state isn't needed.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtClearEvent(HANDLE EventHandle);

// NtPulseEvent — atomically signal and then reset an event.
// For notification (manual-reset) events: releases all currently waiting
// threads, then resets to non-signaled. For synchronization (auto-reset)
// events: releases exactly one waiter, then resets.
// Note: inherently racy — threads that arrive between signal and reset
// miss the pulse. Prefer explicit signal+reset for reliable wake patterns.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtPulseEvent(HANDLE EventHandle,
                                                  PLONG PreviousState);

// NtQueryEvent — query an event's type and current state.
// Useful for non-blocking condition variable state checks and diagnostics.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtQueryEvent(
    HANDLE EventHandle, EVENT_INFORMATION_CLASS EventInformationClass,
    PVOID EventInformation, ULONG EventInformationLength, ULONG *ReturnLength);

// NtWaitForSingleObject - block until object is signaled.
// Alertable=TRUE enables APC delivery (STATUS_USER_APC on signal/EINTR).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, LARGE_INTEGER *Timeout);

// NtSignalAndWaitForSingleObject - atomically signal one object and wait on
// another. For mutants, "signal" means release. Eliminates the race window
// between releasing a lock and starting a wait.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSignalAndWaitForSingleObject(HANDLE SignalHandle, HANDLE WaitHandle,
                               BOOLEAN Alertable, LARGE_INTEGER *Timeout);

// NtWaitForMultipleObjects - block until one/all objects signaled.
// Enables select()/poll() on multiple completion events.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtWaitForMultipleObjects(ULONG Count, HANDLE *Handles, WAIT_TYPE WaitType,
                         BOOLEAN Alertable, LARGE_INTEGER *Timeout);

// NtDelayExecution - alertable sleep.
// Timeout is negative relative 100ns units (e.g., -10000000 = 1 second).
// Alertable=TRUE: returns STATUS_USER_APC if a signal fires mid-sleep.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtDelayExecution(BOOLEAN Alertable,
                                                      LARGE_INTEGER *Interval);

//===----------------------------------------------------------------------===//
// Timer APIs (NtCreateTimer2 / NtSetTimer2 / NtCancelTimer2 / NtQueryTimer)
//===----------------------------------------------------------------------===//

// NtCreateTimer2 — create a waitable timer (Windows 10+).
// Attributes: 0 = SynchronizationTimer, TIMER2_ATTRIBUTE_NOTIFICATION =
// NotificationTimer. TIMER2_ATTRIBUTE_HIGH_RESOLUTION for sub-ms precision.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateTimer2(
    PHANDLE TimerHandle, PVOID Reserved1, PCOBJECT_ATTRIBUTES ObjectAttributes,
    ULONG Attributes, ACCESS_MASK DesiredAccess);

// NtSetTimer2 — arm a timer. DueTime: positive = absolute (100ns since
// 1601-01-01), negative = relative. Period: nullptr = one-shot, else periodic
// interval in 100ns units.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetTimer2(HANDLE TimerHandle,
                                                 LARGE_INTEGER *DueTime,
                                                 LARGE_INTEGER *Period,
                                                 T2_SET_PARAMETERS *Parameters);

// NtSetTimer (v1) — arm a timer with wake-from-suspend support.
// DueTime: positive = absolute, negative = relative (100ns units).
// TimerApcRoutine: NULL = signal handle only (no APC).
// ResumeTimer: TRUE = wake system from S3 suspend when timer fires.
// Period: repeat interval in milliseconds (0 = one-shot).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetTimer(HANDLE TimerHandle, LARGE_INTEGER *DueTime, PVOID TimerApcRoutine,
           PVOID TimerContext, BOOLEAN ResumeTimer, LONG Period,
           BOOLEAN *PreviousState);

// NtCancelTimer2 — disarm a timer without destroying it.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCancelTimer2(HANDLE TimerHandle,
                                                    PVOID Parameters);

// NtQueryTimer — query remaining time and state.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtQueryTimer(
    HANDLE TimerHandle, TIMER_INFORMATION_CLASS TimerInformationClass,
    PVOID TimerInformation, ULONG TimerInformationLength, PULONG ReturnLength);

//===----------------------------------------------------------------------===//
// Mutant (Kernel Mutex) — cross-process mutual exclusion
//===----------------------------------------------------------------------===//

// NtCreateMutant - create or open a kernel mutex.
// Named mutants (via ObjectAttributes) are cross-process.
// InitialOwner=TRUE acquires immediately in the calling thread.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateMutant(PHANDLE MutantHandle, ACCESS_MASK DesiredAccess,
               PCOBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner);

// NtReleaseMutant - release a mutant owned by the calling thread.
// PreviousCount receives the previous recursion count (may be nullptr).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtReleaseMutant(HANDLE MutantHandle,
                                                     PLONG PreviousCount);

//===----------------------------------------------------------------------===//
// Semaphore — cross-process counting semaphore
//===----------------------------------------------------------------------===//

// NtCreateSemaphore - create or open a kernel semaphore.
// Named semaphores (via ObjectAttributes) are cross-process.
// MaximumCount is the upper bound; InitialCount is the starting value.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCreateSemaphore(
    PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
    PCOBJECT_ATTRIBUTES ObjectAttributes, LONG InitialCount, LONG MaximumCount);

// NtOpenSemaphore - open a handle to an existing named semaphore.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenSemaphore(PHANDLE SemaphoreHandle, ACCESS_MASK DesiredAccess,
                PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtQuerySemaphore - retrieve the current count of a semaphore.
// SemaphoreInformationClass must be SemaphoreBasicInformation (0).
// SemaphoreInformation points to a SEMAPHORE_BASIC_INFORMATION struct.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQuerySemaphore(HANDLE SemaphoreHandle,
                 ULONG SemaphoreInformationClass,
                 PVOID SemaphoreInformation,
                 ULONG SemaphoreInformationLength, PULONG ReturnLength);

struct SEMAPHORE_BASIC_INFORMATION {
  LONG CurrentCount;
  LONG MaximumCount;
};

// NtReleaseSemaphore - increment the semaphore count by ReleaseCount.
// PreviousCount receives the pre-increment value (may be nullptr).
// Returns STATUS_SEMAPHORE_LIMIT_EXCEEDED if count would exceed maximum.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtReleaseSemaphore(HANDLE SemaphoreHandle,
                                                        LONG ReleaseCount,
                                                        PLONG PreviousCount);

//===----------------------------------------------------------------------===//
// DLL Load Notifications
//===----------------------------------------------------------------------===//

// Register a process-wide callback for DLL load/unload events.
// Fires inside loader lock — callback must not call LoadLibrary/FreeLibrary
// or any function that may acquire the loader lock (deadlock).
// Flags: must be 0. Cookie: opaque token for LdrUnregisterDllNotification.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS LdrRegisterDllNotification(
    ULONG Flags, LDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context, PVOID *Cookie);

// Unregister a DLL notification. Cookie is the value from registration.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS LdrUnregisterDllNotification(PVOID Cookie);

//===----------------------------------------------------------------------===//
// Fiber Local Storage (FLS)
//===----------------------------------------------------------------------===//

// Process a thread's FLS data block. Flags are a bitmask:
//   RTL_FLS_DATA_CLEANUP_PER_SLOT   (1) — invoke registered callbacks
//   RTL_FLS_DATA_CLEANUP_DEALLOCATE (2) — free the data block
//
// Called by LdrShutdownProcess with flags 1|2 (invoke + free). Calling with
// flag 2 alone deallocates the block WITHOUT invoking callbacks — useful for
// fork-child and exec cleanup where callbacks point to stale code.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void RtlProcessFlsData(PVOID FlsData, ULONG Flags);

//===----------------------------------------------------------------------===//
// Dynamic Function Table (Exception Handling)
//===----------------------------------------------------------------------===//

// Register a dynamic function table for exception dispatch. The runtime
// function entries describe the unwind data (.pdata) for code at BaseAddress.
// RtlLookupFunctionEntry checks dynamic tables FIRST, before the inverted
// function table, so entries registered here take precedence over stale
// loader-registered entries.
//
// Used by self-hollowing exec to register the new image's .pdata after
// remapping at the old EXE's base address.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOLEAN
RtlAddFunctionTable(PRUNTIME_FUNCTION FunctionTable, DWORD EntryCount,
                    DWORD64 BaseAddress);

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR BOOLEAN
RtlDeleteFunctionTable(PRUNTIME_FUNCTION FunctionTable);

//===----------------------------------------------------------------------===//
// Object Query APIs
//===----------------------------------------------------------------------===//

// NtQueryObject - retrieve object metadata by information class.
// ObjectNameInformation returns the full NT namespace path of a handle,
// e.g. \Device\HarddiskVolume3\Users\user\file.txt.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryObject(HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass,
              PVOID ObjectInformation, ULONG ObjectInformationLength,
              PULONG ReturnLength);

// NtSetInformationObject — set attributes on an open handle.
// ObjectHandleFlagInformation: toggle Inherit and ProtectFromClose flags.
// ProtectFromClose prevents accidental handle closure during critical sections
// (e.g. protecting the fd table's underlying handles from close-on-exec races).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtSetInformationObject(
    HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass,
    PVOID ObjectInformation, ULONG ObjectInformationLength);

// NtCompareObjects — determine whether two handles refer to the same kernel
// object. Returns STATUS_SUCCESS if they refer to the same object,
// STATUS_NOT_SAME_OBJECT otherwise. Useful for fd deduplication and
// detecting duplicate handles inherited across fork/exec.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCompareObjects(HANDLE FirstObjectHandle, HANDLE SecondObjectHandle);

// NtMakeTemporaryObject — clear the permanent flag on a named kernel object.
// The object is then automatically deleted when the last handle is closed.
// Implements shm_unlink() semantics: the name is removed from the namespace
// but existing handles remain valid until all are closed.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtMakeTemporaryObject(HANDLE Handle);

// NtMakePermanentObject — set the permanent flag on a named kernel object.
// The object persists in the namespace even after all handles are closed,
// until explicitly made temporary again. Requires SeTcbPrivilege.
// Useful for creating persistent shared memory segments or named semaphores
// that survive process exits (shm_open without shm_unlink).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtMakePermanentObject(HANDLE Handle);

//===----------------------------------------------------------------------===//
// Private Namespaces — isolated object namespaces for sem_open/shm_open
//===----------------------------------------------------------------------===//

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR OBJECT_BOUNDARY_DESCRIPTOR *
RtlCreateBoundaryDescriptor(PCUNICODE_STRING Name, ULONG Flags);

// RtlDeleteBoundaryDescriptor — free a boundary descriptor.
// The descriptor is invalid after this call.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR void
RtlDeleteBoundaryDescriptor(OBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor);

// RtlAddSIDToBoundaryDescriptor — add a SID to the boundary.
// Only tokens containing this SID can access the private namespace.
// BoundaryDescriptor is in/out — the pointer may be reallocated.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS RtlAddSIDToBoundaryDescriptor(
    OBJECT_BOUNDARY_DESCRIPTOR **BoundaryDescriptor, PVOID RequiredSid);

// RtlAddIntegrityLabelToBoundaryDescriptor — add an integrity label to the
// boundary. Only processes with at least this integrity level can access
// the namespace.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlAddIntegrityLabelToBoundaryDescriptor(
    OBJECT_BOUNDARY_DESCRIPTOR **BoundaryDescriptor, PVOID IntegrityLabel);

// NtCreatePrivateNamespace — create a new private namespace.
// ObjectAttributes.ObjectName is the namespace name within the boundary.
// BoundaryDescriptor restricts which processes can access it.
// The returned handle must be closed with NtDeletePrivateNamespace (not
// NtClose) to properly remove the namespace from the object directory.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreatePrivateNamespace(HANDLE *NamespaceHandle, ACCESS_MASK DesiredAccess,
                         PCOBJECT_ATTRIBUTES ObjectAttributes,
                         OBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor);

// NtOpenPrivateNamespace — open an existing private namespace.
// The caller's token must satisfy the boundary descriptor's SID requirements.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenPrivateNamespace(HANDLE *NamespaceHandle, ACCESS_MASK DesiredAccess,
                       PCOBJECT_ATTRIBUTES ObjectAttributes,
                       OBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor);

// NtDeletePrivateNamespace — close and delete a private namespace handle.
// The namespace is removed from the object directory. Existing objects within
// the namespace remain accessible via open handles but cannot be opened by
// name.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtDeletePrivateNamespace(HANDLE NamespaceHandle);

//===----------------------------------------------------------------------===//
// UUID Generation — NtAllocateUuids
//===----------------------------------------------------------------------===//

// NtAllocateUuids — generate a batch of UUID material from the kernel's
// UUID generator. Produces the time, range, sequence, and seed components
// needed to construct RFC 4122 version 1 UUIDs.
//
// Time: 100-nanosecond intervals since the UUID epoch (1582-10-15).
// Range: number of UUIDs this batch covers (caller can generate this many
//        sequential UUIDs by incrementing Time).
// Sequence: clock sequence number (changes when time moves backwards or
//           the node ID changes).
// Seed: 6-byte node ID (IEEE 802 address or random), written to Seed[0..5].
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtAllocateUuids(ULARGE_INTEGER *Time,
                                                     ULONG *Range,
                                                     ULONG *Sequence,
                                                     CHAR *Seed);

//===----------------------------------------------------------------------===//
// Console subsystem registration
//===----------------------------------------------------------------------===//

// Registers the client with a CSR subsystem server. For the console
// subsystem (ServerId=1), the modern server-side decode shows an unexported
// 8-byte connect hook in basesrv consumes the payload and writes it into the
// first qword of the per-process ctrl-registration block that winsrv later
// reads via BaseGetProcessCrtlRoutine. That qword is the CtrlRoutine
// thread-entry address used later for async ctrl delivery.
//
// This is the ONLY mechanism for async ctrl event delivery — ConDrv
// IOCTLs handle I/O, but the ctrl thread injection requires this
// one-time registration at process startup.
//
// Parameters:
//   ObjectDirectory — session path, or NULL for default.
//   ServerId — subsystem index (1 = CONSRV / console).
//   ConnectionInfo — pointer to data sent to the server.
//     For the console ctrl-registration path: pointer to the CtrlRoutine
//     function address (8 bytes).
//   ConnectionInfoSize — size of ConnectionInfo in bytes (8 for CONSRV).
//   ServerToServerCall — receives TRUE if this is a server-to-server call.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS CsrClientConnectToServer(
    PWSTR ObjectDirectory, ULONG ServerId, PVOID ConnectionInfo,
    ULONG ConnectionInfoLength, BOOLEAN *ServerToServerCall);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PROCESS_API_H
