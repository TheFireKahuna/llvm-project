//===-- NT IoRing constants and structures ------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// IoRing Versions
//===----------------------------------------------------------------------===//

inline constexpr ULONG IORING_VERSION_1 = 1;   // Win11 21H2: READ, REGISTER, CANCEL
inline constexpr ULONG IORING_VERSION_2 = 2;   // Win11 21H2: completion event race fix
inline constexpr ULONG IORING_VERSION_3 = 300; // Win11 22H2: adds WRITE, FLUSH, DRAIN
inline constexpr ULONG IORING_VERSION_4 = 400; // Win11 24H2: adds READ_SCATTER, WRITE_GATHER

//===----------------------------------------------------------------------===//
// IoRing Operation Codes — SQE OpCode field
//===----------------------------------------------------------------------===//

inline constexpr ULONG IORING_OP_NOP = 0;              // No-op (test/barrier)
inline constexpr ULONG IORING_OP_READ = 1;             // Read from file/socket
inline constexpr ULONG IORING_OP_REGISTER_FILES = 2;   // Register handle table
inline constexpr ULONG IORING_OP_REGISTER_BUFFERS = 3; // Register pinned buffers
inline constexpr ULONG IORING_OP_CANCEL = 4;           // Cancel pending op by UserData
inline constexpr ULONG IORING_OP_WRITE = 5;            // Write to file/socket (v3+)
inline constexpr ULONG IORING_OP_FLUSH = 6;            // Flush file (v3+, not sockets)
inline constexpr ULONG IORING_OP_READ_SCATTER = 7;     // Scatter read (v4+, files only)
inline constexpr ULONG IORING_OP_WRITE_GATHER = 8;     // Gather write (v4+, files only)

//===----------------------------------------------------------------------===//
// SQE Flags
//===----------------------------------------------------------------------===//

inline constexpr ULONG IORING_SQE_FLAG_NONE = 0x00000000;
// DRAIN: all SQEs before this one must complete before this SQE executes.
// Ordering barrier — equivalent to io_uring IOSQE_IO_DRAIN.
inline constexpr ULONG IORING_SQE_FLAG_DRAIN = 0x00000001;

//===----------------------------------------------------------------------===//
// SQE CommonOpFlags — reference type for file handles and buffers
//===----------------------------------------------------------------------===//

// File reference is a raw HANDLE (default).
inline constexpr ULONG IORING_OP_FLAG_NONE = 0x00000000;
// File reference is an index into a registered handle table.
inline constexpr ULONG IORING_OP_FLAG_REGISTERED_FILE = 0x00000001;
// Buffer reference is a {index, offset} into a registered buffer table.
inline constexpr ULONG IORING_OP_FLAG_REGISTERED_BUFFER = 0x00000002;

//===----------------------------------------------------------------------===//
// IoRing NTSTATUS codes (from ntstatus.h)
//===----------------------------------------------------------------------===//

inline constexpr NTSTATUS STATUS_IORING_REQUIRED_FLAG_NOT_SUPPORTED =
    static_cast<NTSTATUS>(0xC0460001L);
inline constexpr NTSTATUS STATUS_IORING_SUBMISSION_QUEUE_FULL =
    static_cast<NTSTATUS>(0xC0460002L);
inline constexpr NTSTATUS STATUS_IORING_VERSION_NOT_SUPPORTED =
    static_cast<NTSTATUS>(0xC0460003L);
inline constexpr NTSTATUS STATUS_IORING_SUBMISSION_QUEUE_TOO_BIG =
    static_cast<NTSTATUS>(0xC0460004L);
inline constexpr NTSTATUS STATUS_IORING_COMPLETION_QUEUE_TOO_BIG =
    static_cast<NTSTATUS>(0xC0460005L);
inline constexpr NTSTATUS STATUS_IORING_SUBMIT_IN_PROGRESS =
    static_cast<NTSTATUS>(0xC0460006L);
inline constexpr NTSTATUS STATUS_IORING_CORRUPT =
    static_cast<NTSTATUS>(0xC0460007L);
inline constexpr NTSTATUS STATUS_IORING_COMPLETION_QUEUE_TOO_FULL =
    static_cast<NTSTATUS>(0xC0460008L);

//===----------------------------------------------------------------------===//
// IoRing Feature Flags — returned in NT_IORING_CAPABILITIES.Features
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// IoRing Feature Flags — returned in NT_IORING_CAPABILITIES.Features
// (from ntioring_x.h IORING_FEATURE_FLAGS enum)
//===----------------------------------------------------------------------===//

inline constexpr ULONG IORING_FEATURE_FLAGS_NONE = 0x00000000;
// IoRing is emulated in user mode (no kernel support). Functional but slow.
inline constexpr ULONG IORING_FEATURE_UM_EMULATION = 0x00000001;
// SetIoRingCompletionEvent / NtSetInformationIoRing is supported.
inline constexpr ULONG IORING_FEATURE_SET_COMPLETION_EVENT = 0x00000002;

//===----------------------------------------------------------------------===//
// NtCreateIoRing — Input Parameters
//===----------------------------------------------------------------------===//

// NT_IORING_STRUCTV1 — creation parameters passed to NtCreateIoRing.
// CreateParametersLength must be sizeof(NT_IORING_STRUCTV1) = 20.
struct NT_IORING_STRUCTV1 {
  ULONG IoRingVersion;       // Requested version (e.g. IORING_VERSION_3 = 300)
  ULONG SubmissionQueueSize; // Requested SQ entry count (rounded to power-of-2)
  ULONG CompletionQueueSize; // Requested CQ entry count (rounded to power-of-2)
  ULONG RequiredFlags;       // Required feature flags (0 = none)
  ULONG AdvisoryFlags;       // Advisory flags (0 = none)
};

//===----------------------------------------------------------------------===//
// Submission Queue / Completion Queue — shared memory layout
//===----------------------------------------------------------------------===//

// Submission Queue Entry (SQE) — 64 bytes.
// Pushed by user mode into the SQ ring. The kernel processes these on
// NtSubmitIoRing and posts corresponding CQEs.
struct NT_IORING_SQE {
  ULONG OpCode;        // +0x00: IORING_OP_* constant
  ULONG Flags;         // +0x04: IORING_SQE_FLAG_* (0 = none, 1 = DRAIN)
  ULONGLONG UserData;  // +0x08: opaque value echoed in the corresponding CQE

  // Operation-specific payload — all ops fit within this 48-byte union.
  union {
    // IORING_OP_READ / IORING_OP_WRITE
    struct {
      ULONG CommonOpFlags;   // +0x10: IORING_OP_FLAG_REGISTERED_FILE/BUFFER
      ULONG _Padding;        // +0x14: (WRITE: FileWriteFlags)
      ULONGLONG FileRef;     // +0x18: HANDLE or registered index
      ULONGLONG BufferRef;   // +0x20: pointer or {index, offset}
      ULONGLONG Offset;      // +0x28: file offset (0 for sockets/pipes)
      ULONG Length;           // +0x30: bytes to read/write
      ULONG Key;              // +0x34: file key (0 for most uses)
      UCHAR _Reserved[8];    // +0x38
    } ReadWrite;

    // IORING_OP_FLUSH
    struct {
      ULONG CommonOpFlags;   // +0x10
      ULONG FlushMode;       // +0x14
      ULONGLONG FileRef;     // +0x18: file handle to flush
      UCHAR _pad[32];        // +0x20
    } Flush;

    // IORING_OP_CANCEL
    struct {
      ULONG CommonOpFlags;   // +0x10
      ULONG _Padding;        // +0x14
      ULONGLONG FileRef;     // +0x18: file handle of the op to cancel
      ULONGLONG CancelId;    // +0x20: UserData of the target SQE
      UCHAR _pad[24];        // +0x28
    } Cancel;

    // IORING_OP_REGISTER_FILES
    struct {
      ULONG CommonOpFlags;   // +0x10
      ULONG RequiredFlags;   // +0x14
      ULONG AdvisoryFlags;   // +0x18
      ULONG Count;            // +0x1C: number of handles
      ULONGLONG Handles;     // +0x20: pointer to HANDLE[] array
      UCHAR _pad[24];        // +0x28
    } RegisterFiles;

    // IORING_OP_REGISTER_BUFFERS
    struct {
      ULONG CommonOpFlags;   // +0x10
      ULONG RequiredFlags;   // +0x14
      ULONG AdvisoryFlags;   // +0x18
      ULONG Count;            // +0x1C: number of buffers
      ULONGLONG Buffers;     // +0x20: pointer to IORING_BUFFER_INFO[]
      UCHAR _pad[24];        // +0x28
    } RegisterBuffers;

    // Raw bytes for future ops.
    UCHAR _Raw[48];          // +0x10 through +0x3F
  };
};

static_assert(sizeof(NT_IORING_SQE) == 64, "SQE must be 64 bytes");

// Completion Queue Entry (CQE) — 24 bytes.
// Posted by the kernel into the CQ ring. Popped by user mode after
// NtSubmitIoRing returns or when polling the ring.
struct NT_IORING_CQE {
  ULONGLONG UserData;   // +0x00: echoed from the corresponding SQE
  LONG ResultCode;      // +0x08: HRESULT (S_OK=0, or HRESULT_FROM_NT)
  ULONG _Padding;       // +0x0C
  ULONGLONG Information; // +0x10: bytes transferred (for READ/WRITE)
};

static_assert(sizeof(NT_IORING_CQE) == 24, "CQE must be 24 bytes");

// Submission Queue header — mapped shared memory.
// SQEs are at Entries[0] through Entries[capacity-1].
// User mode pushes by writing SQE at Entries[Tail & mask], then incrementing
// Tail with a release fence. Kernel advances Head after processing.
struct NT_IORING_SQ {
  ULONG Head;           // +0x00: kernel advances after processing
  ULONG Tail;           // +0x04: user advances after pushing SQE
  ULONG Flags;          // +0x08: reserved
  ULONG _Padding;       // +0x0C
  NT_IORING_SQE Entries[1]; // +0x10: variable-length SQE array
};

// Completion Queue header — mapped shared memory.
// CQEs are at Entries[0] through Entries[capacity-1].
// Kernel pushes by writing CQE at Entries[Tail & mask], then incrementing
// Tail. User mode reads from Entries[Head & mask], then increments Head
// with a release fence.
struct NT_IORING_CQ {
  ULONG Head;           // +0x00: user advances after reading CQE
  ULONG Tail;           // +0x04: kernel advances after posting CQE
  NT_IORING_CQE Entries[1]; // +0x08: variable-length CQE array
};

//===----------------------------------------------------------------------===//
// NtCreateIoRing — Output Parameters
//===----------------------------------------------------------------------===//

// NT_IORING_INFO — returned by NtCreateIoRing. Contains the negotiated
// version, ring parameters, and pointers to the mapped SQ/CQ shared memory.
// OutputParametersLength must be sizeof(NT_IORING_INFO) = 48.
struct NT_IORING_INFO {
  ULONG IoRingVersion;              // +0x00: accepted version
  ULONG RequiredFlags;              // +0x04: flags the kernel requires
  ULONG AdvisoryFlags;              // +0x08: advisory flags accepted
  ULONG SubmissionQueueSize;        // +0x0C: actual SQ entry count (power-of-2)
  ULONG SubmissionQueueRingMask;    // +0x10: SQ index mask (Size - 1)
  ULONG CompletionQueueSize;        // +0x14: actual CQ entry count (power-of-2)
  ULONG CompletionQueueRingMask;    // +0x18: CQ index mask (Size - 1)
  ULONG _Padding;                   // +0x1C
  NT_IORING_SQ *SubmissionQueue;    // +0x20: mapped SQ pointer
  NT_IORING_CQ *CompletionQueue;    // +0x28: mapped CQ pointer
};

//===----------------------------------------------------------------------===//
// NtQueryIoRingCapabilities — Output
//===----------------------------------------------------------------------===//

// NT_IORING_CAPABILITIES — system-wide IoRing capability discovery.
// Returned by NtQueryIoRingCapabilities.
struct NT_IORING_CAPABILITIES {
  ULONG MaxVersion;              // Highest supported IORING_VERSION_*
  ULONG MaxOpCode;               // Highest supported IORING_OP_* + 1
  ULONG Features;                // IORING_FEATURE_* bitmask
  ULONG MaxSubmissionQueueSize;  // Maximum SQ entry count
  ULONG MaxCompletionQueueSize;  // Maximum CQ entry count
};

//===----------------------------------------------------------------------===//
// NtSetInformationIoRing — Information Classes
//===----------------------------------------------------------------------===//

// Information class for NtSetInformationIoRing.
inline constexpr ULONG IoRingInformationClassCompletionEvent = 1;

// Input for IoRingInformationClassCompletionEvent — register an event
// that the kernel signals when new CQEs are posted. This event can be
// bridged to an IOCP via NtAssociateWaitCompletionPacket for unified
// event-loop integration.
struct NT_IORING_COMPLETION_EVENT_INFO {
  HANDLE CompletionEvent; // Event handle to signal on new CQEs
};

//===----------------------------------------------------------------------===//
// Buffer Registration (from ntioring_x.h)
//===----------------------------------------------------------------------===//

// Buffer info for IORING_OP_REGISTER_BUFFERS.
struct IORING_BUFFER_INFO {
  PVOID Address;  // Buffer base address (must be committed memory)
  UINT32 Length;   // Buffer length in bytes
};

// Registered buffer reference — index + offset into a pre-registered buffer.
struct IORING_REGISTERED_BUFFER {
  UINT32 BufferIndex;
  UINT32 Offset;
};

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_IORING_TYPES_H
