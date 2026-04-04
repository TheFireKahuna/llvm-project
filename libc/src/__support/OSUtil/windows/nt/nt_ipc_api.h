//===-- NT IPC / ALPC API declarations ------------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ALPC syscall declarations and RTL helper functions for IPC and private
// namespaces. These are the raw NT API surface — no Win32 wrappers.
//
// Reference: ntlpcapi.h, ntobapi.h, ntrtl.h from NT headers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IPC_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IPC_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_ipc_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// ALPC Port Lifecycle
//===----------------------------------------------------------------------===//

// Create a server-side ALPC port. The port name is specified in
// ObjectAttributes. PortAttributes configures security QoS, max message
// size, and resource limits. Returns the server port handle.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcCreatePort(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes,
                 PALPC_PORT_ATTRIBUTES PortAttributes);

// Connect to an existing ALPC port (basic variant).
// RequiredServerSid: if non-null, the kernel verifies the server process
// is running as this SID. Connection fails with STATUS_ACCESS_DENIED if
// the server doesn't match.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcConnectPort(PHANDLE PortHandle, PCUNICODE_STRING PortName,
                  POBJECT_ATTRIBUTES ObjectAttributes,
                  PALPC_PORT_ATTRIBUTES PortAttributes, ULONG Flags,
                  SID *RequiredServerSid, PPORT_MESSAGE ConnectionMessage,
                  PSIZE_T BufferLength,
                  PALPC_MESSAGE_ATTRIBUTES OutMessageAttributes,
                  PALPC_MESSAGE_ATTRIBUTES InMessageAttributes,
                  PLARGE_INTEGER Timeout);

// Connect to an existing ALPC port (extended variant, Windows 8+).
// ConnectionPortObjectAttributes.ObjectName is the port path.
// ServerSecurityRequirements: security descriptor the server must match.
// This is strictly stronger than RequiredServerSid — supports full DACL
// evaluation against the server's process token.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcConnectPortEx(PHANDLE PortHandle,
                    POBJECT_ATTRIBUTES ConnectionPortObjectAttributes,
                    POBJECT_ATTRIBUTES ClientPortObjectAttributes,
                    PALPC_PORT_ATTRIBUTES PortAttributes, ULONG Flags,
                    PSECURITY_DESCRIPTOR ServerSecurityRequirements,
                    PPORT_MESSAGE ConnectionMessage, PSIZE_T BufferLength,
                    PALPC_MESSAGE_ATTRIBUTES OutMessageAttributes,
                    PALPC_MESSAGE_ATTRIBUTES InMessageAttributes,
                    PLARGE_INTEGER Timeout);

// Accept or reject a connection request on a server port.
// ConnectionRequest is the LPC_CONNECTION_REQUEST message received via
// NtAlpcSendWaitReceivePort. AcceptConnection: TRUE = accept (returns
// a per-client communication port handle), FALSE = reject (client gets
// STATUS_PORT_CONNECTION_REFUSED).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcAcceptConnectPort(PHANDLE PortHandle, HANDLE ConnectionPortHandle,
                        ULONG Flags, POBJECT_ATTRIBUTES ObjectAttributes,
                        PALPC_PORT_ATTRIBUTES PortAttributes,
                        PVOID PortContext, PPORT_MESSAGE ConnectionRequest,
                        PALPC_MESSAGE_ATTRIBUTES ConnectionMessageAttributes,
                        BOOLEAN AcceptConnection);

// Complete an accepted connection so the client-side connect call can return
// a usable communication port.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtCompleteConnectPort(HANDLE PortHandle);

//===----------------------------------------------------------------------===//
// ALPC Message Send/Receive
//===----------------------------------------------------------------------===//

// Combined send + receive on an ALPC port.
// SendMessage: message to send (null = receive only).
// ReceiveMessage: buffer for received message (null = send only).
// BufferLength: in/out — size of receive buffer / actual received size.
// ReceiveMessageAttributes: kernel fills with sender attributes.
// Timeout: null = wait indefinitely.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcSendWaitReceivePort(HANDLE PortHandle, ULONG Flags,
                          PPORT_MESSAGE SendMessage,
                          PALPC_MESSAGE_ATTRIBUTES SendMessageAttributes,
                          PPORT_MESSAGE ReceiveMessage, PSIZE_T BufferLength,
                          PALPC_MESSAGE_ATTRIBUTES ReceiveMessageAttributes,
                          PLARGE_INTEGER Timeout);

// Disconnect a connected port. After disconnect, no more messages can be
// sent or received on this port handle. The handle itself must still be
// closed via NtClose.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtAlpcDisconnectPort(HANDLE PortHandle,
                                                          ULONG Flags);

//===----------------------------------------------------------------------===//
// ALPC Port Information
//===----------------------------------------------------------------------===//

// NtAlpcSetInformation — set port properties.
// Used with AlpcAssociateCompletionPortInformation to bind an ALPC port to
// an I/O completion port. When a message arrives, the kernel posts a
// completion to the IOCP with the specified key — no polling or dedicated
// listener thread required.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcSetInformation(HANDLE PortHandle,
                     ALPC_PORT_INFORMATION_CLASS PortInformationClass,
                     PVOID PortInformation, ULONG Length);

// NtAlpcQueryInformation — query port properties.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcQueryInformation(HANDLE PortHandle,
                       ALPC_PORT_INFORMATION_CLASS PortInformationClass,
                       PVOID PortInformation, ULONG Length,
                       PULONG ReturnLength);

//===----------------------------------------------------------------------===//
// ALPC Security Context
//===----------------------------------------------------------------------===//

// Create a security context for an ALPC port. The context tracks the
// sender's token and makes it available to the receiver via
// ALPC_SECURITY_ATTR on received messages.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcCreateSecurityContext(HANDLE PortHandle, ULONG Flags,
                            PALPC_SECURITY_ATTR SecurityAttribute);

// Delete a previously created security context.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAlpcDeleteSecurityContext(HANDLE PortHandle, ULONG Flags,
                            ALPC_HANDLE ContextHandle);

//===----------------------------------------------------------------------===//
// ALPC Message Attribute Helpers
//===----------------------------------------------------------------------===//

// Initialize a message attribute buffer. AttributeFlags selects which
// attribute types to allocate space for. Returns the required buffer size.
// The buffer must be at least this large and properly aligned.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
AlpcInitializeMessageAttribute(ULONG AttributeFlags,
                               PALPC_MESSAGE_ATTRIBUTES Buffer,
                               ULONG BufferLength, PULONG RequiredBufferSize);

// Extract a typed attribute pointer from a received message attribute buffer.
// AttributeFlag must be exactly one of the ALPC_MESSAGE_*_ATTRIBUTE values.
// Returns nullptr if the attribute is not present (not in ValidAttributes).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PVOID
AlpcGetMessageAttribute(PALPC_MESSAGE_ATTRIBUTES Buffer, ULONG AttributeFlag);

// Private namespace APIs (NtCreatePrivateNamespace, NtOpenPrivateNamespace,
// NtDeletePrivateNamespace) and boundary descriptor construction APIs are
// declared in nt_process_api.h to avoid duplicate declarations.

//===----------------------------------------------------------------------===//
// SID Construction Helpers (RTL)
//===----------------------------------------------------------------------===//

// Initialize a SID structure with the given authority and sub-authority count.
// The caller must fill in sub-authority values afterward.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
RtlInitializeSid(SID *Sid, SID_IDENTIFIER_AUTHORITY *IdentifierAuthority,
                 UCHAR SubAuthorityCount);

// Returns a pointer to the Nth sub-authority value in a SID.
// SubAuthorityIndex must be < Sid->SubAuthorityCount.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR PULONG RtlSubAuthoritySid(SID *Sid,
                                                       ULONG SubAuthorityIndex);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IPC_API_H
