//===-- AFD (Ancillary Function Driver) socket API declarations -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AFD is the kernel driver behind all Windows sockets. Every Winsock call
// (socket, bind, listen, connect, accept, send, recv, poll, shutdown) is
// an NtDeviceIoControlFile to \Device\Afd with an AFD ioctl code.
//
// This header declares the ioctl codes, structures, and constants needed
// to implement POSIX socket APIs directly against AFD, bypassing Winsock
// (ws2_32.dll, mswsock.dll) entirely.
//
// Transport mode: TLI (no transport device name at open time). This is the
// default and most common mode. TLI sockets use _TL structures and raw
// SOCKADDR for addresses. All structures here are the TLI/_TL variants.
//
// Reference: System Informer phnt/include/ntafd.h
// Validated: experiments/AfUnixCleanPath/ (pure-NT AF_UNIX proof-of-concept)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_AFD_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_AFD_H

#include "src/__support/OSUtil/windows/nt/nt_peb.h"
#include "src/__support/OSUtil/windows/nt/nt_types.h"

struct FILE_FULL_EA_INFORMATION;

// Opaque kernel file-object type — only the pointer form is used in AFUNIX
// endpoint layouts (RefdFileObject / BindFileObject / PeerFileObject). We
// never dereference it from user mode, so a forward declaration suffices.
struct FILE_OBJECT;
using PFILE_OBJECT = FILE_OBJECT *;

//===----------------------------------------------------------------------===//
// Kernel File-Object Prefixes
//===----------------------------------------------------------------------===//

// Public kernel structure used by file systems to associate cache/section state
// with a file stream. Multiple FILE_OBJECTs opened on the same stream share the
// same SectionObjectPointer value.
struct SECTION_OBJECT_POINTERS {
  PVOID DataSectionObject;
  PVOID SharedCacheMap;
  PVOID ImageSectionObject;
};

static_assert(sizeof(SECTION_OBJECT_POINTERS) == 0x18,
              "SECTION_OBJECT_POINTERS size mismatch");

// Prefix of the kernel FILE_OBJECT needed for the AF_UNIX bind/open decode.
// afunix.sys does not key duplicate binds on pathname text or handle value. It
// obtains a referenced FILE_OBJECT from IoCreateFileEx/ObReferenceObjectByHandle
// and then derives stream identity as:
//   file_object->SectionObjectPointer ? file_object->SectionObjectPointer
//                                     : file_object->FsContext
// This matches the documented semantics:
// - SectionObjectPointer is one-per-stream and shared across multiple opens
// - FsContext is file-system stream state and is also shared across multiple
//   opens to the same data stream
struct FILE_OBJECT_PREFIX {
  short Type;
  short Size;
  PVOID DeviceObject;
  PVOID Vpb;
  PVOID FsContext;
  PVOID FsContext2;
  SECTION_OBJECT_POINTERS *SectionObjectPointer;
};

static_assert(sizeof(FILE_OBJECT_PREFIX) == 0x30,
              "FILE_OBJECT prefix size mismatch");
static_assert(__builtin_offsetof(FILE_OBJECT_PREFIX, FsContext) == 0x18,
              "FILE_OBJECT FsContext offset mismatch");
static_assert(__builtin_offsetof(FILE_OBJECT_PREFIX, SectionObjectPointer) ==
                  0x28,
              "FILE_OBJECT SectionObjectPointer offset mismatch");

//===----------------------------------------------------------------------===//
// Device Names
//===----------------------------------------------------------------------===//

// Primary AFD device — opened via NtCreateFile with an AfdOpenPacketXX EA.
#define AFD_DEVICE_NAME u"\\Device\\Afd"

// AFD endpoint subpath — standard per-socket handle.
#define AFD_ENDPOINT_PATH u"\\Device\\Afd\\Endpoint"

// AFD poll helper — shared handle for IOCTL_AFD_POLL. Opened once, reused
// for all poll operations. Opened with DesiredAccess=SYNCHRONIZE, no EA.
#define AFD_MIO_PATH u"\\Device\\Afd\\Mio"

//===----------------------------------------------------------------------===//
// Extended Attribute Names
//===----------------------------------------------------------------------===//

// EA name for NtCreateFile to create an AFD endpoint. The EA value is an
// AFD_OPEN_PACKET structure. The "XX" suffix is part of the name.
#define AfdOpenPacket "AfdOpenPacketXX"

// Additional EA names observed in afd.sys for internal helper opens.
#define AfdSwOpenPacket "AfdSwOpenPacket"
#define AfdRioRDOpenPacket "AfdRioRDOpenPacket"

//===----------------------------------------------------------------------===//
// Endpoint Flags — AFD_OPEN_PACKET.EndpointFlags
//===----------------------------------------------------------------------===//

// Nibble-packed bitfield (4 bits per flag).
struct AFD_ENDPOINT_FLAGS {
  union {
    struct {
      UCHAR ConnectionLess : 1;  // bit 0: SOCK_DGRAM
      UCHAR : 3;
      UCHAR MessageMode : 1;    // bit 4: SOCK_SEQPACKET
      UCHAR : 3;
      UCHAR Raw : 1;            // bit 8: SOCK_RAW
      UCHAR : 3;
      UCHAR Multipoint : 1;     // bit 12: multipoint socket
      UCHAR : 3;
      UCHAR C_Root : 1;         // bit 16: multipoint C_ROOT
      UCHAR : 3;
      UCHAR D_Root : 1;         // bit 20: multipoint D_ROOT
      UCHAR : 3;
      UCHAR IgnoreTDI : 1;      // bit 24: ignore TDI layer
      UCHAR : 3;
      UCHAR RioSocket : 1;      // bit 28: RIO-capable endpoint
      UCHAR : 3;
    };
    ULONG EndpointFlags;
  };
};

//===----------------------------------------------------------------------===//
// Open Packet — EA value for NtCreateFile
//===----------------------------------------------------------------------===//

// Passed as the EA value when creating an AFD endpoint via NtCreateFile.
// For TLI mode: TransportDeviceNameLength = 0, no TransportDeviceName.
struct AFD_OPEN_PACKET {
  AFD_ENDPOINT_FLAGS __f;
  ULONG GroupID;                   // 0 (unused for AF_UNIX)
  LONG AddressFamily;              // AF_* (e.g. AF_UNIX=1, AF_INET=2)
  LONG SocketType;                 // SOCK_* (e.g. SOCK_STREAM=1)
  LONG Protocol;                   // IPPROTO_* or 0
  ULONG TransportDeviceNameLength; // 0 for TLI mode
  WCHAR TransportDeviceName[1];    // variable-length, empty for TLI
};

// Pre-built FILE_FULL_EA_INFORMATION + AfdOpenPacket EA name + AFD_OPEN_PACKET.
// Pass this as the EaBuffer to NtCreateFile.
struct AFD_OPEN_PACKET_FULL_EA {
  ULONG NextEntryOffset;
  UCHAR Flags;
  UCHAR EaNameLength;   // sizeof(AfdOpenPacket) - 1 = 15
  USHORT EaValueLength;  // sizeof(AFD_OPEN_PACKET) for TLI (no transport name)
  CHAR EaName[sizeof(AfdOpenPacket)]; // "AfdOpenPacketXX\0"
  AFD_OPEN_PACKET OpenPacket;
};

//===----------------------------------------------------------------------===//
// Transport Device Names
//===----------------------------------------------------------------------===//

// Only needed if specifying a transport device at open time (TDI/hybrid mode).
// TLI mode (TransportDeviceNameLength=0) is the standard path.
#define DD_TCP_DEVICE_NAME u"\\Device\\Tcp"
#define DD_TCPV6_DEVICE_NAME u"\\Device\\Tcp6"
#define DD_UDP_DEVICE_NAME u"\\Device\\Udp"
#define DD_UDPV6_DEVICE_NAME u"\\Device\\Udp6"
#define DD_RAW_IP_DEVICE_NAME u"\\Device\\RawIp"
#define DD_RAW_IPV6_DEVICE_NAME u"\\Device\\RawIp6"

//===----------------------------------------------------------------------===//
// IOCTL Function Numbers
//===----------------------------------------------------------------------===//

// AFD function codes — these are the Request values in the IOCTL encoding.
inline constexpr ULONG AFD_BIND = 0;
inline constexpr ULONG AFD_CONNECT = 1;
inline constexpr ULONG AFD_START_LISTEN = 2;
inline constexpr ULONG AFD_WAIT_FOR_LISTEN = 3;
inline constexpr ULONG AFD_ACCEPT = 4;
inline constexpr ULONG AFD_RECEIVE = 5;
inline constexpr ULONG AFD_RECEIVE_DATAGRAM = 6;
inline constexpr ULONG AFD_SEND = 7;
inline constexpr ULONG AFD_SEND_DATAGRAM = 8;
inline constexpr ULONG AFD_POLL = 9;
inline constexpr ULONG AFD_PARTIAL_DISCONNECT = 10;
inline constexpr ULONG AFD_GET_ADDRESS = 11;
inline constexpr ULONG AFD_QUERY_RECEIVE_INFO = 12;
inline constexpr ULONG AFD_QUERY_HANDLES = 13;
inline constexpr ULONG AFD_SET_INFORMATION = 14;
inline constexpr ULONG AFD_GET_REMOTE_ADDRESS = 15;
inline constexpr ULONG AFD_GET_CONTEXT = 16;
inline constexpr ULONG AFD_SET_CONTEXT = 17;
inline constexpr ULONG AFD_GET_INFORMATION = 30;
inline constexpr ULONG AFD_TRANSMIT_FILE = 31;
inline constexpr ULONG AFD_SUPER_ACCEPT = 32;
inline constexpr ULONG AFD_EVENT_SELECT = 33;
inline constexpr ULONG AFD_ENUM_NETWORK_EVENTS = 34;
inline constexpr ULONG AFD_DEFER_ACCEPT = 35;
inline constexpr ULONG AFD_WAIT_FOR_LISTEN_LIFO = 36;
inline constexpr ULONG AFD_NO_OPERATION = 39;
inline constexpr ULONG AFD_TRANSPORT_IOCTL = 47;
inline constexpr ULONG AFD_TRANSMIT_PACKETS = 48;
inline constexpr ULONG AFD_SUPER_CONNECT = 49;
inline constexpr ULONG AFD_SUPER_DISCONNECT = 50;
inline constexpr ULONG AFD_RECEIVE_MESSAGE = 51;
inline constexpr ULONG AFD_SEND_MESSAGE = 52;
inline constexpr ULONG AFD_SWITCH_PROVIDER_CHANGE = 66;
inline constexpr ULONG AFD_SWITCH_ADDRLIST_CHANGE = 67;
inline constexpr ULONG AFD_UNBIND = 68;
inline constexpr ULONG AFD_RIO = 70;
inline constexpr ULONG AFD_NOTIFY = 73;

//===----------------------------------------------------------------------===//
// IOCTL Code Encoding
//===----------------------------------------------------------------------===//

// AFD uses FILE_DEVICE_NETWORK (0x12) with a non-standard bit layout.
inline constexpr ULONG FSCTL_AFD_BASE = 0x12; // FILE_DEVICE_NETWORK

// I/O transfer methods (from wdm.h).
inline constexpr ULONG METHOD_BUFFERED = 0;
inline constexpr ULONG METHOD_NEITHER = 3;

inline constexpr ULONG AFD_CONTROL_CODE(ULONG Request, ULONG Method) {
  return (FSCTL_AFD_BASE << 12) | (Request << 2) | Method;
}

//===----------------------------------------------------------------------===//
// IOCTL Codes
//===----------------------------------------------------------------------===//

inline constexpr ULONG IOCTL_AFD_BIND = AFD_CONTROL_CODE(AFD_BIND, METHOD_NEITHER);                             // 0x12003
inline constexpr ULONG IOCTL_AFD_CONNECT = AFD_CONTROL_CODE(AFD_CONNECT, METHOD_NEITHER);                       // 0x12007
inline constexpr ULONG IOCTL_AFD_START_LISTEN = AFD_CONTROL_CODE(AFD_START_LISTEN, METHOD_NEITHER);              // 0x1200B
inline constexpr ULONG IOCTL_AFD_WAIT_FOR_LISTEN = AFD_CONTROL_CODE(AFD_WAIT_FOR_LISTEN, METHOD_BUFFERED);       // 0x1200C
inline constexpr ULONG IOCTL_AFD_ACCEPT = AFD_CONTROL_CODE(AFD_ACCEPT, METHOD_BUFFERED);                        // 0x12010
inline constexpr ULONG IOCTL_AFD_RECEIVE = AFD_CONTROL_CODE(AFD_RECEIVE, METHOD_NEITHER);                       // 0x12017
inline constexpr ULONG IOCTL_AFD_RECEIVE_DATAGRAM = AFD_CONTROL_CODE(AFD_RECEIVE_DATAGRAM, METHOD_NEITHER);      // 0x1201B
inline constexpr ULONG IOCTL_AFD_SEND = AFD_CONTROL_CODE(AFD_SEND, METHOD_NEITHER);                             // 0x1201F
inline constexpr ULONG IOCTL_AFD_SEND_DATAGRAM = AFD_CONTROL_CODE(AFD_SEND_DATAGRAM, METHOD_NEITHER);           // 0x12023
inline constexpr ULONG IOCTL_AFD_POLL = AFD_CONTROL_CODE(AFD_POLL, METHOD_BUFFERED);                            // 0x12024
inline constexpr ULONG IOCTL_AFD_PARTIAL_DISCONNECT = AFD_CONTROL_CODE(AFD_PARTIAL_DISCONNECT, METHOD_NEITHER);  // 0x1202B
inline constexpr ULONG IOCTL_AFD_GET_ADDRESS = AFD_CONTROL_CODE(AFD_GET_ADDRESS, METHOD_NEITHER);                // 0x1202F
inline constexpr ULONG IOCTL_AFD_QUERY_RECEIVE_INFO = AFD_CONTROL_CODE(AFD_QUERY_RECEIVE_INFO, METHOD_NEITHER);  // 0x12033
inline constexpr ULONG IOCTL_AFD_QUERY_HANDLES = AFD_CONTROL_CODE(AFD_QUERY_HANDLES, METHOD_NEITHER);            // 0x12037
inline constexpr ULONG IOCTL_AFD_SET_INFORMATION = AFD_CONTROL_CODE(AFD_SET_INFORMATION, METHOD_NEITHER);        // 0x1203B
inline constexpr ULONG IOCTL_AFD_GET_REMOTE_ADDRESS = AFD_CONTROL_CODE(AFD_GET_REMOTE_ADDRESS, METHOD_NEITHER);  // 0x1203F
inline constexpr ULONG IOCTL_AFD_GET_CONTEXT = AFD_CONTROL_CODE(AFD_GET_CONTEXT, METHOD_NEITHER);                // 0x12043
inline constexpr ULONG IOCTL_AFD_SET_CONTEXT = AFD_CONTROL_CODE(AFD_SET_CONTEXT, METHOD_NEITHER);                // 0x12047
inline constexpr ULONG IOCTL_AFD_SET_CONNECT_DATA = AFD_CONTROL_CODE(18, METHOD_NEITHER);                        // 0x1204B
inline constexpr ULONG IOCTL_AFD_GET_CONNECT_DATA = AFD_CONTROL_CODE(22, METHOD_NEITHER);                        // 0x1205B
inline constexpr ULONG IOCTL_AFD_GET_INFORMATION = AFD_CONTROL_CODE(AFD_GET_INFORMATION, METHOD_NEITHER);        // 0x1207B
inline constexpr ULONG IOCTL_AFD_TRANSMIT_FILE = AFD_CONTROL_CODE(AFD_TRANSMIT_FILE, METHOD_NEITHER);            // 0x1207F
inline constexpr ULONG IOCTL_AFD_SUPER_ACCEPT = AFD_CONTROL_CODE(AFD_SUPER_ACCEPT, METHOD_NEITHER);              // 0x12083
inline constexpr ULONG IOCTL_AFD_EVENT_SELECT = AFD_CONTROL_CODE(AFD_EVENT_SELECT, METHOD_NEITHER);              // 0x12087
inline constexpr ULONG IOCTL_AFD_ENUM_NETWORK_EVENTS = AFD_CONTROL_CODE(AFD_ENUM_NETWORK_EVENTS, METHOD_NEITHER); // 0x1208B
inline constexpr ULONG IOCTL_AFD_DEFER_ACCEPT = AFD_CONTROL_CODE(AFD_DEFER_ACCEPT, METHOD_BUFFERED);             // 0x1208C
inline constexpr ULONG IOCTL_AFD_WAIT_FOR_LISTEN_LIFO = AFD_CONTROL_CODE(AFD_WAIT_FOR_LISTEN_LIFO, METHOD_BUFFERED); // 0x12090
inline constexpr ULONG IOCTL_AFD_NO_OPERATION = AFD_CONTROL_CODE(AFD_NO_OPERATION, METHOD_NEITHER);              // 0x1209F
inline constexpr ULONG IOCTL_AFD_GET_UNACCEPTED_CONNECT_DATA = AFD_CONTROL_CODE(41, METHOD_NEITHER);             // 0x120A7
inline constexpr ULONG IOCTL_AFD_TRANSPORT_IOCTL = AFD_CONTROL_CODE(AFD_TRANSPORT_IOCTL, METHOD_NEITHER);        // 0x120BF
inline constexpr ULONG IOCTL_AFD_TRANSMIT_PACKETS = AFD_CONTROL_CODE(AFD_TRANSMIT_PACKETS, METHOD_NEITHER);      // 0x120C3
inline constexpr ULONG IOCTL_AFD_SUPER_CONNECT = AFD_CONTROL_CODE(AFD_SUPER_CONNECT, METHOD_NEITHER);            // 0x120C7
inline constexpr ULONG IOCTL_AFD_SUPER_DISCONNECT = AFD_CONTROL_CODE(AFD_SUPER_DISCONNECT, METHOD_NEITHER);      // 0x120CB
inline constexpr ULONG IOCTL_AFD_RECEIVE_MESSAGE = AFD_CONTROL_CODE(AFD_RECEIVE_MESSAGE, METHOD_NEITHER);        // 0x120CF
inline constexpr ULONG IOCTL_AFD_SEND_MESSAGE = AFD_CONTROL_CODE(AFD_SEND_MESSAGE, METHOD_NEITHER);              // 0x120D3
inline constexpr ULONG IOCTL_AFD_SWITCH_PROVIDER_CHANGE = AFD_CONTROL_CODE(AFD_SWITCH_PROVIDER_CHANGE, METHOD_NEITHER); // 0x1210B
inline constexpr ULONG IOCTL_AFD_SWITCH_ADDRLIST_CHANGE = AFD_CONTROL_CODE(AFD_SWITCH_ADDRLIST_CHANGE, METHOD_BUFFERED); // 0x1210C
inline constexpr ULONG IOCTL_AFD_UNBIND = AFD_CONTROL_CODE(AFD_UNBIND, METHOD_NEITHER);                          // 0x12113
inline constexpr ULONG IOCTL_AFD_RIO = AFD_CONTROL_CODE(AFD_RIO, METHOD_NEITHER);                                // 0x1211B
inline constexpr ULONG IOCTL_AFD_NOTIFY = AFD_CONTROL_CODE(AFD_NOTIFY, METHOD_NEITHER);                          // 0x12127

//===----------------------------------------------------------------------===//
// Transport IOCTL Opcodes (used with IOCTL_AFD_TRANSPORT_IOCTL)
//===----------------------------------------------------------------------===//

// afunix.sys provider-level option names (routed via IOCTL_AFD_TRANSPORT_IOCTL
// with AFD_TL_IO_CONTROL_INFO.Type == TlSetSockOptIoControlType and Level ==
// 0xFFFC or SOL_SOCKET (0xFFFF) — AfUnixEndpointSetSocketOption enforces this
// gate before reaching the option handlers).
//
// Full instruction-level decode lives in AF_UNIX_DECODE_NOTES.md S8 (authoritative,
// supersedes S5.5/S7.6). Summary:
//
//   0x98000000 SetFilePathOption handler @ afunix!AfUnixEndpointSetFilePathOption
//                                          (afunix.sys 10.0.26100.6725, 0x14000CC30)
//     Input blob: { PVOID UserFileHandle; BYTE ExtraData[]; }
//       — UserFileHandle may be NULL (cbz bypass at 0x14000CCE8 skips
//         ObReferenceObjectByHandle; handle-less form is fully supported).
//       — ExtraData is fully opaque to afunix.sys: the driver pool-copies the
//         bytes and stores them at [Ep+0x30]/[Ep+0x28] as (PVOID, USHORT, USHORT)
//         under the endpoint push lock. The only code path that ever reads the
//         payload back is the destructive extract in AfUnixEndpointGetFileProperties
//         (@ 0x14000C9F0), which consumes it during bind and the endpoint
//         destructor (which simply ExFreePool's it). No reparse-buffer or EA
//         parsing ever happens on these bytes.
//     Size: 10 <= InputLength < 0x8008 (i.e. 2 <= ExtraLen < 32768).
//
//   0x98000001 SetEaBuffer handler @ afunix!AfUnixEndpointSetEaBuffer
//                                    (afunix.sys 10.0.26100.6725, 0x14000CAF0)
//     Input blob: any opaque byte sequence, 1 <= InputLength <= UINT32_MAX.
//       — Stored verbatim at [Ep+0xD0]/[Ep+0xD8]. ZERO readers anywhere in the
//         currently-decoded afunix code paths. The slot is effectively dead
//         code retained for ABI compatibility; only the destructor frees it.
//     Fields are DISJOINT from SetFilePathOption ({0x20,0x28,0x30}); both may
//     be set simultaneously and are independently managed.
//
// libc NT-POSIX consequence: neither option is load-bearing for our rewrite.
// We create AF_UNIX reparse-point files directly via NtCreateFile + the
// IOCTL_AFD_BIND sockaddr_un path, and peer-cred queries go through
// SIO_AF_UNIX_GETPEERPID below.
inline constexpr ULONG AFD_AFINUX_SET_FILE_PATH = 0x98000000;
inline constexpr ULONG AFD_AFINUX_SET_CREATE_EA_BUFFER = 0x98000001;

// Level gate on the provider-ioctl dispatcher (S4.6 / S7.4 decode at
// AfUnixEndpointSetSocketOption @ 0x14000CE68): requests fail with
// STATUS_NOT_SUPPORTED unless Level is one of these two values.
inline constexpr ULONG AFD_AFINUX_SOCKET_OPTION_LEVEL_PROVIDER = 0xFFFC;
inline constexpr ULONG AFD_AFINUX_SOCKET_OPTION_LEVEL_SOCKET = 0xFFFF;

// Private user-mode AF_UNIX socket-info level used by wshunix.dll. This is a
// Winsock helper cache layered above AFD/afunix, not part of afd.sys
// GET_CONTEXT/SET_CONTEXT. On Windows 11 22H2+/24H2, option 1 serializes a
// variable-length snapshot whose fixed 0x80-byte header is described below,
// while 0x20000001 and 0x20000002 expose the cached local and remote pathname
// bytes respectively.
inline constexpr ULONG WSHUNIX_PRIVATE_SOCKET_INFO_LEVEL = 0xFFFE;
inline constexpr ULONG WSHUNIX_SOCKET_INFO_SNAPSHOT = 0x00000001;
inline constexpr ULONG WSHUNIX_SOCKET_INFO_LOCAL_PATH = 0x20000001;
inline constexpr ULONG WSHUNIX_SOCKET_INFO_REMOTE_PATH = 0x20000002;
// Private mswsock.dll WSAIoctl codes that forward to the path-only helper
// setters above on AF_UNIX sockets. On the traced Windows 11 producer path,
// bind uses 0x98000101 / option 0x20000001 for the local pathname cache and
// connect uses 0x98000102 / option 0x20000002 for the remote pathname cache
// before SocketAfUnixSetFilePath canonicalizes the sockaddr_un pathname and
// issues AFD_AFINUX_SET_FILE_PATH.
inline constexpr ULONG MSWSOCK_AFINUX_SET_LOCAL_PATH_IOCTL = 0x98000101;
inline constexpr ULONG MSWSOCK_AFINUX_SET_REMOTE_PATH_IOCTL = 0x98000102;
inline constexpr ULONG WSH_NOTIFY_BIND = 0x00000001;
inline constexpr ULONG WSH_NOTIFY_LISTEN = 0x00000002;
inline constexpr ULONG WSH_NOTIFY_CONNECT = 0x00000004;
inline constexpr ULONG WSH_NOTIFY_ACCEPT = 0x00000008;
inline constexpr ULONG WSH_NOTIFY_SHUTDOWN_RECEIVE = 0x00000010;
inline constexpr ULONG WSH_NOTIFY_SHUTDOWN_SEND = 0x00000020;
inline constexpr ULONG WSH_NOTIFY_SHUTDOWN_ALL = 0x00000040;
inline constexpr ULONG WSH_NOTIFY_CLOSE = 0x00000080;
inline constexpr ULONG WSH_NOTIFY_CONNECT_ERROR = 0x00000100;
inline constexpr ULONG WSHUNIX_SOCKET_NOTIFICATION_MASK =
    WSH_NOTIFY_BIND | WSH_NOTIFY_CONNECT | WSH_NOTIFY_CLOSE;

// Private AF_UNIX endpoint ioctls dispatched inside afunix!AfUnixEndpointIoctl
// (@ 0x14000D070). Full sub-opcode table from S7.4 decode:
//
//   0x04 CancelIo / Shutdown-RD — inline in the dispatcher; requires Kind==2
//                                 endpoint, returns STATUS_INVALID_DEVICE_REQUEST
//                                 otherwise.
//   0x08 Bind                    — AfUnixEndpointIoctlBind @ 0x14000D1B0.
//   0x0C Unbind                  — hardcoded error path returning
//                                  STATUS_INVALID_PARAMETER (0xC000000D) with
//                                  trace string "unbind unimplemented". This is
//                                  a permanent architectural refusal in the
//                                  current afunix.sys — not a stub awaiting
//                                  implementation. Never issue this sub-opcode.
//   0x10 QueryLocalAddress       — AfUnixEndpointIoctlQueryLocalAddress
//                                  @ 0x14000D528 (getsockname).
//   0x14 QueryRemoteAddress      — AfUnixEndpointIoctlQueryRemoteAddress
//                                  @ 0x14000D608 (getpeername).
//   other                         — STATUS_INVALID_DEVICE_REQUEST (0xC0000010).
//
// The `0x08` bind arm (S6.4) requires InputLength >= 0x6E (sizeof sockaddr_un),
// validates sun_family == AF_UNIX, and for pathname binds consumes the
// provider-primed pathname state via the destructive GetFileProperties extract
// at [Ep+0x20/+0x28/+0x30] (see SetFilePathOption below). afunix.sys keys
// duplicate binds and later open-to-endpoint lookup on the RB-tree node at
// [Ep+0xB0] using `FILE_OBJECT.FsContext2 ?: FILE_OBJECT.FsContext` as the
// comparator (S6.2).
//
// A direct user-mode IOCTL_AFD_TRANSPORT_IOCTL probe with TlEndpointIoControlType
// is rejected with STATUS_INVALID_PARAMETER on the traced Windows 11 path, so
// these are internal provider operations reached via the mswsock/wshunix
// bind/connect bridge rather than a stable direct user ABI. libc NT-POSIX
// drives the equivalent behavior through IOCTL_AFD_BIND /
// IOCTL_AFD_GET_ADDRESS / IOCTL_AFD_GET_REMOTE_ADDRESS.
inline constexpr ULONG AFD_AFINUX_ENDPOINT_CANCEL_IO = 0x04;
inline constexpr ULONG AFD_AFINUX_ENDPOINT_BIND = 0x08;
inline constexpr ULONG AFD_AFINUX_ENDPOINT_UNBIND = 0x0C;
inline constexpr ULONG AFD_AFINUX_ENDPOINT_QUERY_LOCAL_ADDRESS = 0x10;
inline constexpr ULONG AFD_AFINUX_ENDPOINT_QUERY_REMOTE_ADDRESS = 0x14;

// Retrieve the PID of the peer process on an AF_UNIX connection.
inline constexpr ULONG SIO_AF_UNIX_GETPEERPID = 0x58000100;

// Input for AFD_AFINUX_SET_FILE_PATH.
// afunix.sys interprets this as an optional 8-byte handle followed by the
// exact UTF-16 path byte sequence supplied by InputBufferLength - 8.
struct AFD_UNIX_SET_FILE_PATH_INPUT {
  HANDLE FileHandle; // Optional file handle/root handle, usually nullptr.
  WCHAR NtPath[1];   // Variable-length UTF-16 path bytes.
};

// Input for AFD_AFINUX_SET_CREATE_EA_BUFFER.
// afunix.sys stores the caller bytes verbatim and bind later forwards them as
// the EaBuffer/EaLength pair to IoCreateFileEx. Producers are expected to
// begin this buffer with one or more FILE_FULL_EA_INFORMATION entries. The
// created/opened AF_UNIX address file is then identified by the resulting
// FILE_OBJECT stream key (`SectionObjectPointer ?: FsContext`), not by the EA
// bytes themselves.
using AFD_UNIX_SET_CREATE_EA_BUFFER_INPUT = FILE_FULL_EA_INFORMATION;

// Public Winsock metadata bits mirrored into SOCK_SHARED_INFO by the traced
// AF_UNIX SET_CONTEXT producer path.
inline constexpr USHORT AFD_SOCK_SHARED_INFO_FLAG_LISTENING = 0x0001;
inline constexpr USHORT AFD_SOCK_SHARED_INFO_FLAG_IS_TLI = 0x1000;
inline constexpr ULONG AFD_WSA_FLAG_OVERLAPPED = 0x00000001;
inline constexpr ULONG AFD_WSA_FLAG_NO_HANDLE_INHERIT = 0x00000080;
inline constexpr ULONG AFD_XP1_GUARANTEED_DELIVERY = 0x00000002;
inline constexpr ULONG AFD_XP1_GUARANTEED_ORDER = 0x00000004;
inline constexpr ULONG AFD_XP1_GRACEFUL_CLOSE = 0x00000020;
inline constexpr ULONG AFD_XP1_IFS_HANDLES = 0x00020000;
inline constexpr ULONG AFD_PFL_MATCHES_PROTOCOL_ZERO = 0x00000008;

// Current Win11 AF_UNIX SOCK_SHARED_INFO publication defaults observed in the
// mswsock/wshunix SET_CONTEXT producer path. These are producer values, not a
// public kernel contract, but they are the grounded defaults libc mirrors for
// AF_UNIX stream sockets today.
inline constexpr USHORT AFD_UNIX_SHARED_INFO_FLAGS_BOUND =
    AFD_SOCK_SHARED_INFO_FLAG_IS_TLI;
inline constexpr USHORT AFD_UNIX_SHARED_INFO_FLAGS_LISTENING =
    AFD_SOCK_SHARED_INFO_FLAG_LISTENING | AFD_SOCK_SHARED_INFO_FLAG_IS_TLI;
inline constexpr ULONG AFD_UNIX_SHARED_INFO_CREATION_FLAGS =
    AFD_WSA_FLAG_OVERLAPPED | AFD_WSA_FLAG_NO_HANDLE_INHERIT;
// This is SOCK_SHARED_INFO.CatalogEntryId, i.e. WSAPROTOCOL_INFO.dwCatalogEntryId
// for the selected protocol-catalog row. ws2_32 resolves a PROTO_CATALOG_ITEM
// from the requested socket attributes, DSOCKET::Initialize stores that
// catalog item's WSAPROTOCOL_INFO into the live socket state, and
// mswsock!SockSetHandleContext later bulk-copies the 0x78-byte SOCK_SHARED_INFO
// block into the AFD SET_CONTEXT image. Reverse lookup in ws2_32 goes through
// DCATALOG::GetCountedCatalogItemFromCatalogEntryId, so this is a Winsock
// catalog identity token, not an AFD/afunix semantic control value. The
// traced AF_UNIX producer row on the current Win11 install is 0x3EE.
inline constexpr ULONG AFD_UNIX_SHARED_INFO_OBSERVED_PROTOCOL_CATALOG_ENTRY_ID =
    0x000003EE;
inline constexpr ULONG AFD_UNIX_SHARED_INFO_SERVICE_FLAGS1 =
    AFD_XP1_GUARANTEED_DELIVERY | AFD_XP1_GUARANTEED_ORDER |
    AFD_XP1_GRACEFUL_CLOSE | AFD_XP1_IFS_HANDLES;
inline constexpr ULONG AFD_UNIX_SHARED_INFO_PROVIDER_FLAGS =
    AFD_PFL_MATCHES_PROTOCOL_ZERO;
// Producer-side publication defaults: mswsock.dll's SockSetHandleContext
// stamps these into SOCK_SHARED_INFO.{ReceiveBufferSize,SendBufferSize} when
// materialising the AFD context blob. They are *decorative* — neither afd.sys
// nor afunix.sys reads them back (Phase A.1/A.2 decode). The actual kernel-side
// send-queue depth is tracked on the connection endpoint at [Ep+0x1A0]
// (AFUNIX_CONN_ENDPOINT::SendQueuedBytes, S6.5). We mirror the published
// defaults only so that a WSADuplicateSocket round-trip through wshunix.dll
// would see plausible values.
inline constexpr ULONG AFD_UNIX_DEFAULT_RECEIVE_BUFFER_SIZE = 0x00010000;
inline constexpr ULONG AFD_UNIX_DEFAULT_SEND_BUFFER_SIZE = 0x00010000;
inline constexpr ULONG AFD_UNIX_CONTEXT_TAIL_ALIGNMENT_PADDING = 0x00000000;
// AF_UNIX provider GUID: {A00943D9-9C2E-4633-9B59-0057A3160994}
inline constexpr UCHAR AFD_UNIX_PROVIDER_GUID[16] = {
    0xD9, 0x43, 0x09, 0xA0, 0x2E, 0x9C, 0x33, 0x46,
    0x9B, 0x59, 0x00, 0x57, 0xA3, 0x16, 0x09, 0x94};

// NPI client module id for afunix.sys:
// {2227E804-8D8B-11D4-ABAD-009027719E09}. Located in afunix.sys .rdata at
// VA 0x140006000 (S2.5). Used by afd.sys's NPI binder to locate the AF_UNIX
// transport provider at module load. Informational only — user-mode code
// never constructs an NPI registration.
inline constexpr UCHAR AFD_UNIX_NPI_MODULE_ID[16] = {
    0x04, 0xE8, 0x27, 0x22, 0x8B, 0x8D, 0xD4, 0x11,
    0xAB, 0xAD, 0x00, 0x90, 0x27, 0x71, 0x9E, 0x09};

//===----------------------------------------------------------------------===//
// Kernel Pool Tags (for kernel debugger !pooltag / !poolfind queries)
//===----------------------------------------------------------------------===//

// Tag carried on most afunix.sys allocations: AFUNIX_ENDPOINT (256/280/560B),
// AFUNIX_REPARSE_DATA_BUFFER (24B), FSRTL_ATOMIC_CREATE_ECP_CONTEXT (88B),
// the SetFilePathOption ExtraData pool, the SetEaBuffer pool, SendRequestNode
// (0x48B), and RecvRequestNode (0x58B). Four ASCII bytes 'W','n','p','I'
// packed little-endian = 'IpnW' when read as a ULONG — the debugger displays
// either spelling depending on endianness convention. Sourced from literal at
// afunix.sys VA 0x14000A3D0 (S2.10).
inline constexpr ULONG AFUNIX_POOL_TAG_WNPI = 0x69706E57; // 'WnpI'

// afd.sys AfdSetContext caches the caller context blob with this tag. See
// ExAllocatePoolWithTag call at afd!AfdSetContext @ 0x14001FDB0 (Phase A.1).
inline constexpr ULONG AFD_POOL_TAG_AFDX = 0x58646641;    // 'AfdX'

//===----------------------------------------------------------------------===//
// AFD_ENDPOINT_TYPE — 16-bit type discriminant at AFD_ENDPOINT[+0x00]
//===----------------------------------------------------------------------===//
//
// afd.sys stamps an endpoint-type USHORT into its private kernel endpoint
// struct (distinct from our AFD_UNIX_CONTEXT_IMAGE and from the afunix
// AFUNIX_ENDPOINT+0x00 RefCount word). `AfdSetContext` / `AfdGetContext`
// branch on this discriminant to decide code paths.
//
// Values confirmed by exhaustive MOVZ-immediate scan of afd.sys .text
// (S2.8 + S3.1). Each value has a single producer site:
enum AFD_ENDPOINT_TYPE : USHORT {
  AFD_EP_TDI_LEGACY         = 0xAFD0, // generic TDI endpoint; produced at
                                      // afd!... @ 0x140008014
  AFD_EP_TDI_HANDLE_HELPER  = 0xAFD2, // produced at 0x140002C78+
  AFD_EP_EXTENSION_FLAG     = 0xAFD4, // or-in flag — never a sole value;
                                      // 0x14001CE18
  AFD_EP_GROUP_HELPER       = 0xAFD8, // produced at 0x1400068DC
  AFD_EP_ANCILLARY_1        = 0xACE1,
  AFD_EP_ANCILLARY_2        = 0xACE2,
  AFD_EP_ANCILLARY_3        = 0xACE3, // 0x14003A00C/74/BC
  AFD_EP_SOCKET_FILE_BIND   = 0xAAFD, // 0x140007EC8
  AFD_EP_CONNECTION         = 0xCAFD, // 0x140008FB4
  AFD_EP_CONTROL_REQUEST    = 0xEAFD, // 0x1400073B0
  AFD_EP_AFUNIX             = 0x1AFD, // SAN/NPI endpoint, produced *only* by
                                      // afd!AfdSanInitEndpoint+0x7C
                                      // @ VA 0x140060300. This is the
                                      // discriminant AfdGetContext tests at
                                      // 0x14001E660 to return
                                      // STATUS_MORE_ENTRIES (0x00000105 —
                                      // success class) instead of
                                      // STATUS_SUCCESS on AF_UNIX endpoints.
                                      // It is ALSO the gate that afd.sys's
                                      // AfdSendMessage / AfdReceiveMessage
                                      // dispatchers consult to route
                                      // datagram-message ioctls away from
                                      // the stream-only afunix transport —
                                      // i.e. it is an afd.sys-side SAN-path
                                      // discriminant, not an afunix.sys
                                      // handler gate. The magic is stamped
                                      // by the kernel during NPI endpoint
                                      // initialisation; no user-mode
                                      // operation can set or clear it.
};

//===----------------------------------------------------------------------===//
// AF_UNIX Reparse-Point Wire Format — on-disk rendezvous object
//===----------------------------------------------------------------------===//
//
// afunix.sys materialises a pathname-bound AF_UNIX socket as a zero-length
// NTFS reparse point whose tag alone distinguishes it from a regular file.
// The reparse buffer carries NO payload — peer resolution happens later via
// `FileObject->FsContext2 ?: FileObject->FsContext` keyed into a global
// RB-tree (see AFUNIX_GLOBAL below). The tag's only job is to let
// `AfUnixAddressOpenFileInternal` reject non-socket files after opening.
//
// `IO_REPARSE_TAG_AF_UNIX` is also the ntifs.h-public tag value; confirmed by
// instruction-level decode of the literal pool at afunix.sys VA 0x14000A3DC
// (S2.1). The canonical definition lives in nt_file_types.h.

// 24-byte tag-only reparse data buffer written by
// afunix!AfUnixAddressCreateFile @ 0x14000A10C..0x14000A130 (S2.2 / S6.1).
// Allocated as NonPagedPoolNx, tag AFUNIX_POOL_TAG_WNPI, size 0x18.
struct AFUNIX_REPARSE_DATA_BUFFER {
  ULONG  ReparseTag;         // +0x00 = IO_REPARSE_TAG_AF_UNIX (0x80000023)
  USHORT ReparseDataLength;  // +0x04 = 0 (no payload)
  USHORT Reserved;           // +0x06 = 0
  UCHAR  DataBuffer[0x10];   // +0x08..+0x17, zero-initialised, unused
};

static_assert(sizeof(AFUNIX_REPARSE_DATA_BUFFER) == 0x18,
              "AF_UNIX reparse-point buffer must be 24 bytes tag-only");
static_assert(__builtin_offsetof(AFUNIX_REPARSE_DATA_BUFFER, ReparseTag) == 0x00,
              "AFUNIX_REPARSE_DATA_BUFFER.ReparseTag offset mismatch");
static_assert(__builtin_offsetof(AFUNIX_REPARSE_DATA_BUFFER,
                                 ReparseDataLength) == 0x04,
              "AFUNIX_REPARSE_DATA_BUFFER.ReparseDataLength offset mismatch");

//===----------------------------------------------------------------------===//
// FSRTL Atomic-Create ECP — GUID_ECP_ATOMIC_CREATE driven path
//===----------------------------------------------------------------------===//
//
// afunix.sys installs an extra-create parameter (ECP) on the NtCreateFile call
// that materialises the socket file, carrying the reparse buffer above so the
// filesystem can stamp the reparse tag *atomically* with the file create. This
// avoids the create+set-reparse race that would otherwise allow a peer to open
// a zero-tagged file mid-bind.
//
// Source: S2.3 / S2.4 / S6.1. ECP allocated via FsRtlAllocateExtraCreateParameter
// at afunix.sys VA 0x14000A180 (SizeOfContext = 0x58, NonPagedPool, tag WnpI).
// The ECP GUID literal lives in afunix.sys .rdata at VA 0x1400060D8.

// `GUID_ECP_ATOMIC_CREATE` = {4720BD83-52AC-4104-A130-D1EC6A8CC8E5}
// Same public kernel GUID documented in ntifs.h.
inline constexpr UCHAR AFUNIX_ECP_ATOMIC_CREATE_GUID[16] = {
    0x83, 0xBD, 0x20, 0x47, 0xAC, 0x52, 0x04, 0x41,
    0xA1, 0x30, 0xD1, 0xEC, 0x6A, 0x8C, 0xC8, 0xE5};

// Subset of FSRTL_ATOMIC_CREATE_ECP_CONTEXT (public ntifs.h type; layout is
// the public one — this struct exists purely so the decoded field accesses
// at afunix.sys VA 0x14000A1D8..0x14000A1F8 are self-describing in this tree).
struct AFUNIX_ATOMIC_CREATE_ECP_CONTEXT {
  USHORT Size;                 // +0x00 — set to 0x58 (88)
  USHORT InFlags;              // +0x02 — bit 1 =
                               //          ATOMIC_CREATE_ECP_IN_FLAG_REPARSE_POINT_SPECIFIED
  USHORT OutFlags;             // +0x04 — set by FS on return; bit 1 =
                               //          ATOMIC_CREATE_ECP_OUT_FLAG_REPARSE_POINT_SET
  USHORT ReparseBufferLength;  // +0x06 — set to 0x18 (sizeof AFUNIX_REPARSE_DATA_BUFFER)
  AFUNIX_REPARSE_DATA_BUFFER *ReparseBuffer; // +0x08
  // Remaining 0x48 bytes (+0x10..+0x57) left to the kernel for file/stream
  // metadata fields that afunix.sys does not populate.
};

inline constexpr USHORT ATOMIC_CREATE_ECP_IN_FLAG_REPARSE_POINT_SPECIFIED =
    0x0002;
inline constexpr USHORT ATOMIC_CREATE_ECP_OUT_FLAG_REPARSE_POINT_SET = 0x0002;

static_assert(sizeof(AFUNIX_ATOMIC_CREATE_ECP_CONTEXT::Size) == 2, "");
static_assert(__builtin_offsetof(AFUNIX_ATOMIC_CREATE_ECP_CONTEXT,
                                 ReparseBuffer) == 0x08,
              "ECP ReparseBuffer pointer offset mismatch");

//===----------------------------------------------------------------------===//
// AFD Open Packet Flags (extra_endpoint_flags for afd_open_endpoint)
//===----------------------------------------------------------------------===//

// Accept target — endpoint opened for receiving accepted connections.
inline constexpr ULONG AFD_OPEN_FLAG_ACCEPT_TARGET = 0x01000000;
// RIO-capable endpoint.
inline constexpr ULONG AFD_OPEN_FLAG_RIO = 0x10000000;

//===----------------------------------------------------------------------===//
// Bind Share Access
//===----------------------------------------------------------------------===//

enum AFD_BIND_SHARE_ACCESS : ULONG {
  AfdBindNormalAddressUse = 0,     // SO_REUSEADDR off
  AfdBindReuseAddress = 1,          // SO_REUSEADDR on
  AfdBindWildcardAddress = 2,       // Wildcard bind
  AfdBindExclusiveAddressUse = 3,   // SO_EXCLUSIVEADDRUSE
};

//===----------------------------------------------------------------------===//
// Socket Structures — TLI variants (SOCKADDR-based)
//===----------------------------------------------------------------------===//

// WSABUF equivalent — scatter/gather buffer descriptor.
// AFD reads {ULONG Length, PVOID Buffer} pairs. Natural alignment on x64:
// 16 bytes per entry (4 + 4 pad + 8).
struct AFD_WSABUF {
  ULONG Length;
  PVOID Buffer;
};

// Bind input (TLI mode) — ShareAccess + raw SOCKADDR.
// For AF_UNIX: SOCKADDR is sockaddr_un (2 + 108 = 110 bytes).
struct AFD_BIND_INFO_TL {
  AFD_BIND_SHARE_ACCESS ShareAccess;
  // Followed by raw SOCKADDR bytes (variable length by address family)
};

// Connect/join input (TLI mode).
struct AFD_CONNECT_JOIN_INFO_TL {
  BOOLEAN SanActive;       // 0 (SAN is obsolete)
  HANDLE RootEndpoint;     // 0 (not a multipoint join)
  HANDLE ConnectEndpoint;  // 0
  // Followed by raw SOCKADDR bytes (fixed-width for AF_UNIX = 110 bytes)
};

// Listen input.
struct AFD_LISTEN_INFO {
  BOOLEAN SanActive;                 // 0
  ULONG MaximumConnectionQueue;      // Backlog
  BOOLEAN UseDelayedAcceptance;      // 0
};

// Listen response (TLI mode) — returned by WAIT_FOR_LISTEN.
// Sequence is passed to AFD_ACCEPT_INFO to complete the accept.
struct AFD_LISTEN_RESPONSE_INFO_TL {
  LONG Sequence;
  // Followed by remote SOCKADDR bytes
};

// Accept input — completes a pending connection.
struct AFD_ACCEPT_INFO {
  BOOLEAN SanActive;     // 0
  LONG Sequence;          // From WAIT_FOR_LISTEN response
  HANDLE AcceptHandle;    // Handle of the pre-opened accept target endpoint
};

//===----------------------------------------------------------------------===//
// Send / Receive
//===----------------------------------------------------------------------===//

// AfdFlags for send/recv operations.
inline constexpr ULONG AFD_NO_FAST_IO = 0x0001;
inline constexpr ULONG AFD_OVERLAPPED = 0x0002;

// Receive input.
struct AFD_RECV_INFO {
  AFD_WSABUF *BufferArray;  // Scatter/gather buffer list
  ULONG BufferCount;
  ULONG AfdFlags;            // AFD_NO_FAST_IO | AFD_OVERLAPPED
  ULONG TdiFlags;            // TDI_RECEIVE_* flags
};

// Send input — same layout as AFD_RECV_INFO.
struct AFD_SEND_INFO {
  AFD_WSABUF *BufferArray;
  ULONG BufferCount;
  ULONG AfdFlags;
  ULONG TdiFlags;   // TDI_SEND_* flags
};

// TDI receive flags (from DDK tdi.h).
inline constexpr ULONG TDI_RECEIVE_NORMAL = 0x0020;           // In-band data (required)
inline constexpr ULONG TDI_RECEIVE_EXPEDITED = 0x0040;        // OOB data
inline constexpr ULONG TDI_RECEIVE_PEEK = 0x0080;             // MSG_PEEK (don't consume)
inline constexpr ULONG TDI_RECEIVE_ENTIRE_MESSAGE = 0x0400;   // MSG_WAITALL hint
inline constexpr ULONG TDI_RECEIVE_NO_PUSH = 0x4000;          // Complete only when buffer full

// TDI send flags.
inline constexpr ULONG TDI_SEND_EXPEDITED = 0x0020;           // OOB send
inline constexpr ULONG TDI_SEND_NON_BLOCKING = 0x0100;        // MSG_DONTWAIT
inline constexpr ULONG TDI_SEND_AND_DISCONNECT = 0x0200;      // Send + SHUT_WR

// Sendmsg/recvmsg input — extends AFD_DATAGRAM_INFO with ancillary buffers.
// Used with IOCTL_AFD_SEND_MESSAGE / IOCTL_AFD_RECEIVE_MESSAGE.
// On the datagram receive path, ControlLength and MsgFlags are writable in/out
// slots that let AFD publish control-buffer consumption and status such as
// truncation/cmsg truncation.
struct AFD_DATAGRAM_INFO {
  AFD_WSABUF *BufferArray;
  ULONG BufferCount;
  ULONG AfdFlags;
  ULONG TdiFlags;
  PVOID Address;          // SOCKADDR pointer
  ULONG *AddressLength;
};

struct AFD_MESSAGE_INFO {
  AFD_DATAGRAM_INFO dgi;
  PVOID ControlBuffer;    // Ancillary data buffer
  ULONG *ControlLength;   // In/out ancillary buffer length
  ULONG *MsgFlags;        // In/out message flags
};

//===----------------------------------------------------------------------===//
// Poll (select/poll equivalent)
//===----------------------------------------------------------------------===//

// Poll event bit numbers.
inline constexpr ULONG AFD_POLL_RECEIVE_BIT = 0;
inline constexpr ULONG AFD_POLL_RECEIVE_EXPEDITED_BIT = 1;
inline constexpr ULONG AFD_POLL_SEND_BIT = 2;
inline constexpr ULONG AFD_POLL_DISCONNECT_BIT = 3;
inline constexpr ULONG AFD_POLL_ABORT_BIT = 4;
inline constexpr ULONG AFD_POLL_LOCAL_CLOSE_BIT = 5;
inline constexpr ULONG AFD_POLL_CONNECT_BIT = 6;
inline constexpr ULONG AFD_POLL_ACCEPT_BIT = 7;
inline constexpr ULONG AFD_POLL_CONNECT_FAIL_BIT = 8;
inline constexpr ULONG AFD_POLL_QOS_BIT = 9;
inline constexpr ULONG AFD_POLL_GROUP_QOS_BIT = 10;
inline constexpr ULONG AFD_POLL_ROUTING_IF_CHANGE_BIT = 11;
inline constexpr ULONG AFD_POLL_ADDRESS_LIST_CHANGE_BIT = 12;
inline constexpr ULONG AFD_NUM_POLL_EVENTS = 13;

// Poll event flags — combine in AFD_POLL_HANDLE_INFO.PollEvents.
inline constexpr ULONG AFD_POLL_RECEIVE = (1 << AFD_POLL_RECEIVE_BIT);
inline constexpr ULONG AFD_POLL_RECEIVE_EXPEDITED = (1 << AFD_POLL_RECEIVE_EXPEDITED_BIT);
inline constexpr ULONG AFD_POLL_SEND = (1 << AFD_POLL_SEND_BIT);
inline constexpr ULONG AFD_POLL_DISCONNECT = (1 << AFD_POLL_DISCONNECT_BIT);
inline constexpr ULONG AFD_POLL_ABORT = (1 << AFD_POLL_ABORT_BIT);
inline constexpr ULONG AFD_POLL_LOCAL_CLOSE = (1 << AFD_POLL_LOCAL_CLOSE_BIT);
inline constexpr ULONG AFD_POLL_CONNECT = (1 << AFD_POLL_CONNECT_BIT);
inline constexpr ULONG AFD_POLL_ACCEPT = (1 << AFD_POLL_ACCEPT_BIT);
inline constexpr ULONG AFD_POLL_CONNECT_FAIL = (1 << AFD_POLL_CONNECT_FAIL_BIT);
inline constexpr ULONG AFD_POLL_QOS = (1 << AFD_POLL_QOS_BIT);
inline constexpr ULONG AFD_POLL_GROUP_QOS = (1 << AFD_POLL_GROUP_QOS_BIT);
inline constexpr ULONG AFD_POLL_ROUTING_IF_CHANGE = (1 << AFD_POLL_ROUTING_IF_CHANGE_BIT);
inline constexpr ULONG AFD_POLL_ADDRESS_LIST_CHANGE = (1 << AFD_POLL_ADDRESS_LIST_CHANGE_BIT);
inline constexpr ULONG AFD_POLL_ALL = ((1 << AFD_NUM_POLL_EVENTS) - 1);

// Per-handle entry in AFD_POLL_INFO.
struct AFD_POLL_HANDLE_INFO {
  HANDLE Handle;          // AFD socket handle
  ULONG PollEvents;       // Input: requested events; output: triggered events
  NTSTATUS Status;        // Output: per-handle NTSTATUS
};

// Input/output for IOCTL_AFD_POLL. The same buffer is used for both.
// Issued against a shared \Device\Afd\Mio handle, not the per-socket handle.
struct AFD_POLL_INFO {
  LARGE_INTEGER Timeout;   // 100ns ticks; INT64_MAX = infinite
  ULONG NumberOfHandles;
  BOOLEAN Unique;           // 0
  AFD_POLL_HANDLE_INFO Handles[1]; // Variable length
};

//===----------------------------------------------------------------------===//
// Disconnect (shutdown)
//===----------------------------------------------------------------------===//

// Disconnect mode flags.
inline constexpr ULONG AFD_PARTIAL_DISCONNECT_SEND = 0x01;    // shutdown(SHUT_WR)
inline constexpr ULONG AFD_PARTIAL_DISCONNECT_RECEIVE = 0x02; // shutdown(SHUT_RD)
inline constexpr ULONG AFD_ABORTIVE_DISCONNECT = 0x04;        // RST / SO_LINGER(0)
inline constexpr ULONG AFD_UNCONNECT_DATAGRAM = 0x08;         // Unconnect a datagram socket

struct AFD_PARTIAL_DISCONNECT_INFO {
  ULONG DisconnectMode;     // AFD_PARTIAL_DISCONNECT_* | AFD_ABORTIVE_DISCONNECT
  LARGE_INTEGER Timeout;    // -1 = infinite (100ns units)
};

//===----------------------------------------------------------------------===//
// Receive Information (FIONREAD)
//===----------------------------------------------------------------------===//

struct AFD_RECEIVE_INFORMATION {
  ULONG BytesAvailable;            // Pending readable bytes
  ULONG ExpeditedBytesAvailable;   // OOB bytes (0 for AF_UNIX)
};

//===----------------------------------------------------------------------===//
// Handle Query
//===----------------------------------------------------------------------===//

inline constexpr ULONG AFD_QUERY_ADDRESS_HANDLE = 0x01;
inline constexpr ULONG AFD_QUERY_CONNECTION_HANDLE = 0x02;

struct AFD_HANDLE_INFO {
  HANDLE TdiAddressHandle;
  HANDLE TdiConnectionHandle;
};

//===----------------------------------------------------------------------===//
// Socket Information (GET/SET_INFORMATION)
//===----------------------------------------------------------------------===//

// InformationType values for AFD_INFORMATION.
inline constexpr ULONG AFD_INLINE_MODE = 1;                    // s: BOOLEAN
inline constexpr ULONG AFD_NONBLOCKING_MODE = 2;               // s: BOOLEAN
inline constexpr ULONG AFD_MAX_SEND_SIZE = 3;                  // q: ULONG
inline constexpr ULONG AFD_SENDS_PENDING = 4;                  // q: ULONG
inline constexpr ULONG AFD_MAX_PATH_SEND_SIZE = 5;             // q: ULONG
inline constexpr ULONG AFD_RECEIVE_WINDOW_SIZE = 6;            // qs: ULONG
inline constexpr ULONG AFD_SEND_WINDOW_SIZE = 7;               // qs: ULONG
inline constexpr ULONG AFD_CONNECT_TIME = 8;                   // q: ULONG (seconds, 0xFFFFFFFF = not connected)
inline constexpr ULONG AFD_CIRCULAR_QUEUEING = 9;              // s: BOOLEAN
inline constexpr ULONG AFD_GROUP_ID_AND_TYPE = 10;             // q: AFD_GROUP_INFO
inline constexpr ULONG AFD_REPORT_PORT_UNREACHABLE = 11;       // s: BOOLEAN
inline constexpr ULONG AFD_REPORT_NETWORK_UNREACHABLE = 12;    // s: BOOLEAN
inline constexpr ULONG AFD_DELIVERY_STATUS = 14;               // q: SIO_DELIVERY_STATUS
inline constexpr ULONG AFD_CANCEL_TL = 15;                     // s: void (cancel pending op)

using GROUP = ULONG;

enum class AFD_GROUP_TYPE : ULONG {
  Neither = 0,
  Unconstrained = 0x01,
  Constrained = 0x02,
};

struct AFD_GROUP_INFO {
  GROUP GroupID;
  AFD_GROUP_TYPE GroupType;
};

struct SIO_DELIVERY_STATUS {
  BOOLEAN DeliveryAvailable;
  ULONG PendedReceiveRequests;
};

struct AFD_INFORMATION {
  ULONG InformationType;
  union {
    BOOLEAN Boolean;
    ULONG Ulong;
    LARGE_INTEGER LargeInteger;
    AFD_GROUP_INFO GroupInfo;
    SIO_DELIVERY_STATUS DeliveryStatus;
  } Information;
};

//===----------------------------------------------------------------------===//
// Event Select (WSAEventSelect equivalent)
//===----------------------------------------------------------------------===//

// Register a kernel event to be signaled when specific poll events occur.
struct AFD_EVENT_SELECT_INFO {
  HANDLE Event;           // Event handle to signal
  ULONG PollEvents;       // AFD_POLL_* bitmask
};

// Harvest triggered events and per-event status after event fires.
struct AFD_ENUM_NETWORK_EVENTS_INFO {
  ULONG PollEvents;                          // Triggered events bitmask
  NTSTATUS EventStatus[AFD_NUM_POLL_EVENTS]; // Per-event NTSTATUS
};

//===----------------------------------------------------------------------===//
// Super Accept (AcceptEx equivalent)
//===----------------------------------------------------------------------===//

// Async accept with optional initial data receive in a single operation.
struct AFD_SUPER_ACCEPT_INFO {
  BOOLEAN SanActive;            // 0
  BOOLEAN FixAddressAlignment;  // 0
  HANDLE AcceptHandle;           // Pre-opened accept target endpoint
  ULONG ReceiveDataLength;      // Bytes to receive with accept (0 = none)
  ULONG LocalAddressLength;     // Size of local address output
  ULONG RemoteAddressLength;    // Size of remote address output
};

//===----------------------------------------------------------------------===//
// Super Connect (ConnectEx equivalent)
//===----------------------------------------------------------------------===//

// Async connect with optional initial data send.
// TLI mode — uses raw SOCKADDR.
struct AFD_SUPER_CONNECT_INFO_TL {
  BOOLEAN SanActive;  // 0
  // Followed by raw SOCKADDR bytes
};

//===----------------------------------------------------------------------===//
// Super Disconnect (DisconnectEx equivalent)
//===----------------------------------------------------------------------===//

struct AFD_SUPER_DISCONNECT_INFO {
  ULONG Flags;  // Same as AFD_PARTIAL_DISCONNECT_* flags
};

//===----------------------------------------------------------------------===//
// Transmit File (sendfile equivalent)
//===----------------------------------------------------------------------===//

// Transmit flags.
inline constexpr ULONG AFD_TF_DISCONNECT = 0x01;          // Disconnect after send
inline constexpr ULONG AFD_TF_REUSE_SOCKET = 0x02;        // Allow socket reuse
inline constexpr ULONG AFD_TF_WRITE_BEHIND = 0x04;        // Don't wait for ACK
inline constexpr ULONG AFD_TF_USE_DEFAULT_WORKER = 0x00;
inline constexpr ULONG AFD_TF_USE_SYSTEM_THREAD = 0x10;
inline constexpr ULONG AFD_TF_USE_KERNEL_APC = 0x20;

struct AFD_TRANSMIT_FILE_INFO {
  LARGE_INTEGER Offset;          // File offset to start sending
  LARGE_INTEGER WriteLength;     // Bytes to send (0 = entire file)
  ULONG SendPacketLength;        // Packet size hint
  HANDLE FileHandle;              // File to transmit
  PVOID Head;                     // Header data to prepend
  ULONG HeadLength;
  PVOID Tail;                     // Trailer data to append
  ULONG TailLength;
  ULONG Flags;                   // AFD_TF_* flags
};

//===----------------------------------------------------------------------===//
// Deferred Accept
//===----------------------------------------------------------------------===//

struct AFD_DEFER_ACCEPT_INFO {
  LONG Sequence;     // Sequence from WAIT_FOR_LISTEN
  BOOLEAN Reject;    // TRUE to reject the connection
};

//===----------------------------------------------------------------------===//
// Unaccepted Connect Data
//===----------------------------------------------------------------------===//

struct AFD_UNACCEPTED_CONNECT_DATA_INFO {
  LONG Sequence;
  ULONG ConnectDataLength;
  BOOLEAN LengthOnly;  // TRUE = query length only
};

//===----------------------------------------------------------------------===//
// Transport IOCTL (setsockopt/getsockopt/ioctlsocket)
//===----------------------------------------------------------------------===//

// Control types for AFD_TL_IO_CONTROL_INFO.
enum TL_IO_CONTROL_TYPE : ULONG {
  TlEndpointIoControlType = 0,    // AF_UNIX private endpoint ioctls
  TlSetSockOptIoControlType = 1,  // setsockopt-style transport ops
  TlGetSockOptIoControlType = 2,  // getsockopt-style transport ops
  TlSocketIoControlType = 3,      // ioctlsocket-style transport ops
};

struct AFD_TL_IO_CONTROL_INFO {
  TL_IO_CONTROL_TYPE Type;
  ULONG Level;                      // SOL_* or IPPROTO_*
  ULONG IoControlCode;             // SO_*, IP_*, TCP_*, SIO_*, etc.
  BOOLEAN EndpointIoctl;            // Must be TRUE
  PVOID InputBuffer;
  SIZE_T InputBufferLength;
};

//===----------------------------------------------------------------------===//
// Buffered Transport-Change Registration Payload
//===----------------------------------------------------------------------===//
//
// Used with IOCTL_AFD_ROUTING_INTERFACE_CHANGE,
// IOCTL_AFD_ADDRESS_LIST_CHANGE, and IOCTL_AFD_SWITCH_ADDRLIST_CHANGE.
// This is distinct from IOCTL_AFD_TRANSPORT_IOCTL, which uses
// AFD_TL_IO_CONTROL_INFO.
//
// On the traced Windows 11 AfdAddressListChange / AfdRoutingInterfaceChange
// entry paths, the only proven consumer bit in AfdFlags is 0x2. When that bit
// is clear and the endpoint is in the internal "device not ready / IP
// availability gated" state, routing-interface change returns
// STATUS_DEVICE_NOT_READY and address-list change routes through the deferred
// IP-availability-consumer path instead of immediate provider registration.
// Setting the bit bypasses that gate. No other bits are read on those entry
// paths before the internal change-registration objects are materialized.
//
// On the traced SAN switch-address-list lane (IOCTL_AFD_SWITCH_ADDRLIST_CHANGE /
// AfdSanAddrListChange), no additional reads of a caller-provided AfdFlags
// dword were found. The observed SAN callers synthesize their own local control
// words (including hardcoded 0x2 mode values) from provider/transport state
// before entering the SAN helper, so there are currently no grounded SAN-only
// AfdFlags bits to publish in this header.
inline constexpr ULONG AFD_TRANSPORT_CHANGE_FLAG_BYPASS_DEVICE_NOT_READY_GATE =
    0x2;

struct AFD_TRANSPORT_IOCTL_INFO {
  HANDLE Handle;       // registration/callback handle carried by afd.sys
  PVOID InputBuffer;   // optional family-specific filter blob captured into
                       // the change registration
  ULONG InputBufferLength;
  ULONG IoControlCode; // change-source / provider query selector
  ULONG AfdFlags;      // bit 1 (0x2) bypasses the internal device-not-ready /
                       // IP-availability gate on traced route/address-change
                       // paths; no other bits are proven yet
  ULONG PollEvent;     // AFD_POLL_ROUTING_IF_CHANGE / _ADDRESS_LIST_CHANGE
};

static_assert(sizeof(AFD_TRANSPORT_IOCTL_INFO) == 0x20,
              "AFD transport-change ioctl info must stay 0x20 bytes on Win64");

//===----------------------------------------------------------------------===//
// Unbind
//===----------------------------------------------------------------------===//

struct AFD_UNBIND_INFO {
  LONG AddressFamily;  // AF_*
  LONG Protocol;       // IPPROTO_*
};

//===----------------------------------------------------------------------===//
// Socket State (for SET_CONTEXT/GET_CONTEXT)
//===----------------------------------------------------------------------===//

enum SOCKET_STATE : LONG {
  SocketStateInitializing = -1,
  SocketStateOpen = 0,
  SocketStateBound = 1,
  SocketStateBoundSpecific = 2,
  SocketStateConnected = 3,
  SocketStateClosing = 4,
};

inline constexpr SOCKET_STATE AFD_UNIX_CONTEXT_STATE_OPEN = SocketStateOpen;
inline constexpr SOCKET_STATE AFD_UNIX_CONTEXT_STATE_BOUND =
    SocketStateBound; // bound and listening share this state
inline constexpr SOCKET_STATE AFD_UNIX_CONTEXT_STATE_CONNECTED =
    SocketStateConnected;

// SOCK_SHARED_INFO — the context blob for GET_CONTEXT/SET_CONTEXT.
// This is the Winsock-level socket state that AFD caches per-endpoint.
struct SOCK_SHARED_INFO {
  SOCKET_STATE State;
  LONG AddressFamily;
  LONG SocketType;
  LONG Protocol;
  LONG LocalAddressLength;
  LONG RemoteAddressLength;
  struct {
    USHORT l_onoff;
    USHORT l_linger;
  } LingerInfo;
  ULONG SendTimeout;         // Milliseconds
  ULONG ReceiveTimeout;      // Milliseconds
  ULONG ReceiveBufferSize;
  ULONG SendBufferSize;
  union {
    USHORT Flags;
    struct {
      USHORT Listening : 1;
      USHORT Broadcast : 1;
      USHORT Debug : 1;
      USHORT OobInline : 1;
      USHORT ReuseAddresses : 1;
      USHORT ExclusiveAddressUse : 1;
      USHORT NonBlocking : 1;
      USHORT DontUseWildcard : 1;
      USHORT ReceiveShutdown : 1;
      USHORT SendShutdown : 1;
      USHORT ConditionalAccept : 1;
      USHORT IsSANSocket : 1;
      USHORT fIsTLI : 1;
      USHORT Rio : 1;
      USHORT ReceiveBufferSizeSet : 1;
      USHORT SendBufferSizeSet : 1;
    };
  };
  ULONG CreationFlags;        // WSA_FLAG_*
  ULONG CatalogEntryId;
  ULONG ServiceFlags1;        // XP1_*
  ULONG ProviderFlags;        // PFL_*
  ULONG GroupID;
  LONG GroupType;
  LONG GroupPriority;
  LONG LastError;
  union {
    PVOID AsyncSelecthWnd;     // HWND
    ULONGLONG AsyncSelectWnd64;
  };
  ULONG AsyncSelectSerialNumber;
  ULONG AsyncSelectwMsg;
  LONG AsyncSelectlEvent;
  LONG DisabledAsyncSelectEvents;
  UCHAR ProviderId[16];       // GUID
};

static_assert(sizeof(SOCK_SHARED_INFO) == 0x78,
              "AFD GET/SET_CONTEXT uses a 0x78-byte SOCK_SHARED_INFO prefix");
static_assert(__builtin_offsetof(SOCK_SHARED_INFO, ReceiveBufferSize) == 0x24,
              "SOCK_SHARED_INFO.ReceiveBufferSize offset mismatch");
static_assert(__builtin_offsetof(SOCK_SHARED_INFO, SendBufferSize) == 0x28,
              "SOCK_SHARED_INFO.SendBufferSize offset mismatch");
static_assert(__builtin_offsetof(SOCK_SHARED_INFO, Flags) == 0x2C,
              "SOCK_SHARED_INFO.Flags offset mismatch");
static_assert(__builtin_offsetof(SOCK_SHARED_INFO, ProviderId) == 0x68,
              "SOCK_SHARED_INFO.ProviderId offset mismatch");

// Fixed 0x80-byte prefix used by current AFD SET_CONTEXT/GET_CONTEXT buffers.
// Only the SOCK_SHARED_INFO prefix is reflected in public/third-party headers.
// The trailing dwords are producer-side publication plumbing:
//   +0x78 = selector/query-blob length
//   +0x7C = zero-filled padding so appended tail blocks begin at +0x80
//
// On current Windows 11 builds, mswsock!SockSetHandleContext zeroes the full
// 0x80-byte prefix, copies only the 0x78-byte SOCK_SHARED_INFO prefix, writes
// SelectorBlobLength at +0x78, and starts the first appended block at +0x80.
// afd.sys does not consume +0x7C; it round-trips only because AfdSetContext
// caches the caller's query blob verbatim. A runtime mutation matrix on Win11
// also confirmed that varying +0x7C across 0, 1, 0x7FF8, 0xBAADF00D, and
// 0xFFFFFFFF changes only the bytes returned by GET_CONTEXT, not bind/connect/
// accept/data/getsockname behavior.
//
// Important layering note: the only semantic "0x7C word" we can currently
// ground in the Win32 AF_UNIX publication path is not this header field. In
// the standalone 0x80-byte WSHGetSocketInformation(option=1) snapshot, bytes
// 0x78..0x7F are the 64-bit RemotePathFixup pointer, so snapshot byte 0x7C is
// simply that pointer's upper 32 bits on 64-bit builds. That does not apply to
// AFD_CONTEXT_HEADER+0x7C.
struct AFD_CONTEXT_HEADER {
  SOCK_SHARED_INFO SharedInfo; // 0x00..0x77
  ULONG SelectorBlobLength;    // 0x78
  ULONG TailAlignmentPadding;  // 0x7C, zero-filled so tail blocks start +0x80
};

static_assert(sizeof(AFD_CONTEXT_HEADER) == 0x80,
              "AFD context header must stay 0x80 bytes");

// Partial owner for the cached publication/query surface used by
// IOCTL_AFD_GET_CONTEXT / IOCTL_AFD_GET_REMOTE_ADDRESS.
//
// AfdLockEndpointContext/AfdUnlockEndpointContext operate on the pointer slot at
// +0xE0, not on the whole object. The slot carries either the current query
// blob pointer or one of two sentinels. AfdSetContext and AfdGetContext use
// that lock domain when replacing/copying the blob.
//
// QueryRemoteOffset/QueryRemoteLength have a second synchronization domain at
// +0x168. AfdSetContext takes it as an exclusive writer with 0 -> 1, while
// AfdGetRemoteAddress takes it as a reader with 0 -> -1 and increments it back
// toward 0 on release.
inline constexpr LONG_PTR AFD_QUERY_BLOB_LOCKED_SENTINEL = -1;
inline constexpr LONG_PTR AFD_QUERY_BLOB_WAITERS_SENTINEL = -2;

struct AFD_ENDPOINT_CONTEXT_QUERY_CACHE {
  USHORT ContextSignature; // observed 0xAFD0/0xAFD1/0xAFD2/0x1AFD subtypes
  UCHAR ContextSubtype;    // observed 1/2/4 depending on the owner subtype
  UCHAR Reserved03;
  ULONG ContextFlags;      // bit 8 participates in remote-query publication
  UCHAR Reserved08[4];
  UCHAR Reserved0C[0xA4];
  ULONG ContextInfoFlags;  // auxiliary info flags touched by set-info paths
  ULONG ReservedB4;
  USHORT QueryRemoteOffset;       // +0xB8
  USHORT QueryRemoteLength;       // +0xBA
  UCHAR ReservedBC[0x24];
  PVOID QueryBlobPointerOrSentinel; // +0xE0: blob pointer, -1 locked, -2 waiters
  ULONG QueryBlobTotalLength;       // +0xE8
  UCHAR ReservedEC[0x7C];
  LONG QueryRemotePublishGate; // +0x168: 0 idle, 1 writer, negative readers
};

using AFD_PRIVATE_ENDPOINT_QUERY_CACHE = AFD_ENDPOINT_CONTEXT_QUERY_CACHE;

static_assert(__builtin_offsetof(AFD_ENDPOINT_CONTEXT_QUERY_CACHE,
                                 ContextInfoFlags) == 0xB0,
              "AFD endpoint-context info-flags offset mismatch");
static_assert(__builtin_offsetof(AFD_ENDPOINT_CONTEXT_QUERY_CACHE,
                                 QueryRemoteOffset) == 0xB8,
              "AFD query cache remote offset mismatch");
static_assert(__builtin_offsetof(AFD_ENDPOINT_CONTEXT_QUERY_CACHE,
                                 QueryBlobPointerOrSentinel) == 0xE0,
              "AFD query cache blob pointer mismatch");
static_assert(__builtin_offsetof(AFD_ENDPOINT_CONTEXT_QUERY_CACHE,
                                 QueryBlobTotalLength) == 0xE8,
              "AFD query cache blob length mismatch");
static_assert(__builtin_offsetof(AFD_ENDPOINT_CONTEXT_QUERY_CACHE,
                                 QueryRemotePublishGate) == 0x168,
              "AFD query cache publish-gate offset mismatch");

inline constexpr ULONG AFD_UNIX_CONTEXT_ADDRESS_SLOT_LENGTH = 0x70;
inline constexpr ULONG AFD_UNIX_CONTEXT_LOCAL_ADDRESS_OFFSET = 0x80;
inline constexpr ULONG AFD_UNIX_CONTEXT_REMOTE_ADDRESS_OFFSET = 0xF0;
inline constexpr ULONG AFD_UNIX_CONTEXT_SELECTOR_OFFSET = 0x160;

// Kernel contract for the selector tail (what afd.sys / afunix.sys actually
// require). Phase A.1 + Phase C.1 decode: neither driver reads a single byte
// of the selector — it is pure opaque memcpy storage (AfdSetContext @
// 0x14001FCD0, AfdGetContext @ 0x14001E5F0). Empirical probe on the live
// driver (S3.11) further shows that only the leading INT32 at offset 0 is
// observable; bytes +0x04 onward can be any value with no behavioural effect.
//
// We therefore publish a 4-byte selector carrying just AF_UNIX. Foreign
// producers (Winsock / mswsock via WSADuplicateSocket → wshunix.dll) may
// publish 0x80-byte or longer blobs; context_is_plausible() accepts both.
inline constexpr ULONG AFD_UNIX_CONTEXT_MIN_SELECTOR_LENGTH = sizeof(LONG);
inline constexpr ULONG AFD_UNIX_CONTEXT_SELECTOR_LENGTH =
    AFD_UNIX_CONTEXT_MIN_SELECTOR_LENGTH;

// Size of the Winsock/wshunix snapshot header defined below — *not* our
// selector size, kept as a distinct constant so consumers reading a
// foreign-producer blob can validate it against the right expected length.
inline constexpr ULONG WSHUNIX_SOCKET_CONTEXT_WIN11_SIZE = 0x80;
inline constexpr ULONG WSHUNIX_SOCKET_PATH_STATE_WIN11_SIZE = 0x70;

// AF_UNIX selector blob — the 4-byte kernel contract.
//
// On producer: we write one of these into AFD_UNIX_CONTEXT_IMAGE.SelectorBlob
// and set AFD_CONTEXT_HEADER.SelectorBlobLength = 4. This is the minimum that
// context_is_plausible() and any future Winsock-SPI reader will accept as a
// well-formed AF_UNIX publication.
//
// On consumer: when reading back a selector produced by us the length is 4.
// When reading back a selector produced by Winsock/wshunix.dll the length is
// 0x80 or larger and the bytes overlay WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER
// (declared below) — context_is_plausible() branches on the returned length
// to validate each shape independently.
struct AFD_UNIX_SELECTOR_BLOB {
  LONG AddressFamilyValue; // +0x00 = AF_UNIX
};

static_assert(sizeof(AFD_UNIX_SELECTOR_BLOB) ==
                  AFD_UNIX_CONTEXT_SELECTOR_LENGTH,
              "AFD AF_UNIX selector blob size mismatch");

// Shared semantic prefix of the Win11 wshunix AF_UNIX helper and its
// option=1 serialized snapshot header.
//
// This is a user-mode pathname cache:
// - AddressFamilyValue identifies AF_UNIX
// - PathLock is a real RTL_RESOURCE, not anonymous helper bytes
// - Local/RemotePathByteCount are the canonical UTF-16 NT path lengths
//
// The natural alignment of RTL_RESOURCE and PWSTR introduces 4-byte padding
// holes before PathLock and before the trailing path pointers. They are layout
// padding, not semantic state.
struct WSHUNIX_SOCKET_PATH_STATE_WIN11 {
  LONG AddressFamilyValue;     // AF_UNIX
  RTL_RESOURCE PathLock;       // protects local/remote path cache
  USHORT LocalPathByteCount;   // UTF-16 byte count, no trailing NUL
  USHORT RemotePathByteCount;  // UTF-16 byte count, no trailing NUL
};

static_assert(sizeof(WSHUNIX_SOCKET_PATH_STATE_WIN11) ==
                  WSHUNIX_SOCKET_PATH_STATE_WIN11_SIZE,
              "WSHUNIX path-state size mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_PATH_STATE_WIN11,
                                 AddressFamilyValue) == 0x00,
              "WSHUNIX path-state AF offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_PATH_STATE_WIN11, PathLock) ==
                  0x08,
              "WSHUNIX path-state lock offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_PATH_STATE_WIN11,
                                 LocalPathByteCount) == 0x68,
              "WSHUNIX path-state local-length offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_PATH_STATE_WIN11,
                                 RemotePathByteCount) == 0x6A,
              "WSHUNIX path-state remote-length offset mismatch");

// Fixed 0x80-byte header of the WSHGetSocketInformation(option=1) serialized
// snapshot used by mswsock.dll. The full snapshot size is:
//   0x80 + LocalPathByteCount + RemotePathByteCount
//
// The trailing path bytes are canonical UTF-16 NT pathname bytes with lengths
// excluding the terminating NUL. LocalPathFixup and RemotePathFixup are not
// semantic path fields from AFD's point of view; they are live in-buffer
// fixups consumed directly by WSHSetSocketInformation(option=1) when rebuilding
// a helper from a snapshot.
// On current 64-bit builds, snapshot byte 0x7C is therefore just the upper
// 32 bits of RemotePathFixup.
// Because AFD only caches bytes, those fixups are not a durable ABI contract
// across arbitrary copies or round-trips.
struct WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER {
  WSHUNIX_SOCKET_PATH_STATE_WIN11 PathState;
  PWSTR LocalPathFixup;  // in-buffer fixup for option=1 snapshot
  PWSTR RemotePathFixup; // in-buffer fixup for option=1 snapshot
};

static_assert(sizeof(WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER) ==
                  WSHUNIX_SOCKET_CONTEXT_WIN11_SIZE,
              "WSHUNIX snapshot header size mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER,
                                 PathState.LocalPathByteCount) == 0x68,
              "WSHUNIX snapshot local-path length offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER,
                                 PathState.RemotePathByteCount) == 0x6A,
              "WSHUNIX snapshot remote-path length offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER,
                                 LocalPathFixup) == 0x70,
              "WSHUNIX snapshot local-path fixup offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER,
                                 RemotePathFixup) == 0x78,
              "WSHUNIX snapshot remote-path fixup offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_SNAPSHOT_HEADER,
                                 RemotePathFixup) + sizeof(ULONG) == 0x7C,
              "WSHUNIX snapshot remote-path fixup high-dword offset mismatch");

// Private AF_UNIX helper object used by wshunix.dll on Windows 11 22H2+/24H2.
// This is distinct from the AFD selector slot above. WSHOpenSocket2 allocates
// this 0x80-byte object, requests WSH_NOTIFY_BIND | WSH_NOTIFY_CONNECT |
// WSH_NOTIFY_CLOSE (0x85), and uses the object as a user-mode cache for
// normalized NT pathname state.
//
// WSHGetSocketInformation(level=0xFFFE, option=1) serializes this fixed header
// followed by LocalPathByteCount + RemotePathByteCount bytes. In the
// serialized image, LocalPathFixup and RemotePathFixup are in-buffer fixups to
// the appended UTF-16 NT pathname bytes, and the copied PathLock bytes are
// compatibility scaffolding rather than transport semantics.
//
// WSHSetSocketInformation(level=0xFFFE, option=0x20000001/0x20000002) is the
// meaningful path-update API: it trims one trailing '/' or '\\', converts the
// UTF-8 pathname to UTF-16, canonicalizes it with
// RtlDosPathNameToNtPathName_U_WithStatus, and stores the resulting NT path in
// the selected slot with a length that excludes the terminating NUL.
//
// The option=1 restore path is semantically meaningful but narrower than the
// getter: current Win11 builds reinitialize the resource object and then copy
// local/remote path bytes through the in-buffer fixups above. The helper's live
// semantics therefore come from the local/remote path slots, not from the
// copied resource payload.
struct WSHUNIX_SOCKET_CONTEXT_WIN11 {
  WSHUNIX_SOCKET_PATH_STATE_WIN11 PathState;
  PWSTR LocalPath;   // live local NT path buffer
  PWSTR RemotePath;  // live remote NT path buffer
};

static_assert(sizeof(WSHUNIX_SOCKET_CONTEXT_WIN11) ==
                  WSHUNIX_SOCKET_CONTEXT_WIN11_SIZE,
              "WSHUNIX Win11 socket context size mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_WIN11,
                                 PathState.LocalPathByteCount) == 0x68,
              "WSHUNIX local-path length offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_WIN11,
                                 PathState.RemotePathByteCount) == 0x6A,
              "WSHUNIX remote-path length offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_WIN11, LocalPath) ==
                  0x70,
              "WSHUNIX local-path pointer offset mismatch");
static_assert(__builtin_offsetof(WSHUNIX_SOCKET_CONTEXT_WIN11, RemotePath) ==
                  0x78,
              "WSHUNIX remote-path pointer offset mismatch");

// Minimal libc AF_UNIX context publication image.
// This is not a kernel-decoded AFD ABI type. AfdSetContext / AfdGetContext
// cache and return this query blob verbatim; the fixed local/remote slots are
// the stable prefix we rely on, while Win32/mswsock may append a selector blob
// longer than 0x80 when publishing the full wshunix snapshot.
// Packed to suppress the 4-byte tail padding the 8-byte-aligned Header would
// otherwise introduce — the kernel contract for this image is exactly 356
// bytes (0x80 header + 2 * 0x70 address slots + 4-byte selector). All fields
// still land at their natural offsets because AFD IOCTL buffers come from
// 8-aligned heap allocations in practice.
struct __attribute__((packed)) AFD_UNIX_CONTEXT_IMAGE {
  AFD_CONTEXT_HEADER Header; // 0x000..0x07F
  UCHAR LocalAddress[AFD_UNIX_CONTEXT_ADDRESS_SLOT_LENGTH];
  UCHAR RemoteAddress[AFD_UNIX_CONTEXT_ADDRESS_SLOT_LENGTH];
  UCHAR SelectorBlob[AFD_UNIX_CONTEXT_SELECTOR_LENGTH]; // minimal selector header
};

static_assert(sizeof(AFD_UNIX_CONTEXT_IMAGE) ==
                  sizeof(AFD_CONTEXT_HEADER) +
                      (AFD_UNIX_CONTEXT_ADDRESS_SLOT_LENGTH * 2) +
                      AFD_UNIX_CONTEXT_SELECTOR_LENGTH,
              "AFD AF_UNIX context image size mismatch");
static_assert(__builtin_offsetof(AFD_UNIX_CONTEXT_IMAGE, LocalAddress) ==
                  AFD_UNIX_CONTEXT_LOCAL_ADDRESS_OFFSET,
              "AFD AF_UNIX local address offset mismatch");
static_assert(__builtin_offsetof(AFD_UNIX_CONTEXT_IMAGE, RemoteAddress) ==
                  AFD_UNIX_CONTEXT_REMOTE_ADDRESS_OFFSET,
              "AFD AF_UNIX remote address offset mismatch");
static_assert(__builtin_offsetof(AFD_UNIX_CONTEXT_IMAGE, SelectorBlob) ==
                  AFD_UNIX_CONTEXT_SELECTOR_OFFSET,
              "AFD AF_UNIX selector blob offset mismatch");

//===----------------------------------------------------------------------===//
// AFUNIX_ENDPOINT — kernel-internal endpoint object (informational)
//===----------------------------------------------------------------------===//
//
// This is afunix.sys's private kernel-resident endpoint struct. User-mode code
// never constructs, inspects, or embeds it — everything below exists to make
// the decoded field accesses at known VAs self-describing in this tree.
//
// Allocated by `AfUnixEndpointCreate` @ afunix.sys 10.0.26100.6725, VA
// 0x14000C620. Backing store is NonPagedPoolNx, pool tag AFUNIX_POOL_TAG_WNPI.
// Three sizes by endpoint kind (selected via csel at VA 0x14000C650..+0x60):
//
//   Kind 1 (Base / pre-bind / idle)   size 0x100 (256 B)
//   Kind 2 (Connection / per-peer)    size 0x230 (560 B)
//   Kind 3 (Listening)                size 0x118 (280 B)
//
// The common base runs 0x000..0x107. Kind-2 and Kind-3 extend past +0x108.
// See AF_UNIX_DECODE_NOTES.md S5.9 / S6 / S7 for full evidence citations.

// Connection lifecycle state codes stored at Endpoint[+0x100]. Observed values
// from S4.4/S5.7/S5.9 trace stores and shutdown paths.
enum AFUNIX_ENDPOINT_STATE : ULONG {
  AFUNIX_STATE_FREE                  = 0,
  AFUNIX_STATE_LISTENING             = 1,  // Kind-3 only
  AFUNIX_STATE_CONNECTED             = 4,
  AFUNIX_STATE_DISCONNECTING         = 5,
  AFUNIX_STATE_DISCONNECT_DELIVERED  = 6,
  AFUNIX_STATE_HALF_CONNECTED        = 7,
  AFUNIX_STATE_SHUTTING_DOWN         = 8,
  AFUNIX_STATE_DRAINING              = 9,
  AFUNIX_STATE_CLOSED                = 10,
};

// Shutdown-flags bitfield at Endpoint[+0x104]. Each bit latches a peer-facing
// NTSTATUS the afunix data plane returns on subsequent sends (S5.8 / S6.6b).
//
//   bit 0 — peer RCV_SHUTDOWN observed        → STATUS_PIPE_BROKEN (0xC000014B)
//   bit 1 — self SHUT_WR pending              → STATUS_PIPE_CLOSING
//   bit 2 — disconnect acknowledged (abort)   → STATUS_CONNECTION_DISCONNECTED
//   bit 3 — connection reset                  → STATUS_CONNECTION_RESET
inline constexpr ULONG AFUNIX_SHUTDOWN_PEER_RCV   = 0x1;
inline constexpr ULONG AFUNIX_SHUTDOWN_SELF_WR    = 0x2;
inline constexpr ULONG AFUNIX_SHUTDOWN_ABORTED    = 0x4;
inline constexpr ULONG AFUNIX_SHUTDOWN_RESET      = 0x8;

// Common-base layout (all three kinds). Do not `sizeof` or instantiate this —
// the Kind-2 extension overlays at +0x108 make the full object size
// kind-dependent, and the layout is kernel-private. This struct exists so the
// offsets are parsable by tooling and so future readers of the decode can
// locate the exact field for any kernel-side access.
struct AFUNIX_ENDPOINT_COMMON {
  ULONG64 volatile RefCount;          // +0x000 LDADDL atomic fetch-sub at
                                      //          AfUnixEndpointDereference
                                      //          @ 0x14000C814.
  ULONG64 PushLock;                   // +0x008 EX_PUSH_LOCK (ExAcquire... at
                                      //          IAT[+0x98]).
  PVOID   Owner;                      // +0x010 AFUNIX_GLOBAL *
  PVOID   NameSelector;               // +0x018 ObReferenced.

  // Path-prime slots populated by AfUnixEndpointSetFilePathOption
  // (optname 0x98000000). Consumed destructively by GetFileProperties
  // @ 0x14000C9F0 during bind/connect. Disjoint from EaBuffer (+0xD0).
  PFILE_OBJECT RefdFileObject;        // +0x020
  USHORT  ExtraLen;                   // +0x028
  USHORT  ExtraLenDup;                // +0x02A (duplicate of ExtraLen — a
                                      //         typed-header discriminant)
  ULONG   Reserved2C;                 // +0x02C always 0
  PVOID   ExtraData;                  // +0x030 pool (tag WnpI). NULL = prime
                                      //          not set — the "gate" read by
                                      //          GetFileProperties to emit
                                      //          "file path socket option not
                                      //          set".

  UCHAR   BoundSockaddr[0x6E];        // +0x038..+0x0A5 raw sockaddr_un
                                      //          (family+sun_path, 110 B)
  UCHAR   Pad0A6[2];                  // +0x0A6
  PFILE_OBJECT BindFileObject;        // +0x0A8 NULL if abstract/unbound
  UCHAR   AddressRbNode[0x18];        // +0x0B0 RTL_BALANCED_NODE —
                                      //          AFUNIX_GLOBAL::AddressTree
  ULONG_PTR AddressSelector;          // +0x0C8 FsContext2 ?: FsContext of
                                      //          backing file — RB-tree key

  // EaBuffer slots populated by AfUnixEndpointSetEaBuffer (optname 0x98000001).
  // DISJOINT from the prime tuple above. ZERO readers in any decoded afunix
  // code path — effectively dead write-only storage (see S8).
  PVOID   EaBuffer;                   // +0x0D0 pool (tag WnpI)
  ULONG   EaBufferLen;                // +0x0D8 ≤ UINT32_MAX

  ULONG   Kind;                       // +0x0DC 1 / 2 / 3
  UCHAR   FileObjectBit;              // +0x0E0 1 = bound (path or abstract)
  UCHAR   Pad0E1[7];                  // +0x0E1
  PVOID   CallbackSlot;               // +0x0E8 AfUnixEndpointCreate arg3
  PVOID   ProviderDispatchVt;         // +0x0F0
  PVOID   ProviderDispatchCtx;        // +0x0F8
  ULONG   State;                      // +0x100 AFUNIX_ENDPOINT_STATE
  ULONG   ShutdownFlags;              // +0x104 AFUNIX_SHUTDOWN_* bits
};

static_assert(sizeof(AFUNIX_ENDPOINT_COMMON) == 0x108,
              "AFUNIX_ENDPOINT common base must be 0x108 bytes — the Kind-1 "
              "object is sized 0x100 and aliases past the trailing 8 bytes, "
              "so this struct is strictly informational");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, RefdFileObject) == 0x020, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, ExtraData)      == 0x030, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, BoundSockaddr)  == 0x038, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, BindFileObject) == 0x0A8, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, AddressRbNode)  == 0x0B0, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, AddressSelector)== 0x0C8, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, EaBuffer)       == 0x0D0, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, EaBufferLen)    == 0x0D8, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, Kind)           == 0x0DC, "");
static_assert(__builtin_offsetof(AFUNIX_ENDPOINT_COMMON, State)          == 0x100, "");

// Kind-3 (listening) — 280-byte endpoint. Extends the base by 16 bytes that
// latch the TL_LISTEN_REQUEST-propagated local-address pair.
struct AFUNIX_LISTEN_ENDPOINT_TAIL {
  PVOID Listen_LocalAddrA;            // +0x108 from TL_LISTEN_REQUEST+0x58
  PVOID Listen_LocalAddrB;            // +0x110 from TL_LISTEN_REQUEST+0x48
};

// Kind-2 (connection) — 560-byte endpoint. Adds peer linkage, mirrored remote
// sockaddr, and the send/recv queues that drive the data plane.
struct AFUNIX_CONN_ENDPOINT_TAIL {
  UCHAR                     Conn_RemoteAddr[0x6E]; // +0x108 mirrored sockaddr_un
  UCHAR                     Pad176[2];             // +0x176
  PVOID                     Conn_LocalAddrB;       // +0x178 TL_CONNECT+0x70
  PVOID                     Conn_LocalAddrA;       // +0x180 TL_CONNECT+0x48
  struct AFUNIX_ENDPOINT   *Peer;                  // +0x188 set reciprocally
  PVOID                     RecvQueueHead;         // +0x190 RecvRequestNode *
  PVOID                     RecvQueueTail;         // +0x198
  SIZE_T                    SendQueuedBytes;       // +0x1A0 flow-control total
  UCHAR                     Pad1A8[8];             // +0x1A8
  PVOID                     SendQueueHead;         // +0x1B0 SendRequestNode *
  PVOID                     SendQueueTail;         // +0x1B8
  ULONG                     StateFlags;            // +0x1C0 bit 0 EOF consumed,
                                                   //        bit 1 delivering
  UCHAR                     Pad1C4[0x24];          // +0x1C4
  PVOID                     DisconnectNotifyFn;    // +0x1E8
  PVOID                     DisconnectNotifyArg;   // +0x1F0
  UCHAR                     Pad1F8[0x30];          // +0x1F8
  PFILE_OBJECT              PeerFileObject;        // +0x228 ObReferenced
};

static_assert(__builtin_offsetof(AFUNIX_CONN_ENDPOINT_TAIL, Peer)
                  == 0x188 - 0x108, "Conn endpoint Peer offset mismatch");
static_assert(__builtin_offsetof(AFUNIX_CONN_ENDPOINT_TAIL, SendQueuedBytes)
                  == 0x1A0 - 0x108, "Conn endpoint SendQueuedBytes offset");

//===----------------------------------------------------------------------===//
// AFUNIX_GLOBAL — per-transport global state (informational)
//===----------------------------------------------------------------------===//
//
// afunix.sys holds one of these per loaded transport module. Carries the
// global lock and the RB-tree of address records. The RB-tree is keyed on
// `FileObject->FsContext2 ?: FileObject->FsContext` (S6.2) — not on pathname
// text, not on handle value. Two independent opens of the same NTFS stream
// share the same FsContext and therefore map to the same tree slot, which is
// why connect() from a different process reaches the same listener regardless
// of path normalisation or case.
struct AFUNIX_GLOBAL {
  UCHAR       Pad00[0x08];
  ULONG64     GlobalLock;            // +0x08 EX_PUSH_LOCK
  UCHAR       Pad10[0x08];
  // +0x18 RTL_RB_TREE AddressTree — layout is the public RTL type; elided
  // here to avoid pulling in the full RTL_RB_TREE definition.
};

//===----------------------------------------------------------------------===//
// afunix data-plane queue nodes (informational)
//===----------------------------------------------------------------------===//
//
// These are the kernel queue nodes chained into
// AFUNIX_CONN_ENDPOINT_TAIL::{Send,Recv}Queue{Head,Tail}. Not reachable from
// user mode; documented here so that trace-walking session dumps can be
// interpreted.
//
// Source: S6.5 (send/recv layout) + S7.2 (revised field names after
// AfUnixDeliverDataToClient decode). Pool tag AFUNIX_POOL_TAG_WNPI.

struct AFUNIX_SEND_REQUEST_NODE {    // 0x48 bytes, chained via +0x00
  struct AFUNIX_SEND_REQUEST_NODE *Next; // +0x00
  PVOID     Buffer_or_MDL;                // +0x08
  ULONG     Offset_A;                     // +0x10 sender offset
  UCHAR     Pad14[4];                     // +0x14
  SIZE_T    RemainingSize;                // +0x18
  ULONG     Offset_B;                     // +0x20 mirrored counter
  UCHAR     Pad24[4];                     // +0x24
  SIZE_T    BufferEnd;                    // +0x28
  SIZE_T    BufferCursor;                 // +0x30
  ULONG     Flags_or_Zero;                // +0x38
  UCHAR     Pad3C[4];                     // +0x3C
  ULONG_PTR Reserved;                     // +0x40
};
static_assert(sizeof(AFUNIX_SEND_REQUEST_NODE) == 0x48,
              "AFUNIX send request node must be 0x48 bytes");

struct AFUNIX_RECV_REQUEST_NODE {    // 0x58 bytes, chained via +0x50
  PVOID  OriginalIrp;                     // +0x00
  PVOID  AuxContext;                      // +0x08
  ULONG  Flags;                           // +0x10 bit 2 = abort/skip marker
  UCHAR  Pad14[4];                        // +0x14
  PVOID  Buffer_or_MDL;                   // +0x18
  ULONG  Offset;                          // +0x20
  UCHAR  Pad24[4];                        // +0x24
  SIZE_T BufferEnd;                       // +0x28
  SIZE_T BufferCursor;                    // +0x30
  UCHAR  Pad38[0x18];                     // +0x38
  struct AFUNIX_RECV_REQUEST_NODE *Next;  // +0x50
};
static_assert(sizeof(AFUNIX_RECV_REQUEST_NODE) == 0x58,
              "AFUNIX recv request node must be 0x58 bytes");

//===----------------------------------------------------------------------===//
// RIO (Registered I/O) Commands
//===----------------------------------------------------------------------===//

// RIO command codes for IOCTL_AFD_RIO.
enum AFD_RIO_COMMAND : ULONG {
  AfdRioCommandIdCreateCq = 0,
  AfdRioCommandIdDestroyCq = 1,
  AfdRioCommandIdNotifyCq = 2,
  AfdRioCommandIdCreateRqPair = 3,
  AfdRioCommandIdRegisterBuffer = 4,
  AfdRioCommandIdDeregisterBuffer = 5,
  AfdRioCommandIdPokeSend = 6,
  AfdRioCommandIdPokeReceive = 7,
  AfdRioCommandIdResizeCq = 8,
  AfdRioCommandIdResizeRqPair = 9,
  AfdRioCommandIdMaximum = 10,
};

// RIO notification completion type.
enum AFD_RIO_NOTIFICATION_COMPLETION_TYPE : ULONG {
  AfdRioNoCompletion = 0,
  AfdRioEventCompletion = 1,
  AfdRioIocpCompletion = 2,
};

// Common header for all RIO commands.
struct AFD_RIO_COMMAND_HEADER {
  AFD_RIO_COMMAND Command;
};

// RIO buffer descriptor.
struct AFD_RIO_BUF {
  ULONG BufferId;
  ULONG Offset;
  ULONG Length;
};

// RIO request queue entry — one per send/recv operation.
struct AFD_RIO_REQUEST_QUEUE_ENTRY {
  AFD_RIO_BUF Data;
  AFD_RIO_BUF SourceAddress;
  AFD_RIO_BUF DestinationAddress;
  AFD_RIO_BUF Control;
  AFD_RIO_BUF FlagsBuffer;
  ULONG Flags;
  ULONGLONG Context;
};

// RIO request queue — shared memory, mapped into user mode.
struct AFD_RIO_REQUEST_QUEUE {
  ULONG Start;
  ULONG End;
  BOOLEAN PokeRequired;
  AFD_RIO_REQUEST_QUEUE_ENTRY Entries[1]; // Variable length
};

// Create completion queue.
struct AFD_RIO_COMMAND_CREATE_CQ {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG QSize;
  AFD_RIO_NOTIFICATION_COMPLETION_TYPE NotificationType;
  ULONGLONG NotificationHandle;
  ULONGLONG NotificationContext;
  ULONGLONG NotificationContext2;
  ULONG BufferSize;
  ULONGLONG Buffer; // Pointer to user-allocated CQ buffer
};

struct AFD_RIO_COMMAND_CREATE_CQ_RESULT {
  ULONG CqId;
};

// Destroy completion queue.
struct AFD_RIO_COMMAND_DESTROY_CQ {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG CqId;
};

// Notify completion queue — request notification when CQE is posted.
struct AFD_RIO_COMMAND_NOTIFY_CQ {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG Index;
};

// Create request queue pair (send + receive queues for a socket).
struct AFD_RIO_COMMAND_CREATE_RQ_PAIR {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG SendCompletionQueue;
  ULONG ReceiveCompletionQueue;
  ULONG SendQueueEntryCount;
  ULONG SendQueueBufferSize;
  ULONGLONG SendQueueBuffer;     // Pointer to user-allocated send RQ
  ULONG ReceiveQueueEntryCount;
  ULONG ReceiveQueueBufferSize;
  ULONGLONG ReceiveQueueBuffer;  // Pointer to user-allocated recv RQ
  ULONGLONG EndpointHandle;      // Socket handle
  ULONGLONG SocketContext;       // Opaque context
};

// Register a memory buffer for zero-copy I/O.
struct AFD_RIO_COMMAND_REGISTER_BUFFER {
  AFD_RIO_COMMAND_HEADER Header;
  ULONGLONG Buffer;
  ULONG BufferLength;
};

struct AFD_RIO_COMMAND_REGISTER_BUFFER_RESULT {
  ULONG BufferId;
};

// Deregister a previously registered buffer.
struct AFD_RIO_COMMAND_DEREGISTER_BUFFER {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG BufferId;
};

// Poke send/receive — notify the kernel that new entries are in the RQ.
struct AFD_RIO_COMMAND_POKE_SEND {
  AFD_RIO_COMMAND_HEADER Header;
};

struct AFD_RIO_COMMAND_POKE_RECEIVE {
  AFD_RIO_COMMAND_HEADER Header;
};

// Resize completion queue.
struct AFD_RIO_COMMAND_RESIZE_CQ {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG CqId;
  ULONG QSize;
  ULONG BufferSize;
  ULONGLONG Buffer;
};

// Resize request queue pair.
struct AFD_RIO_COMMAND_RESIZE_RQ_PAIR {
  AFD_RIO_COMMAND_HEADER Header;
  ULONG SendQueueEntryCount;
  ULONG SendQueueBufferSize;
  ULONGLONG SendQueueBuffer;
  ULONG ReceiveQueueEntryCount;
  ULONG ReceiveQueueBufferSize;
  ULONGLONG ReceiveQueueBuffer;
};

//===----------------------------------------------------------------------===//
// AFD_NOTIFY — ProcessSocketNotifications (Win11 22H2+, epoll equivalent)
//===----------------------------------------------------------------------===//

// Registration filter bits accepted by SOCK_NOTIFY_REGISTRATION.EventFilter.
// afd.sys validates that only the low three bits are present.
inline constexpr USHORT SOCK_NOTIFY_REGISTER_EVENT_IN = 0x0001;
inline constexpr USHORT SOCK_NOTIFY_REGISTER_EVENT_OUT = 0x0002;
inline constexpr USHORT SOCK_NOTIFY_REGISTER_EVENT_HANGUP = 0x0004;
inline constexpr USHORT SOCK_NOTIFY_REGISTER_EVENTS_ALL =
    SOCK_NOTIFY_REGISTER_EVENT_IN | SOCK_NOTIFY_REGISTER_EVENT_OUT |
    SOCK_NOTIFY_REGISTER_EVENT_HANGUP;

// Completion event bits returned through IoStatusBlock.Information /
// OVERLAPPED_ENTRY.dwNumberOfBytesTransferred. ERR and REMOVE are always
// eligible regardless of EventFilter, which is why afd.sys internally ORs
// them into the enabled compact mask.
inline constexpr USHORT SOCK_NOTIFY_EVENT_IN = SOCK_NOTIFY_REGISTER_EVENT_IN;
inline constexpr USHORT SOCK_NOTIFY_EVENT_OUT = SOCK_NOTIFY_REGISTER_EVENT_OUT;
inline constexpr USHORT SOCK_NOTIFY_EVENT_HANGUP =
    SOCK_NOTIFY_REGISTER_EVENT_HANGUP;
inline constexpr USHORT SOCK_NOTIFY_EVENT_ERR = 0x0040;
inline constexpr USHORT SOCK_NOTIFY_EVENT_REMOVE = 0x0080;
inline constexpr USHORT SOCK_NOTIFY_EVENTS_ALL =
    SOCK_NOTIFY_REGISTER_EVENTS_ALL | SOCK_NOTIFY_EVENT_ERR |
    SOCK_NOTIFY_EVENT_REMOVE;

// Operation codes.
inline constexpr UCHAR SOCK_NOTIFY_OP_NONE = 0;
inline constexpr UCHAR SOCK_NOTIFY_OP_ENABLE = 1;
inline constexpr UCHAR SOCK_NOTIFY_OP_DISABLE = 2;
inline constexpr UCHAR SOCK_NOTIFY_OP_REMOVE = 4;

// Trigger flags.
inline constexpr UCHAR SOCK_NOTIFY_TRIGGER_ONESHOT = 1;
inline constexpr UCHAR SOCK_NOTIFY_TRIGGER_PERSISTENT = 2;
inline constexpr UCHAR SOCK_NOTIFY_TRIGGER_LEVEL = 4;
inline constexpr UCHAR SOCK_NOTIFY_TRIGGER_EDGE = 8;
inline constexpr UCHAR SOCK_NOTIFY_TRIGGER_PERSISTENCE_MASK =
    SOCK_NOTIFY_TRIGGER_ONESHOT | SOCK_NOTIFY_TRIGGER_PERSISTENT;
inline constexpr UCHAR SOCK_NOTIFY_TRIGGER_DELIVERY_MASK =
    SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_EDGE;

// Per-socket registration entry. 24 bytes on x64.
struct SOCK_NOTIFY_REGISTRATION {
  HANDLE Socket;              // +0x00: AFD endpoint handle
  PVOID CompletionKey;        // +0x08: user context (returned in IOCP)
  USHORT EventFilter;         // +0x10: SOCK_NOTIFY_REGISTER_EVENT_* mask
  UCHAR Operation;            // +0x12: SOCK_NOTIFY_OP_*
  UCHAR TriggerFlags;         // +0x13: low nibble only; ENABLE requires
                              // exactly one bit from each *_MASK pair
  ULONG RegistrationResult;   // +0x14: output NTSTATUS per entry
};

// Input for IOCTL_AFD_NOTIFY. Exactly 48 bytes (0x30). No output buffer.
// The IOCTL is issued against the socket handle, not \Device\Afd\Mio.
struct AFD_NOTIFY_INPUT {
  HANDLE CompletionPort;                // +0x00: NtCreateIoCompletion handle
  SOCK_NOTIFY_REGISTRATION *Registrations; // +0x08: array of entries
  PVOID CompletionEntries;              // +0x10: NULL if no retrieval
  ULONG *ReceivedCount;                 // +0x18: NULL if no retrieval
  ULONG RegistrationCount;             // +0x20: must be > 0
  ULONG TimeoutMs;                     // +0x24: 0=no wait, 0xFFFFFFFF=infinite
  ULONG CompletionCount;               // +0x28: 0=register only
  ULONG TailAlignmentPadding;          // +0x2C: alignment-only tail pad;
                                       // AfdNotify requires total input size
                                       // 0x30 and does not read this word
};

inline constexpr ULONG AFD_NOTIFY_CONTEXT_LENGTH = 0x70;

// Raw compact readiness accumulator bits consumed by the AFD_NOTIFY state
// machine before normalization. Grounded by AfdNotifyComputeEvents and
// AfdNotifyNormalizeRawMask (0x14004e878).
inline constexpr USHORT AFD_NOTIFY_RAW_EVENT_RECV = 0x0001;
inline constexpr USHORT AFD_NOTIFY_RAW_EVENT_SEND = 0x0004;
inline constexpr USHORT AFD_NOTIFY_RAW_EVENT_DISCONNECT = 0x0008;
inline constexpr USHORT AFD_NOTIFY_RAW_EVENT_ABORT = 0x0010;
inline constexpr USHORT AFD_NOTIFY_RAW_EVENT_ACCEPT = 0x0080;
inline constexpr USHORT AFD_NOTIFY_RAW_EVENT_CONNECT_FAIL = 0x0100;

// Normalized compact event mask used inside AFD_NOTIFY_CONTEXT_OBJECT.
// These are the same bits that afd.sys posts through IoSetIoCompletionEx3.
inline constexpr USHORT AFD_NOTIFY_COMPACT_EVENT_IN = SOCK_NOTIFY_EVENT_IN;
inline constexpr USHORT AFD_NOTIFY_COMPACT_EVENT_OUT = SOCK_NOTIFY_EVENT_OUT;
inline constexpr USHORT AFD_NOTIFY_COMPACT_EVENT_HANGUP =
    SOCK_NOTIFY_EVENT_HANGUP;
inline constexpr USHORT AFD_NOTIFY_COMPACT_EVENT_ERR = SOCK_NOTIFY_EVENT_ERR;
inline constexpr USHORT AFD_NOTIFY_COMPACT_EVENT_REMOVE =
    SOCK_NOTIFY_EVENT_REMOVE;
inline constexpr USHORT AFD_NOTIFY_COMPACT_REQUIRED_EVENTS =
    AFD_NOTIFY_COMPACT_EVENT_ERR | AFD_NOTIFY_COMPACT_EVENT_REMOVE;

// Per-registration state bits at AFD_NOTIFY_CONTEXT_OBJECT.NotifyStateFlags.
inline constexpr USHORT AFD_NOTIFY_STATE_DESTROY_AFTER_DELIVERY = 0x0001;
inline constexpr USHORT AFD_NOTIFY_STATE_PROCESSING = 0x0002;
inline constexpr USHORT AFD_NOTIFY_STATE_DISABLE_AFTER_DELIVERY = 0x0004;

// Generic ntoskrnl mini-completion packet embedded at offset 0 in the live
// AFD_NOTIFY object. AFD uses the same 0x50-byte prefix in at least one other
// unrelated completion path, so this is a kernel I/O object, not a
// notify-specific private struct.
//
// Grounded directly from ntoskrnl.exe:
// - IoInitializeMiniCompletionPacket(packet, callback, callback_context)
//   writes PacketType = 4, MiniPacketCallback, Context, Allocated=0
// - IoAllocateMiniCompletionPacket(callback, callback_context)
//   allocates 0x50 bytes and writes the same fields with Allocated=1
// - IoSetIoCompletionEx3(sink, key, apc_ctx, status, info, ..., packet, ...)
//   fills KeyContext / ApcContext / IoStatus / IoStatusInformation and queues
//   ListEntry into the sink
// - IoCancelMiniCompletionPacket(sink, packet) removes ListEntry from the sink
// - IoFreeMiniCompletionPacket(packet) clears MiniPacketCallback and retires
//   the packet, freeing storage only when Allocated != 0
inline constexpr UCHAR IO_MINI_COMPLETION_PACKET_TYPE = 0x04;

LIBC_MSABI void
PIO_MINI_COMPLETION_PACKET_CALLBACK_PROTO(PVOID PacketArgument, PVOID Context);
using PIO_MINI_COMPLETION_PACKET_CALLBACK_FN =
    decltype(PIO_MINI_COMPLETION_PACKET_CALLBACK_PROTO);
using PIO_MINI_COMPLETION_PACKET_CALLBACK =
    PIO_MINI_COMPLETION_PACKET_CALLBACK_FN *;

// ntkrnlmp.pdb names this `_IO_MINI_COMPLETION_PACKET_USER`. The missing spans
// at +0x14, +0x2C, and +0x49 are not hidden members; the PDB field list jumps
// over them entirely, proving they are compiler-inserted alignment padding:
// - +0x14 pads ULONG PacketType to the next 8-byte pointer field
// - +0x2C pads LONG IoStatus to the next 8-byte ULONG_PTR field
// - +0x49..+0x4F is the struct tail pad after UCHAR Allocated
struct IO_MINI_COMPLETION_PACKET {
  LIST_ENTRY ListEntry; // +0x00: queued into the sink's internal list
  ULONG PacketType;     // +0x10: IO_MINI_COMPLETION_PACKET_TYPE
  ULONG AlignmentPadding14;
  PVOID KeyContext;   // +0x18: key argument passed to IoSetIoCompletionEx3
  PVOID ApcContext;   // +0x20: APC context argument from IoSetIoCompletionEx3
  NTSTATUS IoStatus;  // +0x28: status argument from IoSetIoCompletionEx3
  ULONG AlignmentPadding2C;
  ULONG_PTR IoStatusInformation; // +0x30: information argument from IoSetIoCompletionEx3
  PIO_MINI_COMPLETION_PACKET_CALLBACK
      MiniPacketCallback; // +0x38: callback from IoInitialize/Allocate*
  PVOID Context;          // +0x40: callback context from IoInitialize/Allocate*
  UCHAR Allocated;        // +0x48: 1 if allocated by IoAllocate*, 0 if embedded
  UCHAR TailAlignmentPadding[0x07];
};

static_assert(sizeof(IO_MINI_COMPLETION_PACKET) == 0x50,
              "IO mini-completion packet size mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET, PacketType) == 0x10,
              "IO mini-completion packet type offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET, KeyContext) == 0x18,
              "IO mini-completion packet key offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET, ApcContext) == 0x20,
              "IO mini-completion packet APC-context offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET, IoStatus) == 0x28,
              "IO mini-completion packet status offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET,
                                 IoStatusInformation) == 0x30,
              "IO mini-completion packet information offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET,
                                 MiniPacketCallback) == 0x38,
              "IO mini-completion packet callback offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET, Context) == 0x40,
              "IO mini-completion packet callback-context offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET, Allocated) == 0x48,
              "IO mini-completion packet ownership flag offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET,
                                 AlignmentPadding14) == 0x14,
              "IO mini-completion packet padding-14 offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET,
                                 AlignmentPadding2C) == 0x2C,
              "IO mini-completion packet padding-2C offset mismatch");
static_assert(__builtin_offsetof(IO_MINI_COMPLETION_PACKET,
                                 TailAlignmentPadding) == 0x49,
              "IO mini-completion packet tail-padding offset mismatch");

// Live per-endpoint AFD_NOTIFY / EVENT_SELECT registration object at
// endpoint +0x178, grounded by afd.pdb AfdNotifyProcessRegistration
// (0x14004ebc8) and the AfdNotifySock helpers. This is separate from the
// AF_UNIX selector blob; AfdSetContext/AfdGetContext do not decode that slot.
struct AFD_NOTIFY_CONTEXT_OBJECT {
  IO_MINI_COMPLETION_PACKET MiniCompletionPacket; // +0x00
  PVOID NotificationSink; // +0x50: referenced IOCP / EVENT_SELECT sink object
  PVOID CompletionKey;    // +0x58: user key returned in IOCP completions
  USHORT EnabledNotifyCompactMask; // +0x60: requested low-3-bit filter plus
                                   // compact ERR/REMOVE
  UCHAR RegistrationOp;            // +0x62: SOCK_NOTIFY_OP_*
  UCHAR TriggerFlags;              // +0x63: stored low nibble from registration
  USHORT CurrentRawCompactEventMask; // +0x64: mutable raw readiness accumulator
  USHORT
      PreviousRawCompactEventMask;   // +0x66: previous raw mask for edge mode
  USHORT PublishedNotifyCompactMask; // +0x68: normalized mask owned by any
                                     // outstanding notification packet
  USHORT NotifyStateFlags;           // +0x6A: AFD_NOTIFY_STATE_*
  ULONG TailAlignmentPadding6C;      // +0x6C: trailing pad to sizeof 0x70;
                                     // traced notify helpers never touch it
};

static_assert(sizeof(AFD_NOTIFY_CONTEXT_OBJECT) == AFD_NOTIFY_CONTEXT_LENGTH,
              "AFD notify context object size mismatch");
static_assert(__builtin_offsetof(AFD_NOTIFY_CONTEXT_OBJECT, NotificationSink) ==
                  0x50,
              "AFD notify context sink offset mismatch");
static_assert(__builtin_offsetof(AFD_NOTIFY_CONTEXT_OBJECT, CompletionKey) ==
                  0x58,
              "AFD notify context completion-key offset mismatch");
static_assert(__builtin_offsetof(AFD_NOTIFY_CONTEXT_OBJECT,
                                 EnabledNotifyCompactMask) == 0x60,
              "AFD notify context compact-mask offset mismatch");
static_assert(__builtin_offsetof(AFD_NOTIFY_CONTEXT_OBJECT, RegistrationOp) ==
                  0x62,
              "AFD notify context registration-op offset mismatch");
static_assert(__builtin_offsetof(AFD_NOTIFY_CONTEXT_OBJECT,
                                 PublishedNotifyCompactMask) == 0x68,
              "AFD notify context published-mask offset mismatch");
static_assert(__builtin_offsetof(AFD_NOTIFY_CONTEXT_OBJECT, NotifyStateFlags) ==
                  0x6A,
              "AFD notify context state offset mismatch");

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_AFD_H
