//===-- NT IPC / ALPC type definitions ------------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ALPC (Advanced Local Procedure Call) structures, constants, and flags.
// These are the kernel-level IPC primitives used by the signal subsystem's
// cross-process transport. No Win32 or RPC/COM overhead.
//
// Reference: ntlpcapi.h, ntobapi.h from NT headers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IPC_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IPC_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"

//===----------------------------------------------------------------------===//
// PORT_MESSAGE — kernel-filled IPC message header
//===----------------------------------------------------------------------===//
//
// Every ALPC message begins with this header. The kernel fills ClientId
// (sender PID/TID), MessageId, and length fields. Userspace cannot forge
// these — they are the foundation of unforgeable sender identity.

struct PORT_MESSAGE {
  union {
    struct {
      CSHORT DataLength;  // Payload size (excluding header).
      CSHORT TotalLength; // DataLength + sizeof(PORT_MESSAGE).
    } s1;
    ULONG Length;
  } u1;
  union {
    struct {
      CSHORT Type;           // LPC_REQUEST, LPC_REPLY, LPC_DATAGRAM, etc.
      CSHORT DataInfoOffset; // Offset to PORT_DATA_INFORMATION (0 = none).
    } s2;
    ULONG ZeroInit;
  } u2;
  union {
    CLIENT_ID ClientId; // Kernel-filled: sender PID + TID.
    double DoNotUseThisField;
  };
  ULONG MessageId; // Kernel-assigned sequential ID.
  union {
    SIZE_T ClientViewSize;  // Valid for LPC_CONNECTION_REQUEST only.
    ULONG CallbackId;       // Valid for LPC_REQUEST only.
  };
};

using PPORT_MESSAGE = PORT_MESSAGE *;

// LPC message types (PORT_MESSAGE.u2.s2.Type).
inline constexpr CSHORT LPC_REQUEST = 1;
inline constexpr CSHORT LPC_REPLY = 2;
inline constexpr CSHORT LPC_DATAGRAM = 3;
inline constexpr CSHORT LPC_LOST_REPLY = 4;
inline constexpr CSHORT LPC_PORT_CLOSED = 5;
inline constexpr CSHORT LPC_CLIENT_DIED = 6;
inline constexpr CSHORT LPC_EXCEPTION = 7;
inline constexpr CSHORT LPC_DEBUG_EVENT = 8;
inline constexpr CSHORT LPC_ERROR_EVENT = 9;
inline constexpr CSHORT LPC_CONNECTION_REQUEST = 10;

//===----------------------------------------------------------------------===//
// ALPC port flags (ALPC_PORT_ATTRIBUTES.Flags)
//===----------------------------------------------------------------------===//

inline constexpr ULONG ALPC_PORFLG_NONE = 0x0;
inline constexpr ULONG ALPC_PORFLG_LPC_MODE = 0x1000;
inline constexpr ULONG ALPC_PORFLG_ALLOW_IMPERSONATION = 0x10000;
inline constexpr ULONG ALPC_PORFLG_ALLOW_LPC_REQUESTS = 0x20000;
inline constexpr ULONG ALPC_PORFLG_WAITABLE_PORT = 0x40000;
inline constexpr ULONG ALPC_PORFLG_ALLOW_DUP_OBJECT = 0x80000;
inline constexpr ULONG ALPC_PORFLG_SYSTEM_PROCESS = 0x100000;
inline constexpr ULONG ALPC_PORFLG_WAKE_POLICY1 = 0x200000;
inline constexpr ULONG ALPC_PORFLG_WAKE_POLICY2 = 0x400000;
inline constexpr ULONG ALPC_PORFLG_WAKE_POLICY3 = 0x800000;
inline constexpr ULONG ALPC_PORFLG_DIRECT_MESSAGE = 0x1000000;
inline constexpr ULONG ALPC_PORFLG_ALLOW_MULTIHANDLE_ATTRIBUTE = 0x2000000;

// Object type flags for DupObjectTypes in ALPC_PORT_ATTRIBUTES.
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_FILE = 0x0001;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_THREAD = 0x0004;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_SEMAPHORE = 0x0008;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_EVENT = 0x0010;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_PROCESS = 0x0020;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_MUTEX = 0x0040;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_SECTION = 0x0080;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_TOKEN = 0x0200;
inline constexpr ULONG ALPC_PORFLG_OBJECT_TYPE_JOB = 0x0800;

//===----------------------------------------------------------------------===//
// ALPC_PORT_ATTRIBUTES — port configuration
//===----------------------------------------------------------------------===//
//
// Passed to NtAlpcCreatePort and NtAlpcConnectPort. Configures the port's
// security QoS, maximum message size, and resource limits. For signal
// delivery we use minimal settings: no impersonation, no handle passing,
// no shared sections.

struct SECURITY_QUALITY_OF_SERVICE {
  ULONG Length;
  ULONG ImpersonationLevel; // SecurityAnonymous..SecurityDelegation
  UCHAR ContextTrackingMode;
  BOOLEAN EffectiveOnly;
};

struct ALPC_PORT_ATTRIBUTES {
  ULONG Flags;
  SECURITY_QUALITY_OF_SERVICE SecurityQos;
  SIZE_T MaxMessageLength;
  SIZE_T MemoryBandwidth;
  SIZE_T MaxPoolUsage;
  SIZE_T MaxSectionSize;
  SIZE_T MaxViewSize;
  SIZE_T MaxTotalSectionSize;
  ULONG DupObjectTypes;
#ifdef _WIN64
  ULONG Reserved;
#endif
};

using PALPC_PORT_ATTRIBUTES = ALPC_PORT_ATTRIBUTES *;

//===----------------------------------------------------------------------===//
// ALPC message attribute flags and structures
//===----------------------------------------------------------------------===//

// Message attribute type flags — select which attributes to request/provide
// in NtAlpcSendWaitReceivePort calls.
inline constexpr ULONG ALPC_MESSAGE_HANDLE_ATTRIBUTE = 0x10000000;
inline constexpr ULONG ALPC_MESSAGE_CONTEXT_ATTRIBUTE = 0x20000000;
inline constexpr ULONG ALPC_MESSAGE_VIEW_ATTRIBUTE = 0x40000000;
inline constexpr ULONG ALPC_MESSAGE_SECURITY_ATTRIBUTE = 0x80000000;

// ALPC_MESSAGE_ATTRIBUTES — per-message attribute buffer header.
// The kernel fills ValidAttributes to indicate which attributes are present.
// Actual attribute data follows this header in a contiguous buffer; use
// AlpcGetMessageAttribute() to extract typed pointers.
struct ALPC_MESSAGE_ATTRIBUTES {
  ULONG AllocatedAttributes; // Which attribute types we requested.
  ULONG ValidAttributes;     // Which attribute types the kernel filled.
};

using PALPC_MESSAGE_ATTRIBUTES = ALPC_MESSAGE_ATTRIBUTES *;

// ALPC handle (used for section handles and security context handles).
using ALPC_HANDLE = PVOID;
using PALPC_HANDLE = ALPC_HANDLE *;

// ALPC_SECURITY_ATTR — sender's security context, filled by the kernel.
// The ContextHandle is a server-side token handle that the server can
// query with NtQueryInformationToken. It represents the sender's token
// at the time the message was sent.
struct ALPC_SECURITY_ATTR {
  ULONG Flags;
  PSECURITY_QUALITY_OF_SERVICE QoS;
  ALPC_HANDLE ContextHandle;
};

using PALPC_SECURITY_ATTR = ALPC_SECURITY_ATTR *;

inline constexpr ULONG ALPC_SECFLG_CREATE_HANDLE = 0x20000;
inline constexpr ULONG ALPC_SECFLG_NOSECTIONHANDLE = 0x40000;

// ALPC_CONTEXT_ATTR — per-message context (port context, sequence, etc.).
struct ALPC_CONTEXT_ATTR {
  PVOID PortContext;
  PVOID MessageContext;
  ULONG Sequence;
  ULONG MessageId;
  ULONG CallbackId;
};

using PALPC_CONTEXT_ATTR = ALPC_CONTEXT_ATTR *;

// ALPC_DATA_VIEW_ATTR — shared memory view for large message payloads.
// Not used by the signal transport (messages are always small), but
// declared for API completeness.
struct ALPC_DATA_VIEW_ATTR {
  ULONG Flags;
  ALPC_HANDLE SectionHandle;
  PVOID ViewBase;
  SIZE_T ViewSize;
};

using PALPC_DATA_VIEW_ATTR = ALPC_DATA_VIEW_ATTR *;

inline constexpr ULONG ALPC_VIEWFLG_UNMAP_EXISTING = 0x10000;
inline constexpr ULONG ALPC_VIEWFLG_AUTO_RELEASE = 0x20000;
inline constexpr ULONG ALPC_VIEWFLG_NOT_SECURE = 0x40000;

//===----------------------------------------------------------------------===//
// ALPC message flags (NtAlpcSendWaitReceivePort Flags parameter)
//===----------------------------------------------------------------------===//

inline constexpr ULONG ALPC_MSGFLG_REPLY_MESSAGE = 0x1;
inline constexpr ULONG ALPC_MSGFLG_LPC_MODE = 0x2;
inline constexpr ULONG ALPC_MSGFLG_RELEASE_MESSAGE = 0x10000;
inline constexpr ULONG ALPC_MSGFLG_SYNC_REQUEST = 0x20000;
inline constexpr ULONG ALPC_MSGFLG_TRACK_PORT_REFERENCES = 0x40000;
inline constexpr ULONG ALPC_MSGFLG_WAIT_USER_MODE = 0x100000;
inline constexpr ULONG ALPC_MSGFLG_WAIT_ALERTABLE = 0x200000;

//===----------------------------------------------------------------------===//
// ALPC port section flags
//===----------------------------------------------------------------------===//

inline constexpr ULONG ALPC_CREATEPORTSECTIONFLG_SECURE = 0x40000;

//===----------------------------------------------------------------------===//
// ALPC-related NTSTATUS codes
//===----------------------------------------------------------------------===//

inline constexpr NTSTATUS STATUS_PORT_CONNECTION_REFUSED =
    static_cast<NTSTATUS>(0xC0000041);
inline constexpr NTSTATUS STATUS_PORT_DISCONNECTED =
    static_cast<NTSTATUS>(0xC0000037);
inline constexpr NTSTATUS STATUS_PORT_CLOSED_X =
    static_cast<NTSTATUS>(0xC0000700);
inline constexpr NTSTATUS STATUS_PORT_ALREADY_SET =
    static_cast<NTSTATUS>(0xC0000048);
inline constexpr NTSTATUS STATUS_REPLY_MESSAGE_MISMATCH =
    static_cast<NTSTATUS>(0xC000002C);

//===----------------------------------------------------------------------===//
// OBJECT_BOUNDARY_DESCRIPTOR — for private namespaces
//===----------------------------------------------------------------------===//

// OBJECT_BOUNDARY_DESCRIPTOR and related constants are defined in
// nt_process_types.h — include that header for boundary descriptor types.

//===----------------------------------------------------------------------===//
// Mandatory integrity level constants
//===----------------------------------------------------------------------===//

// SID authority for mandatory integrity labels: S-1-16-*
// Used with RtlAddIntegrityLabelToBoundaryDescriptor.
inline constexpr UCHAR SECURITY_MANDATORY_LABEL_AUTHORITY_VALUE[6] = {
    0, 0, 0, 0, 0, 16};

// Integrity level RIDs (sub-authority values for S-1-16-{RID}).
inline constexpr ULONG SECURITY_MANDATORY_UNTRUSTED_RID = 0x0000;
inline constexpr ULONG SECURITY_MANDATORY_LOW_RID = 0x1000;
inline constexpr ULONG SECURITY_MANDATORY_MEDIUM_RID = 0x2000;
inline constexpr ULONG SECURITY_MANDATORY_MEDIUM_PLUS_RID = 0x2100;
inline constexpr ULONG SECURITY_MANDATORY_HIGH_RID = 0x3000;
inline constexpr ULONG SECURITY_MANDATORY_SYSTEM_RID = 0x4000;
inline constexpr ULONG SECURITY_MANDATORY_PROTECTED_PROCESS_RID = 0x5000;

//===----------------------------------------------------------------------===//
// Well-known SID authorities
//===----------------------------------------------------------------------===//

// S-1-5-* (NT Authority). Defined here rather than in nt_security_types.h
// because it's needed for boundary descriptor construction.
inline constexpr UCHAR SECURITY_NT_AUTHORITY_VALUE[6] = {0, 0, 0, 0, 0, 5};

// Well-known sub-authority constants for Administrators group.
// S-1-5-32-544 = BUILTIN\Administrators
inline constexpr ULONG SECURITY_BUILTIN_DOMAIN_RID = 32;
inline constexpr ULONG DOMAIN_ALIAS_RID_ADMINS = 544;

//===----------------------------------------------------------------------===//
// ALPC port information classes — NtAlpcQueryInformation/NtAlpcSetInformation
//===----------------------------------------------------------------------===//

enum ALPC_PORT_INFORMATION_CLASS {
  AlpcBasicInformation = 0,
  AlpcPortInformation = 1,
  AlpcAssociateCompletionPortInformation = 2,
  AlpcConnectedSIDInformation = 3,
  AlpcServerInformation = 4,
  AlpcMessageZoneInformation = 5,
  AlpcRegisterCompletionListInformation = 6,
  AlpcUnregisterCompletionListInformation = 7,
  AlpcAdjustCompletionListConcurrencyCountInformation = 8,
  AlpcRegisterCallbackInformation = 9,
  AlpcCompletionListRundownInformation = 10,
  AlpcWaitForPortReferences = 11,
  AlpcServerSessionInformation = 12,
};

//===----------------------------------------------------------------------===//
// ALPC_PORT_ASSOCIATE_COMPLETION_PORT — bind ALPC port to an IOCP
//===----------------------------------------------------------------------===//
//
// Passed to NtAlpcSetInformation with AlpcAssociateCompletionPortInformation.
// When a message arrives on the port, the kernel posts a completion to the
// specified IOCP with the given key. This is persistent — every message
// triggers a completion, no re-arming needed.

struct ALPC_PORT_ASSOCIATE_COMPLETION_PORT {
  PVOID CompletionKey;   // Echoed in FILE_IO_COMPLETION_INFORMATION.KeyContext.
  HANDLE CompletionPort; // IOCP handle from NtCreateIoCompletion.
};

//===----------------------------------------------------------------------===//
// APC_CALLBACK_DATA_CONTEXT — for CALLBACK_DATA_CONTEXT APC delivery
//===----------------------------------------------------------------------===//
//
// When NtQueueApcThreadEx2 is called with
// QUEUE_USER_APC_FLAGS_CALLBACK_DATA_CONTEXT (0x00010000), the first
// argument to the APC callback is a pointer to this structure instead
// of the raw arg1. The interrupted thread's CONTEXT is provided via
// ContextRecord.

struct APC_CALLBACK_DATA_CONTEXT {
  ULONG_PTR Parameter;    // Original arg1 passed to NtQueueApcThreadEx2.
  PCONTEXT ContextRecord; // Thread context at point of interruption.
  ULONG_PTR Reserved0;
  ULONG_PTR Reserved1;
};

// QUEUE_USER_APC_FLAGS_* are defined in nt_process_types.h.
// QUEUE_USER_APC_FLAGS_SIGNAL combines SPECIAL_USER_APC | CALLBACK_DATA_CONTEXT
// for signal delivery — defined here as it is IPC-specific.
inline constexpr ULONG QUEUE_USER_APC_FLAGS_SIGNAL =
    QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC |
    QUEUE_USER_APC_FLAGS_CALLBACK_DATA_CONTEXT;

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IPC_TYPES_H
