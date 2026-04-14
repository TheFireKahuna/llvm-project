//===-- NT thread, process, and system type definitions --------- *- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PROCESS_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PROCESS_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_peb.h"
#include "src/__support/OSUtil/windows/nt/nt_security_types.h"

// CONTEXT is defined in <sys/ntabi.h> (included via nt_types.h).
// PEB, TEB, RTL_USER_PROCESS_PARAMETERS are in nt_peb.h (included above).

extern "C" {

// Standard access rights (DELETE_ACCESS, READ_CONTROL, WRITE_DAC,
// WRITE_OWNER, SYNCHRONIZE, STANDARD_RIGHTS_*, GENERIC_*) are in nt_types.h.

//===----------------------------------------------------------------------===//
// Process Access Rights
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK PROCESS_TERMINATE = 0x0001;
inline constexpr ACCESS_MASK PROCESS_CREATE_THREAD = 0x0002;
inline constexpr ACCESS_MASK PROCESS_SET_SESSIONID = 0x0004;
inline constexpr ACCESS_MASK PROCESS_VM_OPERATION = 0x0008;
inline constexpr ACCESS_MASK PROCESS_VM_READ = 0x0010;
inline constexpr ACCESS_MASK PROCESS_VM_WRITE = 0x0020;
inline constexpr ACCESS_MASK PROCESS_DUP_HANDLE = 0x0040;
inline constexpr ACCESS_MASK PROCESS_CREATE_PROCESS = 0x0080;
inline constexpr ACCESS_MASK PROCESS_SET_QUOTA = 0x0100;
inline constexpr ACCESS_MASK PROCESS_SET_INFORMATION = 0x0200;
inline constexpr ACCESS_MASK PROCESS_QUERY_INFORMATION = 0x0400;
inline constexpr ACCESS_MASK PROCESS_SET_PORT = 0x0800;
inline constexpr ACCESS_MASK PROCESS_SUSPEND_RESUME = 0x0800;
inline constexpr ACCESS_MASK PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
inline constexpr ACCESS_MASK PROCESS_SET_LIMITED_INFORMATION = 0x2000;
inline constexpr ACCESS_MASK PROCESS_ALL_ACCESS =
    STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | SPECIFIC_RIGHTS_ALL;

//===----------------------------------------------------------------------===//
// Thread Access Rights
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK THREAD_TERMINATE = 0x0001;
inline constexpr ACCESS_MASK THREAD_SUSPEND_RESUME = 0x0002;
inline constexpr ACCESS_MASK THREAD_ALERT = 0x0004;
inline constexpr ACCESS_MASK THREAD_GET_CONTEXT = 0x0008;
inline constexpr ACCESS_MASK THREAD_SET_CONTEXT = 0x0010;
inline constexpr ACCESS_MASK THREAD_SET_INFORMATION = 0x0020;
inline constexpr ACCESS_MASK THREAD_QUERY_INFORMATION = 0x0040;
inline constexpr ACCESS_MASK THREAD_SET_THREAD_TOKEN = 0x0080;
inline constexpr ACCESS_MASK THREAD_IMPERSONATE = 0x0100;
inline constexpr ACCESS_MASK THREAD_DIRECT_IMPERSONATION = 0x0200;
inline constexpr ACCESS_MASK THREAD_SET_LIMITED_INFORMATION = 0x0400;
inline constexpr ACCESS_MASK THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
inline constexpr ACCESS_MASK THREAD_RESUME = 0x1000;
inline constexpr ACCESS_MASK THREAD_ALL_ACCESS =
    STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | SPECIFIC_RIGHTS_ALL;

//===----------------------------------------------------------------------===//
// Process State Change (safe suspend/resume) - Windows 11+
//===----------------------------------------------------------------------===//

// PROCESS_STATE_CHANGE_TYPE values for NtChangeProcessState.
inline constexpr ULONG ProcessStateSuspend = 0;
inline constexpr ULONG ProcessStateResume = 1;

// Access right for NtCreateProcessStateChange.
inline constexpr ACCESS_MASK PROCESS_STATE_ALL_ACCESS = 0x000F0003;

// Access right mask for state change attribute modification.
inline constexpr ULONG STATECHANGE_SET_ATTRIBUTES = 0x0001;

//===----------------------------------------------------------------------===//
// Thread State Change (safe suspend/resume) - Windows 11+
//===----------------------------------------------------------------------===//

// THREAD_STATE_CHANGE_TYPE values for NtChangeThreadState.
inline constexpr ULONG ThreadStateSuspend = 0;
inline constexpr ULONG ThreadStateResume = 1;

// Access right required for NtCreateThreadStateChange.
inline constexpr DWORD THREAD_STATE_ALL_ACCESS = 0x000F0003;

// Option flags for NtDuplicateObject.
// Close the source handle after duplication.
inline constexpr ULONG DUPLICATE_CLOSE_SOURCE = 0x00000001;
// Copy the source handle's access mask; DesiredAccess is ignored.
inline constexpr ULONG DUPLICATE_SAME_ACCESS = 0x00000002;
// Copy the source handle's attributes (inherit, protect, etc.).
inline constexpr ULONG DUPLICATE_SAME_ATTRIBUTES = 0x00000004;

// Heap manager entry points in ntdll. These back kernel32's Heap* APIs and
// let libc allocate from a private heap or the process heap without the
// kernel32 layer.

// Heap creation flags (combinable).
inline constexpr ULONG HEAP_NO_SERIALIZE = 0x00000001;
inline constexpr ULONG HEAP_GROWABLE = 0x00000002;
inline constexpr ULONG HEAP_GENERATE_EXCEPTIONS = 0x00000004;
inline constexpr ULONG HEAP_ZERO_MEMORY = 0x00000008;
inline constexpr ULONG HEAP_REALLOC_IN_PLACE_ONLY = 0x00000010;

//===----------------------------------------------------------------------===//
// Process/Thread Enumeration
//===----------------------------------------------------------------------===//

// Iterate processes system-wide without a snapshot. Pass nullptr for
// ProcessHandle to get the first process; pass the previous handle to get
// the next. The returned handle has DesiredAccess rights and must be closed
// by the caller. Returns STATUS_NO_MORE_ENTRIES when iteration is complete.
// HandleAttributes: 0 or OBJ_INHERIT.
// Flags: 0 for forward enumeration, PROCESS_GET_NEXT_FLAGS_PREVIOUS_PROCESS
// for reverse.
inline constexpr ULONG PROCESS_GET_NEXT_FLAGS_PREVIOUS_PROCESS = 0x00000001;

inline constexpr NTSTATUS STATUS_NO_MORE_ENTRIES =
    static_cast<NTSTATUS>(0x8000001A);

// Timer2 and NtQueryTimer declarations are in the "Timer APIs" section below.

//===----------------------------------------------------------------------===//
// APCs — Special user APCs can interrupt alertable waits (Win11+)
//===----------------------------------------------------------------------===//

// APC flag constants for NtQueueApcThreadEx2.
inline constexpr ULONG QUEUE_USER_APC_FLAGS_NONE = 0x00000000;
// Deliver as a special user APC — interrupts alertable waits immediately
// without waiting for the thread to enter an alertable state. Equivalent
// to passing QUEUE_USER_APC_SPECIAL_USER_APC as ReserveHandle.
inline constexpr ULONG QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC = 0x00000001;
// APC callback receives APC_CALLBACK_DATA_CONTEXT instead of raw arguments.
inline constexpr ULONG QUEUE_USER_APC_FLAGS_CALLBACK_DATA_CONTEXT = 0x00010000;

//===----------------------------------------------------------------------===//
// Thread Creation — NtCreateThreadEx
//===----------------------------------------------------------------------===//

// Thread creation flags for NtCreateThreadEx.
inline constexpr ULONG THREAD_CREATE_FLAGS_NONE = 0x00000000;
// Create the thread suspended; must be resumed with NtResumeThread.
inline constexpr ULONG THREAD_CREATE_FLAGS_CREATE_SUSPENDED = 0x00000001;
// Skip DLL_THREAD_ATTACH notifications — faster for worker threads that
// don't need DLL initialization. Not safe if loaded DLLs rely on attach.
inline constexpr ULONG THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH = 0x00000002;
// Hide from debuggers — thread won't appear in debug events.
inline constexpr ULONG THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER = 0x00000004;
// Mark as loader worker thread (Threshold+).
inline constexpr ULONG THREAD_CREATE_FLAGS_LOADER_WORKER = 0x00000010;
// Skip loader initialization (RS2+) — thread starts without acquiring
// the loader lock. Used for threads that must not trigger DLL loads.
inline constexpr ULONG THREAD_CREATE_FLAGS_SKIP_LOADER_INIT = 0x00000020;
// Allow thread creation even when the process is frozen (19H1+).
inline constexpr ULONG THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE = 0x00000040;

//===----------------------------------------------------------------------===//
// Process Creation — NtCreateProcessEx / NtCreateUserProcess
//===----------------------------------------------------------------------===//

// Additional PROCESS_CREATE_FLAGS for NtCreateProcessEx only:
inline constexpr ULONG PROCESS_CREATE_FLAGS_OVERRIDE_ADDRESS_SPACE = 0x00000008;
// Require SeLockMemoryPrivilege.
inline constexpr ULONG PROCESS_CREATE_FLAGS_LARGE_PAGES = 0x00000010;
inline constexpr ULONG PROCESS_CREATE_FLAGS_LARGE_PAGE_SYSTEM_DLL = 0x00000020;
// Create a minimal process with no PEB, no TEB, no initial thread.
inline constexpr ULONG PROCESS_CREATE_FLAGS_MINIMAL_PROCESS = 0x00000800;
// Clone the parent's address space into a minimal process (Win8.1+).
inline constexpr ULONG PROCESS_CREATE_FLAGS_CLONE_MINIMAL = 0x00002000;
// Clone with reduced commit charge.
inline constexpr ULONG PROCESS_CREATE_FLAGS_CLONE_MINIMAL_REDUCED_COMMIT =
    0x00004000;

// Process creation flags for NtCreateUserProcess.
inline constexpr ULONG PROCESS_CREATE_FLAGS_NONE = 0x00000000;
// Break away from the current job object.
inline constexpr ULONG PROCESS_CREATE_FLAGS_BREAKAWAY = 0x00000001;
// Don't inherit the debug port from the parent.
inline constexpr ULONG PROCESS_CREATE_FLAGS_NO_DEBUG_INHERIT = 0x00000002;
// Inherit handles from the parent process.
inline constexpr ULONG PROCESS_CREATE_FLAGS_INHERIT_HANDLES = 0x00000004;
// Create as a protected process (requires appropriate signing).
inline constexpr ULONG PROCESS_CREATE_FLAGS_PROTECTED_PROCESS = 0x00000040;
// Create in a new session (requires SeLoadDriverPrivilege).
inline constexpr ULONG PROCESS_CREATE_FLAGS_CREATE_SESSION = 0x00000080;
// Inherit process attributes from parent.
inline constexpr ULONG PROCESS_CREATE_FLAGS_INHERIT_FROM_PARENT = 0x00000100;
// Create the initial thread suspended.
inline constexpr ULONG PROCESS_CREATE_FLAGS_CREATE_SUSPENDED = 0x00000200;
// Force breakaway from job (requires SeTcbPrivilege).
inline constexpr ULONG PROCESS_CREATE_FLAGS_FORCE_BREAKAWAY = 0x00000400;
// Release the image section after mapping.
inline constexpr ULONG PROCESS_CREATE_FLAGS_RELEASE_SECTION = 0x00001000;
// Create as an auxiliary process (requires SeTcbPrivilege).
inline constexpr ULONG PROCESS_CREATE_FLAGS_AUXILIARY_PROCESS = 0x00008000;

// PS_ATTRIBUTE_NUM — identifies each attribute in a PS_ATTRIBUTE_LIST entry.
// Used with PsAttributeValue() to build PS_ATTRIBUTE.Attribute values.
enum PS_ATTRIBUTE_NUM : ULONG {
  PsAttributeParentProcess = 0,   // in HANDLE — parent process handle
  PsAttributeDebugObject = 1,     // in HANDLE — debug object
  PsAttributeToken = 2,           // in HANDLE — primary token
  PsAttributeClientId = 3,        // out CLIENT_ID — process/thread IDs
  PsAttributeTebAddress = 4,      // out TEB* — initial thread TEB
  PsAttributeImageName = 5,       // in PWSTR — NT image path
  PsAttributeImageInfo = 6,       // out SECTION_IMAGE_INFORMATION
  PsAttributeMemoryReserve = 7,   // in PS_MEMORY_RESERVE — pre-reserved VA
  PsAttributePriorityClass = 8,   // in UCHAR — process priority class
  PsAttributeErrorMode = 9,       // in ULONG — error mode flags
  PsAttributeStdHandleInfo = 10,  // in PS_STD_HANDLE_INFO — stdio duplication
  PsAttributeHandleList = 11,     // in HANDLE[] — inheritable handles
  PsAttributeGroupAffinity = 12,  // in GROUP_AFFINITY — processor group
  PsAttributePreferredNode = 13,  // in USHORT — preferred NUMA node
  PsAttributeIdealProcessor = 14, // in PROCESSOR_NUMBER — ideal processor
  PsAttributeMitigationOptions = 16, // in mitigation options map (Win8+)
  PsAttributeProtectionLevel = 17,   // in PS_PROTECTION (Win8.1+)
  PsAttributeSecureProcess = 18, // in trustlet create attributes (Threshold+)
  PsAttributeJobList = 19,       // in HANDLE[] — job objects
  PsAttributeChildProcessPolicy = 20,           // in ULONG (Threshold2+)
  PsAttributeAllApplicationPackagesPolicy = 21, // in ULONG (RS1+)
  PsAttributeMachineType = 28, // in USHORT — machine architecture (21H2+)
  PsAttributeMax
};

// PS_ATTRIBUTE flag bits — combined with PS_ATTRIBUTE_NUM to form
// the Attribute field in a PS_ATTRIBUTE entry.
inline constexpr ULONG_PTR PS_ATTRIBUTE_NUMBER_MASK = 0x0000FFFF;
inline constexpr ULONG_PTR PS_ATTRIBUTE_THREAD =
    0x00010000; // Valid for thread creation
inline constexpr ULONG_PTR PS_ATTRIBUTE_INPUT = 0x00020000; // Input attribute
inline constexpr ULONG_PTR PS_ATTRIBUTE_ADDITIVE =
    0x00040000; // Additive (accumulated)

// Build a PS_ATTRIBUTE.Attribute value from components.
inline constexpr ULONG_PTR PsAttributeValue(ULONG Number, bool Thread,
                                            bool Input, bool Additive) {
  return (Number & PS_ATTRIBUTE_NUMBER_MASK) |
         (Thread ? PS_ATTRIBUTE_THREAD : 0) | (Input ? PS_ATTRIBUTE_INPUT : 0) |
         (Additive ? PS_ATTRIBUTE_ADDITIVE : 0);
}

// Common pre-built attribute values.
inline constexpr ULONG_PTR PS_ATTRIBUTE_PARENT_PROCESS =
    PsAttributeValue(PsAttributeParentProcess, false, true, true);
inline constexpr ULONG_PTR PS_ATTRIBUTE_TOKEN =
    PsAttributeValue(PsAttributeToken, false, true, true);
inline constexpr ULONG_PTR PS_ATTRIBUTE_IMAGE_NAME =
    PsAttributeValue(PsAttributeImageName, false, true, false);
inline constexpr ULONG_PTR PS_ATTRIBUTE_CLIENT_ID =
    PsAttributeValue(PsAttributeClientId, true, false, false);
inline constexpr ULONG_PTR PS_ATTRIBUTE_TEB_ADDRESS =
    PsAttributeValue(PsAttributeTebAddress, true, false, false);
inline constexpr ULONG_PTR PS_ATTRIBUTE_PRIORITY_CLASS =
    PsAttributeValue(PsAttributePriorityClass, false, true, false);
inline constexpr ULONG_PTR PS_ATTRIBUTE_HANDLE_LIST =
    PsAttributeValue(PsAttributeHandleList, false, true, false);
inline constexpr ULONG_PTR PS_ATTRIBUTE_STD_HANDLE_INFO =
    PsAttributeValue(PsAttributeStdHandleInfo, false, true, false);
inline constexpr ULONG_PTR PS_ATTRIBUTE_IMAGE_INFO =
    PsAttributeValue(PsAttributeImageInfo, false, false, false);

// PS_ATTRIBUTE — single attribute in a PS_ATTRIBUTE_LIST.
struct PS_ATTRIBUTE {
  ULONG_PTR Attribute; // PS_ATTRIBUTE_* value
  SIZE_T Size;         // Size of the value in bytes
  union {
    ULONG_PTR Value; // Scalar value (handle, flags)
    PVOID ValuePtr;  // Pointer to value buffer
  };
  SIZE_T *ReturnLength; // Optional — receives actual size written
};

// PS_ATTRIBUTE_LIST — variable-length array of PS_ATTRIBUTE entries.
// TotalLength must be set to sizeof(SIZE_T) + N * sizeof(PS_ATTRIBUTE).
struct PS_ATTRIBUTE_LIST {
  SIZE_T TotalLength;
  PS_ATTRIBUTE Attributes[1]; // Variable-length
};

// PS_STD_HANDLE_STATE — controls standard handle duplication behavior.
enum PS_STD_HANDLE_STATE : ULONG {
  PsNeverDuplicate = 0,   // Never duplicate std handles
  PsRequestDuplicate = 1, // Duplicate if subsystem type matches
  PsAlwaysDuplicate = 2,  // Always duplicate std handles
};

inline constexpr ULONG PS_STD_INPUT_HANDLE = 0x1;
inline constexpr ULONG PS_STD_OUTPUT_HANDLE = 0x2;
inline constexpr ULONG PS_STD_ERROR_HANDLE = 0x4;

// PS_STD_HANDLE_INFO — controls which standard handles to duplicate into
// the new process. Passed via PsAttributeStdHandleInfo.
struct PS_STD_HANDLE_INFO {
  union {
    ULONG Flags;
    struct {
      ULONG StdHandleState : 2;   // PS_STD_HANDLE_STATE
      ULONG PseudoHandleMask : 3; // PS_STD_*_HANDLE bitmask
    };
  };
  ULONG StdHandleSubsystemType;
};

// PS_MEMORY_RESERVE — pre-reserve a VA region in the new process.
// Passed via PsAttributeMemoryReserve.
struct PS_MEMORY_RESERVE {
  PVOID ReserveAddress;
  SIZE_T ReserveSize;
};

// PS_CREATE_STATE — indicates the stage at which process creation completed
// or failed. Returned in PS_CREATE_INFO.State.
enum PS_CREATE_STATE : ULONG {
  PsCreateInitialState = 0,
  PsCreateFailOnFileOpen = 1,
  PsCreateFailOnSectionCreate = 2,
  PsCreateFailExeFormat = 3,
  PsCreateFailMachineMismatch = 4,
  PsCreateFailExeName = 5, // Debugger-specified IFEO redirect
  PsCreateSuccess = 6,
  PsCreateMaximumStates
};

// PS_CREATE_INFO — in/out parameter for NtCreateUserProcess describing
// creation options (input) and results (output). Size must be set before
// the call. State receives the outcome. On PsCreateSuccess, the SuccessState
// union contains handles, PEB address, and manifest info.
struct PS_CREATE_INFO {
  SIZE_T Size;
  PS_CREATE_STATE State;
  union {
    // Input: PsCreateInitialState
    struct {
      union {
        ULONG InitFlags;
        struct {
          UCHAR WriteOutputOnExit : 1;
          UCHAR DetectManifest : 1;
          UCHAR IFEOSkipDebugger : 1;
          UCHAR IFEODoNotPropagateKeyState : 1;
          UCHAR SpareBits1 : 4;
          UCHAR SpareBits2 : 8;
          USHORT ProhibitedImageCharacteristics : 16;
        };
      };
      ACCESS_MASK AdditionalFileAccess;
    } InitState;

    // Failure: PsCreateFailOnSectionCreate
    struct {
      HANDLE FileHandle;
    } FailSection;

    // Failure: PsCreateFailExeFormat
    struct {
      USHORT DllCharacteristics;
    } ExeFormat;

    // Failure: PsCreateFailExeName (IFEO redirect)
    struct {
      HANDLE IFEOKey;
    } ExeName;

    // Success: PsCreateSuccess
    struct {
      union {
        ULONG OutputFlags;
        struct {
          UCHAR ProtectedProcess : 1;
          UCHAR AddressSpaceOverride : 1;
          UCHAR DevOverrideEnabled : 1;
          UCHAR ManifestDetected : 1;
          UCHAR ProtectedProcessLight : 1;
          UCHAR SpareBits1 : 3;
          UCHAR SpareBits2 : 8;
          USHORT SpareBits3 : 16;
        };
      };
      HANDLE FileHandle;                     // Image file handle
      HANDLE SectionHandle;                  // Image section handle
      ULONGLONG UserProcessParametersNative; // RTL_USER_PROCESS_PARAMETERS VA
                                             // (native)
      ULONG
          UserProcessParametersWow64; // RTL_USER_PROCESS_PARAMETERS VA (WOW64)
      ULONG CurrentParameterFlags;
      ULONGLONG PebAddressNative; // PEB address (native)
      ULONG PebAddressWow64;      // PEB address (WOW64)
      ULONGLONG ManifestAddress;  // Address of detected manifest
      ULONG ManifestSize;         // Size of manifest in bytes
    } SuccessState;
  };
};

//===----------------------------------------------------------------------===//
// Process/Thread Information Queries
//===----------------------------------------------------------------------===//

// THREADINFOCLASS — selects which attribute NtQueryInformationThread /
// NtSetInformationThread operates on. "q" = query, "s" = set, "qs" = both.
// Reference: phnt ntpsapi.h (System Informer)
inline constexpr ULONG ThreadBasicInformation =
    0; // q: THREAD_BASIC_INFORMATION — exit status, TEB, client ID, priority
inline constexpr ULONG ThreadTimes =
    1; // q: KERNEL_USER_TIMES — creation, exit, kernel, user times (100ns)
inline constexpr ULONG ThreadBasePriority =
    3; // s: KPRIORITY — base priority increment (-2..+2, 15=TIME_CRITICAL)
inline constexpr ULONG ThreadAffinityMask =
    4; // s: KAFFINITY — single-group only, deprecated by ThreadGroupInformation
inline constexpr ULONG ThreadGroupInformation =
    30; // qs: GROUP_AFFINITY — thread's processor group + affinity mask (one
        // group)
inline constexpr ULONG ThreadIdealProcessorEx =
    33; // qs: PROCESSOR_NUMBER — preferred processor (Group, Number); returns
        // previous
inline constexpr ULONG ThreadSuspendCount =
    35; // q: ULONG — current suspend count (since Win8.1/BLUE)
inline constexpr ULONG ThreadNameInformation =
    38; // qs: THREAD_NAME_INFORMATION — SetThreadDescription name
        // (THREAD_SET_LIMITED_INFORMATION)
inline constexpr ULONG ThreadSelectedCpuSets =
    39; // qs: ULONG[] — array of CPU set IDs; empty = unrestricted (since Win10
        // TH2)
inline constexpr ULONG ThreadActualGroupAffinity =
    41; // q: GROUP_AFFINITY — group the thread is actually running in (since
        // Win10 TH2)

// Returned by NtQueryInformationThread(ThreadTimes) and
// NtQueryInformationProcess(ProcessTimes). Kernel+User = total CPU time.
struct KERNEL_USER_TIMES {
  LARGE_INTEGER CreateTime; // Absolute time (100ns since 1601-01-01)
  LARGE_INTEGER ExitTime;   // 0 if process/thread still running
  LARGE_INTEGER KernelTime; // Cumulative kernel-mode time (100ns units)
  LARGE_INTEGER UserTime;   // Cumulative user-mode time (100ns units)
};

// VM_COUNTERS is now in <sys/ntabi.h>.

// Returned by NtQueryInformationThread(ThreadBasicInformation).
struct THREAD_BASIC_INFORMATION {
  NTSTATUS ExitStatus;    // STATUS_PENDING if thread still running
  PVOID TebBaseAddress;   // Thread Environment Block base address
  CLIENT_ID ClientId;     // Process ID + Thread ID
  ULONG_PTR AffinityMask; // Processor affinity (deprecated, use GROUP_AFFINITY)
  KPRIORITY Priority;     // Current (effective) priority (0-31)
  KPRIORITY BasePriority; // Base priority determined by thread + process class
};

// PROCESSINFOCLASS — selects which attribute NtQueryInformationProcess /
// NtSetInformationProcess operates on. "q" = query, "s" = set, "qs" = both.
// Reference: phnt ntpsapi.h (System Informer)
inline constexpr ULONG ProcessBasicInformation =
    0; // q: PROCESS_BASIC_INFORMATION — PEB, PID, parent PID, exit status
inline constexpr ULONG ProcessIoCounters =
    2; // q: IO_COUNTERS — read/write/other operation and byte counts
inline constexpr ULONG ProcessVmCounters =
    3; // q: VM_COUNTERS — virtual memory statistics (peak, working set, etc.)
inline constexpr ULONG ProcessTimes =
    4; // q: KERNEL_USER_TIMES — creation, exit, kernel, user times
inline constexpr ULONG ProcessDebugPort =
    7; // q: HANDLE — non-zero if process is being debugged
inline constexpr ULONG ProcessDefaultHardErrorMode =
    12; // qs: ULONG — SEM_* bitmask controlling error dialog suppression
inline constexpr ULONG ProcessPriorityClass =
    18; // qs: PROCESS_PRIORITY_CLASS — scheduling priority class + foreground
        // boost

// Hard error mode flags (ProcessDefaultHardErrorMode). Same values as
// the Win32 SEM_* constants — the kernel stores them directly in EPROCESS.
inline constexpr ULONG SEM_NOGPFAULTERRORBOX =
    0x0002; // Suppress WER crash dialog
inline constexpr ULONG ProcessWow64Information =
    26; // q: ULONG_PTR — WOW64 PEB address (0 if native 64-bit process)
inline constexpr ULONG ProcessImageFileName =
    27; // q: UNICODE_STRING — NT-style image path (\Device\HarddiskVolume...)
inline constexpr ULONG ProcessBreakOnTermination =
    29; // qs: ULONG — if 1, termination triggers BSOD (requires SeTcbPrivilege)
inline constexpr ULONG ProcessGroupInformation =
    47; // q: USHORT[] — processor group numbers the process spans (ReturnLength
        // / 2 = count)
inline constexpr ULONG ProcessConsoleHostProcess =
    49; // qs: ULONG_PTR — console-host affinity cookie used by the modern
        // console publish path
inline constexpr ULONG ProcessCommandLineInformation =
    60; // q: UNICODE_STRING — full command line (since Win8.1/BLUE)
inline constexpr ULONG ProcessImageSection =
    89; // q: HANDLE — section object backing the process image (since Win11)

// ProcessExecuteFlags (34) — query or set per-process DEP (NX) policy.
// Buffer is a single ULONG containing MEM_EXECUTE_OPTION_* flags.
inline constexpr ULONG ProcessExecuteFlags =
    34; // qs: ULONG (MEM_EXECUTE_OPTION_*) — DEP / NX enforcement

// MEM_EXECUTE_OPTION_* flags for ProcessExecuteFlags.
inline constexpr ULONG MEM_EXECUTE_OPTION_DISABLE = 0x1;
inline constexpr ULONG MEM_EXECUTE_OPTION_ENABLE = 0x2;
inline constexpr ULONG MEM_EXECUTE_OPTION_DISABLE_THUNK_EMULATION = 0x4;
inline constexpr ULONG MEM_EXECUTE_OPTION_PERMANENT = 0x8;

// ProcessMitigationPolicy (52) — query or set per-process exploit mitigations.
// Buffer is PROCESS_MITIGATION_POLICY_INFORMATION (policy class + union body).
// DEP is NOT handled through this class — use ProcessExecuteFlags instead.
// s: requires PROCESS_SET_INFORMATION; q: requires PROCESS_QUERY_INFORMATION.
inline constexpr ULONG ProcessMitigationPolicy =
    52; // qs: PROCESS_MITIGATION_POLICY_INFORMATION — per-process exploit
        // mitigations (ASLR, CFG, CET, ACG, image load, etc.)

//===----------------------------------------------------------------------===//
// Process Mitigation Policy Structures
//===----------------------------------------------------------------------===//

// PROCESS_MITIGATION_POLICY — selects which mitigation to query/set via
// NtSetInformationProcess(ProcessMitigationPolicy, ...).
// Reference: phnt ntpsapi.h, Windows SDK <winnt.h>
enum PROCESS_MITIGATION_POLICY_ENUM : ULONG {
  ProcessDEPPolicyClass = 0,
  ProcessASLRPolicyClass = 1,
  ProcessDynamicCodePolicyClass = 2,
  ProcessStrictHandleCheckPolicyClass = 3,
  ProcessSystemCallDisablePolicyClass = 4,
  ProcessMitigationOptionsMaskClass = 5,
  ProcessExtensionPointDisablePolicyClass = 6,
  ProcessControlFlowGuardPolicyClass = 7,
  ProcessSignaturePolicyClass = 8,
  ProcessFontDisablePolicyClass = 9,
  ProcessImageLoadPolicyClass = 10,
  ProcessSystemCallFilterPolicyClass = 11,
  ProcessPayloadRestrictionPolicyClass = 12,
  ProcessChildProcessPolicyClass = 13,
  ProcessSideChannelIsolationPolicyClass = 14,
  ProcessUserShadowStackPolicyClass = 15,      // 20H1+
  ProcessRedirectionTrustPolicyClass = 16,      // 22H1+
  ProcessUserPointerAuthPolicyClass = 17,       // ARM64 PA
  ProcessSEHOPPolicyClass = 18,                 // 24H2+
  ProcessActivationContextTrustPolicyClass = 19,
  MaxProcessMitigationPolicyClass = 20,
};

// Individual policy structures. Each is a 4-byte union (DWORD Flags + bitfield).
// These match the Windows SDK <winnt.h> definitions exactly.
// Note: DEP is not here — it uses ProcessExecuteFlags (class 34), not the
// ProcessMitigationPolicy (class 52) envelope.

struct PROCESS_MITIGATION_ASLR_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG EnableBottomUpRandomization : 1;
      ULONG EnableForceRelocateImages : 1;
      ULONG EnableHighEntropy : 1;
      ULONG DisallowStrippedImages : 1;
      ULONG ReservedFlags : 28;
    };
  };
};

struct PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG RaiseExceptionOnInvalidHandleReference : 1;
      ULONG HandleExceptionsPermanentlyEnabled : 1;
      ULONG ReservedFlags : 30;
    };
  };
};

struct PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG DisableExtensionPoints : 1;
      ULONG ReservedFlags : 31;
    };
  };
};

struct PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG EnableControlFlowGuard : 1;
      ULONG EnableExportSuppression : 1;
      ULONG StrictMode : 1;
      ULONG EnableXfg : 1;
      ULONG EnableXfgAuditMode : 1;
      ULONG ReservedFlags : 27;
    };
  };
};

struct PROCESS_MITIGATION_IMAGE_LOAD_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG NoRemoteImages : 1;
      ULONG NoLowMandatoryLabelImages : 1;
      ULONG PreferSystem32Images : 1;
      ULONG AuditNoRemoteImages : 1;
      ULONG AuditNoLowMandatoryLabelImages : 1;
      ULONG ReservedFlags : 27;
    };
  };
};

struct PROCESS_MITIGATION_SIDE_CHANNEL_ISOLATION_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG SmtBranchTargetIsolation : 1;
      ULONG IsolateSecurityDomain : 1;
      ULONG DisablePageCombine : 1;
      ULONG SpeculativeStoreBypassDisable : 1;
      ULONG RestrictCoreSharing : 1;
      ULONG ReservedFlags : 27;
    };
  };
};

struct PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY {
  union {
    ULONG Flags;
    struct {
      ULONG EnableUserShadowStack : 1;
      ULONG AuditUserShadowStack : 1;
      ULONG SetContextIpValidation : 1;
      ULONG AuditSetContextIpValidation : 1;
      ULONG EnableUserShadowStackStrictMode : 1;
      ULONG BlockNonCetBinaries : 1;
      ULONG BlockNonCetBinariesNonEhcont : 1;
      ULONG AuditBlockNonCetBinaries : 1;
      ULONG CetDynamicApisOutOfProcOnly : 1;
      ULONG SetContextIpValidationRelaxedMode : 1;
      ULONG ReservedFlags : 22;
    };
  };
};

// Envelope struct for NtSetInformationProcess(ProcessMitigationPolicy).
// Caller sets Policy to the desired class, then fills the corresponding
// union member.
struct PROCESS_MITIGATION_POLICY_INFORMATION {
  PROCESS_MITIGATION_POLICY_ENUM Policy;
  union {
    PROCESS_MITIGATION_ASLR_POLICY ASLRPolicy;
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY StrictHandleCheckPolicy;
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY
        ExtensionPointDisablePolicy;
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY ControlFlowGuardPolicy;
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY ImageLoadPolicy;
    PROCESS_MITIGATION_SIDE_CHANNEL_ISOLATION_POLICY
        SideChannelIsolationPolicy;
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY UserShadowStackPolicy;
  };
};

// Priority class values for PROCESS_PRIORITY_CLASS::PriorityClass.
// These map to the scheduling base priority that the kernel assigns to
// threads within the process. REALTIME requires
// SeIncreaseBasePriorityPrivilege.
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_UNKNOWN = 0;
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_IDLE = 1;
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_NORMAL = 2;
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_HIGH = 3;
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_REALTIME = 4;
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_BELOW_NORMAL = 5;
inline constexpr UCHAR PROCESS_PRIORITY_CLASS_ABOVE_NORMAL = 6;

// Returned/passed by NtQueryInformationProcess(ProcessPriorityClass).
struct PROCESS_PRIORITY_CLASS {
  BOOLEAN Foreground;  // TRUE if process is foreground-boosted
  UCHAR PriorityClass; // PROCESS_PRIORITY_CLASS_* value
};

// Returned by NtQueryInformationProcess(ProcessBasicInformation).
struct PROCESS_BASIC_INFORMATION {
  NTSTATUS ExitStatus;    // STATUS_PENDING if process still running
  PVOID PebBaseAddress;   // Process Environment Block pointer
  ULONG_PTR AffinityMask; // Processor affinity (deprecated, use GROUP_AFFINITY)
  LONG BasePriority;      // Base priority determined by priority class
  ULONG_PTR UniqueProcessId; // The PID (same as GetCurrentProcessId)
  ULONG_PTR InheritedFromUniqueProcessId; // Parent PID
};

//===----------------------------------------------------------------------===//
// System Information — NtQuerySystemInformation
//===----------------------------------------------------------------------===//

// SYSTEM_INFORMATION_CLASS — selects which system attribute
// NtQuerySystemInformation / NtQuerySystemInformationEx retrieves.
// Reference: phnt ntexapi.h (System Informer)
inline constexpr ULONG SystemBasicInformation =
    0; // q: SYSTEM_BASIC_INFORMATION — page size, processor count, physical
       // memory
inline constexpr ULONG SystemPerformanceInformation =
    2; // q: SYSTEM_PERFORMANCE_INFORMATION — commit, pool, page fault stats
inline constexpr ULONG SystemHandleInformation =
    0x10; // q: SYSTEM_HANDLE_INFORMATION — per-handle entries (deprecated, use
          // 0x40)
inline constexpr ULONG SystemObjectInformation =
    0x11; // q: SYSTEM_OBJECTTYPE_INFORMATION — object type + object entries
          // interleaved
inline constexpr ULONG SystemPagefileInformation =
    0x12; // q: SYSTEM_PAGEFILE_INFORMATION — pagefile paths, sizes, usage
inline constexpr ULONG SystemFileCacheInformation =
    0x15; // qs: SYSTEM_FILECACHE_INFORMATION — file cache working set limits
inline constexpr ULONG SystemPoolTagInformation =
    0x16; // q: SYSTEM_POOLTAG_INFORMATION — per-tag pool allocation stats
inline constexpr ULONG SystemInterruptInformation =
    0x17; // q: SYSTEM_INTERRUPT_INFORMATION — per-processor interrupt counts
inline constexpr ULONG SystemDpcBehaviorInformation =
    0x18; // qs: SYSTEM_DPC_BEHAVIOR_INFORMATION — DPC timeout and threading
          // config
inline constexpr ULONG SystemTimeAdjustmentInformation =
    0x1c; // qs: SYSTEM_QUERY_TIME_ADJUST_INFORMATION — clock tick adjustment
inline constexpr ULONG SystemNumaProcessorMap =
    0x37; // q: SYSTEM_NUMA_INFORMATION — NUMA node ↔ processor group mapping
inline constexpr ULONG SystemExtendedHandleInformation =
    0x40; // q: SYSTEM_HANDLE_INFORMATION_EX — 64-bit-safe handle enumeration

// Returned by NtQuerySystemInformation(SystemBasicInformation).
// Reference: System Informer phnt/include/ntexapi.h
struct SYSTEM_BASIC_INFORMATION {
  ULONG Reserved;        // Formerly TickCountLowDeprecated
  ULONG TimerResolution; // Clock interrupt period (100ns units, typically
                         // 156250 = 15.625ms)
  ULONG
      PageSize; // Base page size in bytes (4096 on x64, 4096/16384 on AArch64)
  ULONG NumberOfPhysicalPages; // Total physical page frames (PageSize * this =
                               // RAM)
  ULONG LowestPhysicalPageNumber;   // Lowest PFN (typically 1)
  ULONG HighestPhysicalPageNumber;  // Highest PFN
  ULONG AllocationGranularity;      // VirtualAlloc alignment (65536 = 64KB)
  ULONG_PTR MinimumUserModeAddress; // Lowest user-mode VA (0x10000)
  ULONG_PTR
      MaximumUserModeAddress; // Highest user-mode VA (0x7FFFFFFEFFFF on x64)
  ULONG_PTR ActiveProcessorsAffinityMask; // Bitmask of active CPUs (deprecated,
                                          // use group affinity)
  UCHAR NumberOfProcessors;               // Active logical processors
};

// Returned by NtQuerySystemInformation(SystemHandleInformation).
// Deprecated: UniqueProcessId is USHORT, limiting to PID < 65536.
// Use SystemExtendedHandleInformation (0x40) for production code.
struct SYSTEM_HANDLE_TABLE_ENTRY_INFO {
  USHORT UniqueProcessId;
  USHORT CreatorBackTraceIndex;
  UCHAR ObjectTypeIndex;
  UCHAR HandleAttributes;
  USHORT HandleValue;
  PVOID Object;
  ACCESS_MASK GrantedAccess;
};

struct SYSTEM_HANDLE_INFORMATION {
  ULONG NumberOfHandles;
  SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1]; // variable-length
};

// Returned by NtQuerySystemInformation(SystemObjectInformation).
// Packed array: walk by NextEntryOffset. Each SYSTEM_OBJECTTYPE_INFORMATION
// is followed by its SYSTEM_OBJECT_INFORMATION entries.
struct SYSTEM_OBJECTTYPE_INFORMATION {
  ULONG NextEntryOffset;
  ULONG NumberOfObjects;
  ULONG NumberOfHandles;
  ULONG TypeIndex;
  ULONG InvalidAttributes;
  GENERIC_MAPPING GenericMapping;
  ACCESS_MASK ValidAccessMask;
  ULONG PoolType;
  BOOLEAN SecurityRequired;
  BOOLEAN WaitableObject;
  UNICODE_STRING TypeName;
};

struct SYSTEM_OBJECT_INFORMATION {
  ULONG NextEntryOffset;
  PVOID Object;
  HANDLE CreatorUniqueProcess;
  USHORT CreatorBackTraceIndex;
  USHORT Flags;
  LONG PointerCount;
  LONG HandleCount;
  ULONG PagedPoolCharge;
  ULONG NonPagedPoolCharge;
  HANDLE ExclusiveProcessId;
  PVOID SecurityDescriptor; // PSECURITY_DESCRIPTOR
  UNICODE_STRING NameInfo;
};

// Returned by NtQuerySystemInformation(SystemPagefileInformation).
// Packed array: walk by NextEntryOffset (0 = last entry).
struct SYSTEM_PAGEFILE_INFORMATION {
  ULONG NextEntryOffset;
  ULONG TotalSize;
  ULONG TotalInUse;
  ULONG PeakUsage;
  UNICODE_STRING PageFileName;
};

// Returned by NtQuerySystemInformation(SystemFileCacheInformation).
struct SYSTEM_FILECACHE_INFORMATION {
  SIZE_T CurrentSize;
  SIZE_T PeakSize;
  ULONG PageFaultCount;
  SIZE_T MinimumWorkingSet;
  SIZE_T MaximumWorkingSet;
  SIZE_T CurrentSizeIncludingTransitionInPages;
  SIZE_T PeakSizeIncludingTransitionInPages;
  ULONG TransitionRePurposeCount;
  ULONG Flags;
};

// Subset of SYSTEM_FILECACHE_INFORMATION — first three fields only.
struct SYSTEM_BASIC_WORKING_SET_INFORMATION {
  SIZE_T CurrentSize;
  SIZE_T PeakSize;
  ULONG PageFaultCount;
};

// Returned by NtQuerySystemInformation(SystemPoolTagInformation).
struct SYSTEM_POOLTAG {
  union {
    UCHAR Tag[4];
    ULONG TagUlong;
  };
  ULONG PagedAllocs;
  ULONG PagedFrees;
  SIZE_T PagedUsed;
  ULONG NonPagedAllocs;
  ULONG NonPagedFrees;
  SIZE_T NonPagedUsed;
};

struct SYSTEM_POOLTAG_INFORMATION {
  ULONG Count;
  SYSTEM_POOLTAG TagInfo[1]; // variable-length
};

// Returned by NtQuerySystemInformation(SystemInterruptInformation).
// One entry per processor.
struct SYSTEM_INTERRUPT_INFORMATION {
  ULONG ContextSwitches;
  ULONG DpcCount;
  ULONG DpcRate;
  ULONG TimeIncrement;
  ULONG DpcBypassCount;
  ULONG ApcBypassCount;
};

// NtQuerySystemInformation(SystemDpcBehaviorInformation).
struct SYSTEM_DPC_BEHAVIOR_INFORMATION {
  ULONG Spare;
  ULONG DpcQueueDepth;
  ULONG MinimumDpcRate;
  ULONG AdjustDpcThreshold;
  ULONG IdealDpcRate;
};

// NtQuerySystemInformation(SystemTimeAdjustmentInformation) — query variants.
// The kernel distinguishes 32-bit vs 64-bit layout by buffer size.
struct SYSTEM_QUERY_TIME_ADJUST_INFORMATION {
  ULONG TimeAdjustment;
  ULONG TimeIncrement;
  BOOLEAN Enable;
};

// 64-bit precision variant — use this on 64-bit systems.
struct SYSTEM_QUERY_TIME_ADJUST_INFORMATION_PRECISE {
  ULONGLONG
      TimeAdjustment; // Clock tick size applied each interrupt (100ns units)
  ULONGLONG TimeIncrement; // Nominal clock interrupt period (100ns units)
  BOOLEAN Enable;
};

// NtSetInformationSystem(SystemTimeAdjustmentInformation) — set variants.
struct SYSTEM_SET_TIME_ADJUST_INFORMATION {
  ULONG TimeAdjustment;
  BOOLEAN Enable;
};

struct SYSTEM_SET_TIME_ADJUST_INFORMATION_PRECISE {
  ULONGLONG TimeAdjustment;
  BOOLEAN Enable;
};

// Returned by NtQuerySystemInformation(SystemPerformanceInformation).
// The full structure is required; NtQuerySystemInformation returns
// STATUS_INFO_LENGTH_MISMATCH if the buffer is smaller than expected.
// Reference: System Informer phnt/include/ntexapi.h
struct SYSTEM_PERFORMANCE_INFORMATION {
  // CPU idle time
  LARGE_INTEGER
      IdleProcessTime; // Total idle time across all CPUs (100ns units)
  // I/O transfer counts
  LARGE_INTEGER IoReadTransferCount;  // Bytes read by all I/O operations
  LARGE_INTEGER IoWriteTransferCount; // Bytes written by all I/O operations
  LARGE_INTEGER IoOtherTransferCount; // Bytes transferred by non-read/write I/O
  ULONG IoReadOperationCount;         // Number of I/O read operations
  ULONG IoWriteOperationCount;        // Number of I/O write operations
  ULONG IoOtherOperationCount;        // Number of non-read/write I/O operations
  // Memory manager counters (values in pages, multiply by PageSize for bytes)
  ULONG AvailablePages; // Pages available for allocation
  ULONG CommittedPages; // Total committed pages (RAM + pagefile backing)
  ULONG CommitLimit;    // Maximum committable pages before pagefile exhaustion
  ULONG PeakCommitment; // High-water mark of CommittedPages
  ULONG PageFaultCount; // Total page faults (hard + soft)
  ULONG CopyOnWriteCount; // COW page faults resolved
  ULONG TransitionCount;  // Soft faults resolved from standby/modified lists
  ULONG CacheTransitionCount;  // Cache-manager transition faults
  ULONG DemandZeroCount;       // Demand-zero page faults
  ULONG PageReadCount;         // Pages read from disk (hard faults)
  ULONG PageReadIoCount;       // I/O operations for page reads
  ULONG CacheReadCount;        // Cache manager pages read
  ULONG CacheIoCount;          // Cache manager I/O operations
  ULONG DirtyPagesWriteCount;  // Modified pages written to disk
  ULONG DirtyWriteIoCount;     // I/O operations for dirty page writes
  ULONG MappedPagesWriteCount; // Mapped file pages written
  ULONG MappedWriteIoCount;    // I/O operations for mapped writes
  // Pool usage (values in pages)
  ULONG PagedPoolPages;     // Paged pool pages in use
  ULONG NonPagedPoolPages;  // Non-paged pool pages in use
  ULONG PagedPoolAllocs;    // Cumulative paged pool allocations
  ULONG PagedPoolFrees;     // Cumulative paged pool frees
  ULONG NonPagedPoolAllocs; // Cumulative non-paged pool allocations
  ULONG NonPagedPoolFrees;  // Cumulative non-paged pool frees
  // System resource counters
  ULONG FreeSystemPtes;         // Free system page table entries
  ULONG ResidentSystemCodePage; // Resident pages of system code (ntoskrnl, HAL)
  ULONG TotalSystemDriverPages; // Total driver pages (resident + paged out)
  ULONG TotalSystemCodePages;   // Total system code pages
  ULONG NonPagedPoolLookasideHits; // Lookaside list hits (non-paged)
  ULONG PagedPoolLookasideHits;    // Lookaside list hits (paged)
  ULONG AvailablePagedPoolPages;   // Free paged pool pages
  ULONG ResidentSystemCachePage;   // Resident system cache pages
  ULONG ResidentPagedPoolPage;     // Resident paged pool pages
  ULONG ResidentSystemDriverPage;  // Resident driver pages
  // Cache manager (Cc) fast-path counters
  ULONG CcFastReadNoWait;
  ULONG CcFastReadWait;
  ULONG CcFastReadResourceMiss;
  ULONG CcFastReadNotPossible;
  ULONG CcFastMdlReadNoWait;
  ULONG CcFastMdlReadWait;
  ULONG CcFastMdlReadResourceMiss;
  ULONG CcFastMdlReadNotPossible;
  ULONG CcMapDataNoWait;
  ULONG CcMapDataWait;
  ULONG CcMapDataNoWaitMiss;
  ULONG CcMapDataWaitMiss;
  ULONG CcPinMappedDataCount;
  ULONG CcPinReadNoWait;
  ULONG CcPinReadWait;
  ULONG CcPinReadNoWaitMiss;
  ULONG CcPinReadWaitMiss;
  ULONG CcCopyReadNoWait;
  ULONG CcCopyReadWait;
  ULONG CcCopyReadNoWaitMiss;
  ULONG CcCopyReadWaitMiss;
  ULONG CcMdlReadNoWait;
  ULONG CcMdlReadWait;
  ULONG CcMdlReadNoWaitMiss;
  ULONG CcMdlReadWaitMiss;
  ULONG CcReadAheadIos;
  ULONG CcLazyWriteIos;
  ULONG CcLazyWritePages;
  ULONG CcDataFlushes;
  ULONG CcDataPages;
  // Scheduling and TLB counters
  ULONG ContextSwitches;    // Total context switches
  ULONG FirstLevelTbFills;  // First-level TLB fills
  ULONG SecondLevelTbFills; // Second-level TLB fills
  ULONG SystemCalls;        // Total system calls
  // Extended dirty page tracking (64-bit, Win8+)
  ULONGLONG CcTotalDirtyPages;     // Current dirty pages in cache
  ULONGLONG CcDirtyPageThreshold;  // Threshold triggering lazy writer
  LONGLONG ResidentAvailablePages; // Available pages including standby (signed)
  ULONGLONG SharedCommittedPages;  // Committed pages in shared sections
};

//===----------------------------------------------------------------------===//
// Logical Processor / Cache Topology — SystemLogicalProcessorInformationEx
//===----------------------------------------------------------------------===//

// SystemLogicalProcessorAndGroupInformation (class 73) — returns a packed
// variable-length array of SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX entries.
// Walk entries via entry->Size. Unlike class 11, supports >64 processors
// across multiple processor groups. Query via NtQuerySystemInformationEx
// with a LOGICAL_PROCESSOR_RELATIONSHIP filter as the input buffer
// (RelationGroup, RelationNumaNode, RelationProcessorCore, etc.).
inline constexpr ULONG SystemLogicalProcessorInformationEx = 73;

// SystemCpuSetInformation (class 175) — returns one SYSTEM_CPU_SET_INFORMATION
// per logical processor, each containing the CPU set ID, processor group,
// group-relative index, core/cache/NUMA topology, efficiency class, and
// allocation state. Walk entries via entry->Size (variable-length).
// Query via NtQuerySystemInformationEx with a process HANDLE as input;
// the AllocatedToTargetProcess flag reflects that specific process.
// Since Win10 TH2/1511.
inline constexpr ULONG SystemCpuSetInformation = 175;

// LOGICAL_PROCESSOR_RELATIONSHIP, PROCESSOR_CACHE_TYPE, GROUP_AFFINITY,
// CACHE_RELATIONSHIP, PROCESSOR_RELATIONSHIP, PROCESSOR_GROUP_INFO,
// GROUP_RELATIONSHIP, and SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX
// are now in <sys/ntabi.h>.

// CPU set information type selector — currently only CpuSetInformation exists.
typedef enum _CPU_SET_INFORMATION_TYPE {
  CpuSetInformation
} CPU_SET_INFORMATION_TYPE,
    *PCPU_SET_INFORMATION_TYPE;

// Per-logical-processor CPU set entry returned by SystemCpuSetInformation
// (175). One entry per LP. Walk the packed array via entry->Size
// (variable-length). Key fields for sched_*affinity: Id (unique CPU set ID
// passed to ThreadSelectedCpuSets), Group, and LogicalProcessorIndex (map to
// cpu_set_t bit = Group * 64 + LogicalProcessorIndex).
typedef struct _SYSTEM_CPU_SET_INFORMATION {
  DWORD Size;                    // Entry size (for variable-length walk)
  CPU_SET_INFORMATION_TYPE Type; // Always CpuSetInformation
  union {
    struct {
      DWORD Id;                   // Unique CPU set ID
      WORD Group;                 // Processor group
      BYTE LogicalProcessorIndex; // Index within group (0-63)
      BYTE CoreIndex;             // Physical core index
      BYTE LastLevelCacheIndex;   // LLC (L3) index
      BYTE NumaNodeIndex;         // NUMA node
      BYTE EfficiencyClass;       // 0=E-core, 1+=P-core (Big.LITTLE)
      union {
        BYTE AllFlags;
        struct {
          BYTE Parked : 1;                   // Core parked by power manager
          BYTE Allocated : 1;                // Assigned to any CPU set
          BYTE AllocatedToTargetProcess : 1; // Assigned to queried process
          BYTE RealTime : 1;                 // Real-time scheduling class
          BYTE ReservedFlags : 4;
        };
      };
      union {
        DWORD Reserved;
        BYTE SchedulingClass; // Thread scheduling class (0-31)
      };
      DWORD64 AllocationTag; // Opaque tag from SetProcessDefaultCpuSets
    } CpuSet;
  };
} SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;

//===----------------------------------------------------------------------===//
// Event and Wait APIs
//===----------------------------------------------------------------------===//

// Event types for NtCreateEvent.
enum EVENT_TYPE : ULONG {
  NotificationEvent = 0,   // Manual-reset event
  SynchronizationEvent = 1 // Auto-reset event
};

inline constexpr ACCESS_MASK EVENT_ALL_ACCESS = 0x001F0003;
inline constexpr ACCESS_MASK EVENT_MODIFY_STATE = 0x0002;

// STATUS_WAIT_0 is the same value as STATUS_SUCCESS (0x00000000).
// STATUS_USER_APC (0x000000C0) and STATUS_TIMEOUT (0x00000102) are
// defined above with the other NTSTATUS constants.

// Wait type for NtWaitForMultipleObjects.
enum WAIT_TYPE : ULONG { WaitAll = 0, WaitAny = 1 };

// Information class for NtQueryEvent.
enum EVENT_INFORMATION_CLASS : ULONG {
  EventBasicInformation = 0,
};

// Returned by NtQueryEvent(EventBasicInformation).
struct EVENT_BASIC_INFORMATION {
  EVENT_TYPE EventType; // NotificationEvent or SynchronizationEvent
  LONG EventState;      // Non-zero if signaled, zero if not signaled
};

//===----------------------------------------------------------------------===//
// Timer APIs (NtCreateTimer2 / NtSetTimer2 / NtCancelTimer2 / NtQueryTimer)
//===----------------------------------------------------------------------===//

// Timer types — SynchronizationTimer auto-resets when a wait is satisfied.
enum TIMER_TYPE : ULONG {
  NotificationTimer = 0,   // Manual-reset (stays signaled)
  SynchronizationTimer = 1 // Auto-reset (consumed by wait)
};

// Timer2 attribute flags (Windows 10+).
inline constexpr ULONG TIMER2_ATTRIBUTE_HIGH_RESOLUTION = 0x00000004;
inline constexpr ULONG TIMER2_ATTRIBUTE_NOTIFICATION = 0x80000000;

// T2_SET_PARAMETERS — coalescing parameters for NtSetTimer2.
struct T2_SET_PARAMETERS {
  ULONG Version; // Must be 0
  ULONG Reserved;
  LONGLONG NoWakeTolerance; // 0 = precise, -1 = max coalescing
};

inline constexpr ACCESS_MASK TIMER_ALL_ACCESS = 0x001F0003;

// Timer information classes for NtQueryTimer.
enum TIMER_INFORMATION_CLASS : ULONG { TimerBasicInformation = 0 };

struct TIMER_BASIC_INFORMATION {
  LARGE_INTEGER RemainingTime;
  BOOLEAN TimerState; // TRUE if signaled
};

//===----------------------------------------------------------------------===//
// Mutant (Kernel Mutex) — cross-process mutual exclusion
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK MUTANT_ALL_ACCESS = 0x001F0001;

// STATUS_ABANDONED: previous owner terminated without releasing.
// Returned by NtWaitForSingleObject — safe to proceed, the mutant is acquired.
inline constexpr NTSTATUS STATUS_ABANDONED = static_cast<NTSTATUS>(0x00000080);
inline constexpr NTSTATUS STATUS_ABANDONED_WAIT_0 =
    static_cast<NTSTATUS>(0x00000080);
inline constexpr NTSTATUS STATUS_MUTANT_NOT_OWNED =
    static_cast<NTSTATUS>(0xC0000046);

//===----------------------------------------------------------------------===//
// Semaphore — cross-process counting semaphore
//===----------------------------------------------------------------------===//

inline constexpr ACCESS_MASK SEMAPHORE_ALL_ACCESS = 0x001F0003;

inline constexpr NTSTATUS STATUS_SEMAPHORE_LIMIT_EXCEEDED =
    static_cast<NTSTATUS>(0xC0000047);

//===----------------------------------------------------------------------===//
// Fiber Local Storage (FLS)
//===----------------------------------------------------------------------===//

// Flags for RtlProcessFlsData(). Bitmask — can be combined.
inline constexpr ULONG RTL_FLS_DATA_CLEANUP_PER_SLOT = 1;   // invoke callbacks
inline constexpr ULONG RTL_FLS_DATA_CLEANUP_DEALLOCATE = 2; // free data block

//===----------------------------------------------------------------------===//
// DLL Load Notifications
//===----------------------------------------------------------------------===//

// Notification reasons for LdrRegisterDllNotification callback.
inline constexpr ULONG LDR_DLL_NOTIFICATION_REASON_LOADED =
    1; // DLL just loaded
inline constexpr ULONG LDR_DLL_NOTIFICATION_REASON_UNLOADED =
    2; // DLL about to unload

// Notification data passed to the callback. The active union member
// depends on the reason: Loaded for REASON_LOADED, Unloaded for
// REASON_UNLOADED. Both share the same layout.
// Reference: System Informer phnt/include/ntldr.h
struct LDR_DLL_NOTIFICATION_DATA_ENTRY {
  ULONG Flags; // Reserved, currently 0
  PCUNICODE_STRING
      FullDllName; // Full path (e.g. "C:\Windows\System32\ntdll.dll")
  PCUNICODE_STRING BaseDllName; // Filename only (e.g. "ntdll.dll")
  PVOID DllBase;                // Base address of the loaded image
  ULONG SizeOfImage;            // Size of the image in memory
};

struct LDR_DLL_NOTIFICATION_DATA {
  union {
    LDR_DLL_NOTIFICATION_DATA_ENTRY Loaded;
    LDR_DLL_NOTIFICATION_DATA_ENTRY Unloaded;
  };
};

using LDR_DLL_NOTIFICATION_FUNCTION = void(NTAPI *)(
    ULONG Reason, const LDR_DLL_NOTIFICATION_DATA *Data, PVOID Context);

//===----------------------------------------------------------------------===//
// Object Query APIs
//===----------------------------------------------------------------------===//

// Object information class for NtQueryObject / NtSetInformationObject.
enum OBJECT_INFORMATION_CLASS : ULONG {
  ObjectBasicInformation = 0,      // q: OBJECT_BASIC_INFORMATION
  ObjectNameInformation = 1,       // q: OBJECT_NAME_INFORMATION
  ObjectTypeInformation = 2,       // q: OBJECT_TYPE_INFORMATION
  ObjectTypesInformation = 3,      // q: OBJECT_TYPES_INFORMATION
  ObjectHandleFlagInformation = 4, // qs: OBJECT_HANDLE_FLAG_INFORMATION
};

// Returned by NtQueryObject(ObjectBasicInformation). Contains the handle's
// granted access mask, which lets us reconstruct O_RDONLY/O_WRONLY/O_RDWR
// for inherited fds.
// Reference: System Informer phnt/include/ntobapi.h
struct OBJECT_BASIC_INFORMATION {
  ULONG Attributes; // Handle attributes (OBJ_INHERIT, OBJ_PERMANENT, etc.)
  ACCESS_MASK
      GrantedAccess;  // Access mask granted when handle was created/duplicated
  ULONG HandleCount;  // Open handles to this object
  ULONG PointerCount; // Total references (handles + internal kernel refs)
  ULONG PagedPoolCharge;    // Paged pool bytes charged to this object
  ULONG NonPagedPoolCharge; // Non-paged pool bytes charged to this object
  ULONG Reserved[3];
  ULONG NameInfoSize;           // Buffer size needed for ObjectNameInformation
  ULONG TypeInfoSize;           // Buffer size needed for ObjectTypeInformation
  ULONG SecurityDescriptorSize; // Buffer size needed for security descriptor
  LARGE_INTEGER CreationTime;   // Creation time (symbolic links only, else 0)
};

// Returned by NtQueryObject(ObjectNameInformation). The Name buffer follows
// the struct in contiguous memory — Length/MaximumLength describe it, and
// Buffer points into the trailing bytes.
struct OBJECT_NAME_INFORMATION {
  UNICODE_STRING Name;
};

// OBJECT_HANDLE_FLAG_INFORMATION — settable handle attributes.
// Used with NtSetInformationObject(ObjectHandleFlagInformation) to toggle
// inheritance and close-protection on individual handles.
struct OBJECT_HANDLE_FLAG_INFORMATION {
  BOOLEAN Inherit;          // TRUE = handle is inherited by child processes
  BOOLEAN ProtectFromClose; // TRUE = NtClose on this handle returns
                            // STATUS_HANDLE_NOT_CLOSABLE
};

inline constexpr NTSTATUS STATUS_NOT_SAME_OBJECT =
    static_cast<NTSTATUS>(0xC0000461);

//===----------------------------------------------------------------------===//
// Private Namespaces — isolated object namespaces for sem_open/shm_open
//===----------------------------------------------------------------------===//
//
// Private namespaces provide isolated object directories in the NT namespace.
// Objects created within a private namespace are invisible to processes that
// don't have a handle to the namespace, providing isolation for POSIX named
// semaphores (sem_open) and shared memory (shm_open) without polluting
// \BaseNamedObjects.
//
// A boundary descriptor controls who can access the namespace. It is built
// in user mode with Rtl functions before passing to the Nt syscalls.

// OBJECT_BOUNDARY_DESCRIPTOR — describes the security boundary for a
// private namespace. Built by RtlCreateBoundaryDescriptor and populated
// with SIDs via RtlAddSIDToBoundaryDescriptor. Only processes whose
// tokens contain the specified SIDs can open the namespace.
inline constexpr ULONG OBJECT_BOUNDARY_DESCRIPTOR_VERSION = 1;

struct OBJECT_BOUNDARY_DESCRIPTOR {
  ULONG Version;   // Must be OBJECT_BOUNDARY_DESCRIPTOR_VERSION.
  ULONG Items;     // Number of boundary entries.
  ULONG TotalSize; // Total size in bytes including entries.
  union {
    ULONG Flags;
    struct {
      ULONG AddAppContainerSid : 1;
      ULONG Reserved : 31;
    };
  };
  // OBJECT_BOUNDARY_ENTRY Entries[] follows in contiguous memory.
};

using POBJECT_BOUNDARY_DESCRIPTOR = OBJECT_BOUNDARY_DESCRIPTOR *;

// RtlCreateBoundaryDescriptor — allocate a boundary descriptor.
// Name: namespace boundary name. Flags: 0 or
// BOUNDARY_DESCRIPTOR_ADD_APPCONTAINER_SID. Returns nullptr on failure. Free
// with RtlDeleteBoundaryDescriptor.
inline constexpr ULONG BOUNDARY_DESCRIPTOR_FLAG_NONE = 0x0000;
inline constexpr ULONG BOUNDARY_DESCRIPTOR_ADD_APPCONTAINER_SID = 0x0001;

//===----------------------------------------------------------------------===//
// Mount Manager — device-to-DOS-path resolution
//===----------------------------------------------------------------------===//
//
// The mount manager (\Device\MountPointManager) maps NT device names to DOS
// paths. IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH takes a device name like
// "\Device\HarddiskVolume3" and returns the DOS volume path "C:\".
// This covers drive letters, volume GUIDs, and mounted folders.

// CTL_CODE(MOUNTMGRCONTROLTYPE=0x6D, 12, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
inline constexpr ULONG IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH = 0x006D0030;

// Input to IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH.
struct MOUNTMGR_TARGET_NAME {
  USHORT DeviceNameLength; // Length in bytes of DeviceName.
  WCHAR DeviceName[1];     // Variable-length NT device name.
};

// Output from IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH.
struct MOUNTMGR_VOLUME_PATHS {
  ULONG MultiSzLength; // Length in bytes of the multi-sz string.
  WCHAR MultiSz[1];    // Double-null-terminated list of DOS paths.
};

} // extern "C"

// Well-known pseudo-handles. These are constants, not real handles — the
// kernel recognizes them as "the current process/thread" without a lookup.
// GetCurrentProcess() and GetCurrentThread() are just wrappers returning these.
inline HANDLE NtCurrentProcess() {
  return reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-1));
}
inline HANDLE NtCurrentThread() {
  return reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-2));
}

// TEB/PEB inline accessors. Outside extern "C" — C++ inline functions.
//
// TEB layout (relevant offsets, x64 and AArch64):
//   gs:0x30 / x18+0x00  — Self (TEB pointer)
//   gs:0x40 / x18+0x40  — ClientId.UniqueProcess (PID as HANDLE)
//   gs:0x48 / x18+0x48  — ClientId.UniqueThread  (TID as HANDLE)
//   gs:0x60 / x18+0x60  — ProcessEnvironmentBlock (PEB pointer)
inline TEB *NtCurrentTeb() {
  TEB *teb;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x30, %0" : "=r"(teb));
#elif defined(__aarch64__)
  __asm__ __volatile__("mov %0, x18" : "=r"(teb));
#endif
  return teb;
}
inline PEB *NtCurrentPeb() {
  PEB *peb;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x60, %0" : "=r"(peb));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %0, [x18, #0x60]" : "=r"(peb));
#endif
  return peb;
}

inline PVOID NtProcessHeap() { return NtCurrentPeb()->ProcessHeap; }

// Win32 last-error slot in TEB.LastErrorValue (+0x068).
// Equivalent to GetLastError()/SetLastError() — reads the same field.
inline DWORD NtGetLastError() noexcept {
  return NtCurrentTeb()->LastErrorValue;
}
inline void NtSetLastError(DWORD err) noexcept {
  NtCurrentTeb()->LastErrorValue = err;
}

// Returns the current process ID. Single instruction, no function call.
// Equivalent to GetCurrentProcessId() which is just `mov eax, gs:[0x40]`.
inline DWORD NtCurrentProcessId() {
  DWORD pid;
#ifdef __x86_64__
  __asm__ __volatile__("movl %%gs:0x40, %0" : "=r"(pid));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %w0, [x18, #0x40]" : "=r"(pid));
#endif
  return pid;
}

// Returns the current thread ID. Single instruction, no function call.
// Equivalent to GetCurrentThreadId() which is just `mov eax, gs:[0x48]`.
inline DWORD NtCurrentThreadId() {
  DWORD tid;
#ifdef __x86_64__
  __asm__ __volatile__("movl %%gs:0x48, %0" : "=r"(tid));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %w0, [x18, #0x48]" : "=r"(tid));
#endif
  return tid;
}

// Standard I/O handles from the current process parameters.
// Reads PEB::ProcessParameters fields directly — no kernel32 call.
inline HANDLE NtCurrentStandardInput() {
  return NtCurrentPeb()->ProcessParameters->StandardInput;
}
inline HANDLE NtCurrentStandardOutput() {
  return NtCurrentPeb()->ProcessParameters->StandardOutput;
}
inline HANDLE NtCurrentStandardError() {
  return NtCurrentPeb()->ProcessParameters->StandardError;
}

// ABI layout validation for NT query output structures.
// KERNEL_USER_TIMES: four LARGE_INTEGER fields — all 8 bytes each.
static_assert(sizeof(KERNEL_USER_TIMES) == 32,
              "KERNEL_USER_TIMES must be 32 bytes");
// THREAD_BASIC_INFORMATION: NTSTATUS + 4 pad before pointer-width fields.
static_assert(sizeof(THREAD_BASIC_INFORMATION) == 48,
              "THREAD_BASIC_INFORMATION must be 48 bytes");
static_assert(__builtin_offsetof(THREAD_BASIC_INFORMATION, TebBaseAddress) == 8,
              "THREAD_BASIC_INFORMATION::TebBaseAddress must be at offset 8");
static_assert(__builtin_offsetof(THREAD_BASIC_INFORMATION, ClientId) == 16,
              "THREAD_BASIC_INFORMATION::ClientId must be at offset 16");
static_assert(__builtin_offsetof(THREAD_BASIC_INFORMATION, AffinityMask) == 32,
              "THREAD_BASIC_INFORMATION::AffinityMask must be at offset 32");
// PROCESS_BASIC_INFORMATION: NTSTATUS + 4 pad, then pointer-width fields;
// BasePriority (LONG) introduces a second 4-byte pad before UniqueProcessId.
static_assert(sizeof(PROCESS_BASIC_INFORMATION) == 48,
              "PROCESS_BASIC_INFORMATION must be 48 bytes");
static_assert(__builtin_offsetof(PROCESS_BASIC_INFORMATION, PebBaseAddress) ==
                  8,
              "PROCESS_BASIC_INFORMATION::PebBaseAddress must be at offset 8");
static_assert(
    __builtin_offsetof(PROCESS_BASIC_INFORMATION, UniqueProcessId) == 32,
    "PROCESS_BASIC_INFORMATION::UniqueProcessId must be at offset 32");
// SYSTEM_BASIC_INFORMATION: seven ULONGs at 0-24 then 4-byte pad before three
// ULONG_PTR fields; NumberOfProcessors is the last byte at offset 56.
static_assert(sizeof(SYSTEM_BASIC_INFORMATION) == 64,
              "SYSTEM_BASIC_INFORMATION must be 64 bytes");
static_assert(
    __builtin_offsetof(SYSTEM_BASIC_INFORMATION, MinimumUserModeAddress) == 32,
    "SYSTEM_BASIC_INFORMATION::MinimumUserModeAddress must be at offset 32");
static_assert(
    __builtin_offsetof(SYSTEM_BASIC_INFORMATION, NumberOfProcessors) == 56,
    "SYSTEM_BASIC_INFORMATION::NumberOfProcessors must be at offset 56");

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_PROCESS_TYPES_H
