//===-- NT file I/O constants, structures, and enums ----------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_FILE_TYPES_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_FILE_TYPES_H

#include "src/__support/OSUtil/windows/nt/nt_types.h"

//===----------------------------------------------------------------------===//
// File Access Rights (NtCreateFile/NtOpenFile DesiredAccess)
//===----------------------------------------------------------------------===//

// NT-native recreation of the Win32 GetFileType classification values. These
// stay in the NT file layer because we derive them directly from native device
// metadata rather than through kernel32.
inline constexpr DWORD FILE_TYPE_UNKNOWN = 0x0000;
inline constexpr DWORD FILE_TYPE_DISK = 0x0001;
inline constexpr DWORD FILE_TYPE_CHAR = 0x0002;
inline constexpr DWORD FILE_TYPE_PIPE = 0x0003;

// File-specific access rights (bits 0–8). These are object-type-specific
// and overlap with directory-specific names at the same bit positions.
//
// Read data from the file. For directories: list entries.
inline constexpr ACCESS_MASK FILE_READ_DATA = 0x0001;
inline constexpr ACCESS_MASK FILE_LIST_DIRECTORY = 0x0001;
// Write data to the file. For directories: create files.
inline constexpr ACCESS_MASK FILE_WRITE_DATA = 0x0002;
inline constexpr ACCESS_MASK FILE_ADD_FILE = 0x0002;
// Append data to the file (write at end-of-file only).
// For directories: create subdirectories.
inline constexpr ACCESS_MASK FILE_APPEND_DATA = 0x0004;
inline constexpr ACCESS_MASK FILE_ADD_SUBDIRECTORY = 0x0004;
// Read extended attributes.
inline constexpr ACCESS_MASK FILE_READ_EA = 0x0008;
// Write extended attributes.
inline constexpr ACCESS_MASK FILE_WRITE_EA = 0x0010;
// Execute the file (traverse for directories).
inline constexpr ACCESS_MASK FILE_EXECUTE = 0x0020;
inline constexpr ACCESS_MASK FILE_TRAVERSE = 0x0020;
// Delete a child entry from a directory (regardless of the child's own ACL).
inline constexpr ACCESS_MASK FILE_DELETE_CHILD = 0x0040;
// Read file attributes (timestamps, size, etc.).
inline constexpr ACCESS_MASK FILE_READ_ATTRIBUTES = 0x0080;
// Write file attributes.
inline constexpr ACCESS_MASK FILE_WRITE_ATTRIBUTES = 0x0100;

// Standard access rights (DELETE_ACCESS, READ_CONTROL, WRITE_DAC,
// WRITE_OWNER, SYNCHRONIZE) are defined in nt_types.h.

// Generic access composites — convenience masks combining the typical rights
// needed for read, write, or execute operations.
inline constexpr ACCESS_MASK FILE_GENERIC_READ =
    READ_CONTROL | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA |
    SYNCHRONIZE;
inline constexpr ACCESS_MASK FILE_GENERIC_WRITE =
    READ_CONTROL | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA |
    FILE_APPEND_DATA | SYNCHRONIZE;
inline constexpr ACCESS_MASK FILE_GENERIC_EXECUTE =
    READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_EXECUTE | SYNCHRONIZE;

//===----------------------------------------------------------------------===//
// File Share Access (NtCreateFile/NtOpenFile ShareAccess)
//===----------------------------------------------------------------------===//
// Controls what other opens can coexist with this one. If a subsequent open
// requests access that is not shared, it fails with STATUS_SHARING_VIOLATION.

// Exclusive — no concurrent opens allowed.
inline constexpr ULONG FILE_SHARE_NONE = 0x00000000;
// Allow concurrent opens that request FILE_READ_DATA.
inline constexpr ULONG FILE_SHARE_READ = 0x00000001;
// Allow concurrent opens that request FILE_WRITE_DATA or FILE_APPEND_DATA.
inline constexpr ULONG FILE_SHARE_WRITE = 0x00000002;
// Allow concurrent opens that request DELETE_ACCESS.
inline constexpr ULONG FILE_SHARE_DELETE = 0x00000004;

//===----------------------------------------------------------------------===//
// File Create Disposition (NtCreateFile CreateDisposition)
//===----------------------------------------------------------------------===//
// Determines how NtCreateFile handles the existence/non-existence of the
// target file. IoStatusBlock.Information indicates which action was taken
// (see FILE_*_RESULT constants below).

// If the file exists, replace it (delete + create). If not, create it.
// Requires DELETE access on existing files. Result: FILE_SUPERSEDED_RESULT
// or FILE_CREATED_RESULT.
inline constexpr ULONG FILE_SUPERSEDE = 0x00000000;
// Open an existing file. Fail with STATUS_OBJECT_NAME_NOT_FOUND if it
// doesn't exist. Result: FILE_OPENED_RESULT.
inline constexpr ULONG FILE_OPEN = 0x00000001;
// Create a new file. Fail with STATUS_OBJECT_NAME_COLLISION if it already
// exists. Result: FILE_CREATED_RESULT.
inline constexpr ULONG FILE_CREATE = 0x00000002;
// Open if the file exists, create if it doesn't.
// Result: FILE_OPENED_RESULT or FILE_CREATED_RESULT.
inline constexpr ULONG FILE_OPEN_IF = 0x00000003;
// Open and overwrite an existing file (truncate to zero length). Fail if
// the file doesn't exist. Requires FILE_WRITE_DATA.
// Result: FILE_OVERWRITTEN_RESULT.
inline constexpr ULONG FILE_OVERWRITE = 0x00000004;
// Open and overwrite if the file exists, create if it doesn't. Requires
// FILE_WRITE_DATA for the overwrite case.
// Result: FILE_OVERWRITTEN_RESULT or FILE_CREATED_RESULT.
inline constexpr ULONG FILE_OVERWRITE_IF = 0x00000005;

//===----------------------------------------------------------------------===//
// File Create Options (NtCreateFile/NtOpenFile CreateOptions/OpenOptions)
//===----------------------------------------------------------------------===//
// Bitwise-OR combination controlling how the I/O manager opens or creates
// the file object. Many flags are mutually exclusive (noted below).

// The file being opened must be a directory. Fails with
// STATUS_NOT_A_DIRECTORY if the target is a regular file.
// Mutually exclusive with FILE_NON_DIRECTORY_FILE.
inline constexpr ULONG FILE_DIRECTORY_FILE = 0x00000001;
// Write operations flush through the filesystem cache directly to disk.
// Filesystem and disk caches are bypassed for write ordering guarantees.
inline constexpr ULONG FILE_WRITE_THROUGH = 0x00000002;
// Hint that the file will be accessed sequentially (head-to-tail).
// The cache manager reads ahead aggressively.
// Mutually exclusive with FILE_RANDOM_ACCESS.
inline constexpr ULONG FILE_SEQUENTIAL_ONLY = 0x00000004;
// Disable all caching. All reads/writes go directly to disk.
// Size and offset must be sector-aligned. Cannot combine with
// FILE_APPEND_DATA in DesiredAccess.
inline constexpr ULONG FILE_NO_INTERMEDIATE_BUFFERING = 0x00000008;
// Synchronous I/O with alertable waits. Each I/O operation completes
// before the syscall returns. STATUS_ALERTED is returned if an APC fires
// during the wait — enables EINTR-like behavior.
// Requires SYNCHRONIZE in DesiredAccess.
// Mutually exclusive with FILE_SYNCHRONOUS_IO_NONALERT.
inline constexpr ULONG FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
// Synchronous I/O with non-alertable waits. Same as _ALERT but APCs
// do not interrupt the wait — the I/O always completes or fails.
// Requires SYNCHRONIZE in DesiredAccess.
inline constexpr ULONG FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
// The file being opened must not be a directory. Fails with
// STATUS_FILE_IS_A_DIRECTORY if the target is a directory.
// Mutually exclusive with FILE_DIRECTORY_FILE.
inline constexpr ULONG FILE_NON_DIRECTORY_FILE = 0x00000040;
// Hint that accesses will be random (non-sequential). The cache manager
// does not read ahead. Mutually exclusive with FILE_SEQUENTIAL_ONLY.
inline constexpr ULONG FILE_RANDOM_ACCESS = 0x00000800;
// Delete the file when the last handle is closed. Requires DELETE in
// DesiredAccess. The file system sets the delete-on-close flag on the
// file object; the actual deletion happens at last-handle-close.
inline constexpr ULONG FILE_DELETE_ON_CLOSE = 0x00001000;
// Open by 8-byte file reference number (NTFS MFT record) or 16-byte file
// ID (ReFS) instead of by name. ObjectAttributes.ObjectName contains the
// binary ID as a UNICODE_STRING (Length=8 or 16, Buffer=&FileId).
inline constexpr ULONG FILE_OPEN_BY_FILE_ID = 0x00002000;
// Bypass security checks that would normally require the caller to have
// traverse access to each directory in the path. Used by backup utilities
// with SeBackupPrivilege / SeRestorePrivilege.
inline constexpr ULONG FILE_OPEN_FOR_BACKUP_INTENT = 0x00004000;
// Open the reparse point itself rather than following it. Without this
// flag, the I/O manager processes the reparse tag (e.g., follows a
// symlink). Required for reading/writing reparse point data via
// FSCTL_GET/SET_REPARSE_POINT.
inline constexpr ULONG FILE_OPEN_REPARSE_POINT = 0x00200000;
// Do not recall the file from remote/offline storage (e.g., HSM).
// The open fails with STATUS_FILE_IS_OFFLINE instead of triggering recall.
inline constexpr ULONG FILE_OPEN_NO_RECALL = 0x00400000;

//===----------------------------------------------------------------------===//
// Named Pipe constants (NtCreateNamedPipeFile)
//===----------------------------------------------------------------------===//

// NamedPipeType — how data is written into the pipe.
// Byte-stream: writes are concatenated into a continuous byte stream.
inline constexpr ULONG FILE_PIPE_BYTE_STREAM_TYPE = 0x00000000;
// Message: each write is a discrete message with preserved boundaries.
inline constexpr ULONG FILE_PIPE_MESSAGE_TYPE = 0x00000001;

// ReadMode — how data is read from the pipe.
// Byte-stream mode: reads return any available bytes (partial messages OK).
// Cannot set message mode on a byte-stream-type pipe.
inline constexpr ULONG FILE_PIPE_BYTE_STREAM_MODE = 0x00000000;
// Message mode: reads return complete messages. If the buffer is too small,
// STATUS_BUFFER_OVERFLOW is returned and the remainder can be read next.
inline constexpr ULONG FILE_PIPE_MESSAGE_MODE = 0x00000001;

// CompletionMode — blocking behavior.
// Queue: operations block until data is available / written / client connects.
inline constexpr ULONG FILE_PIPE_QUEUE_OPERATION = 0x00000000;
// Complete: operations return immediately (STATUS_PIPE_EMPTY if no data).
inline constexpr ULONG FILE_PIPE_COMPLETE_OPERATION = 0x00000001;

// Reject connections from remote clients (local-only pipe).
inline constexpr ULONG FILE_PIPE_REJECT_REMOTE_CLIENTS = 0x00000002;

// NamedPipeConfiguration — pipe direction (FILE_PIPE_LOCAL_INFORMATION).
// Inbound: data flows client→server only.
inline constexpr ULONG FILE_PIPE_INBOUND = 0x00000000;
// Outbound: data flows server→client only.
inline constexpr ULONG FILE_PIPE_OUTBOUND = 0x00000001;
// Full-duplex: data flows in both directions.
inline constexpr ULONG FILE_PIPE_FULL_DUPLEX = 0x00000002;

// NamedPipeState — pipe connection state (FILE_PIPE_LOCAL_INFORMATION).
// Server created but no client has connected.
inline constexpr ULONG FILE_PIPE_DISCONNECTED_STATE = 0x00000001;
// Server is listening for a client connection (FSCTL_PIPE_LISTEN pending).
inline constexpr ULONG FILE_PIPE_LISTENING_STATE = 0x00000002;
// A client is connected; the pipe is ready for I/O.
inline constexpr ULONG FILE_PIPE_CONNECTED_STATE = 0x00000003;
// One side has closed; the pipe is draining remaining data.
inline constexpr ULONG FILE_PIPE_CLOSING_STATE = 0x00000004;

// NamedPipeEnd — which end of the pipe this handle represents.
inline constexpr ULONG FILE_PIPE_CLIENT_END = 0x00000000;
inline constexpr ULONG FILE_PIPE_SERVER_END = 0x00000001;

// No limit on the number of pipe instances.
inline constexpr ULONG FILE_PIPE_UNLIMITED_INSTANCES = 0xFFFFFFFF;

// DeviceType is the native NT classification for a file/device object.
using DEVICE_TYPE = ULONG;

// Device-characteristics flags returned by
// FILE_STAT_BASIC_INFORMATION.DeviceCharacteristics and
// FILE_FS_DEVICE_INFORMATION.Characteristics.
inline constexpr ULONG FILE_REMOVABLE_MEDIA = 0x00000001;
inline constexpr ULONG FILE_READ_ONLY_DEVICE = 0x00000002;
inline constexpr ULONG FILE_FLOPPY_DISKETTE = 0x00000004;
inline constexpr ULONG FILE_WRITE_ONCE_MEDIA = 0x00000008;
inline constexpr ULONG FILE_REMOTE_DEVICE = 0x00000010;
inline constexpr ULONG FILE_DEVICE_IS_MOUNTED = 0x00000020;
inline constexpr ULONG FILE_VIRTUAL_VOLUME = 0x00000040;
inline constexpr ULONG FILE_AUTOGENERATED_DEVICE_NAME = 0x00000080;
inline constexpr ULONG FILE_DEVICE_SECURE_OPEN = 0x00000100;
inline constexpr ULONG FILE_CHARACTERISTIC_PNP_DEVICE = 0x00000800;
inline constexpr ULONG FILE_CHARACTERISTIC_TS_DEVICE = 0x00001000;
inline constexpr ULONG FILE_CHARACTERISTIC_WEBDAV_DEVICE = 0x00002000;
inline constexpr ULONG FILE_CHARACTERISTIC_CSV = 0x00010000;
inline constexpr ULONG FILE_DEVICE_ALLOW_APPCONTAINER_TRAVERSAL = 0x00020000;
inline constexpr ULONG FILE_PORTABLE_DEVICE = 0x00040000;
inline constexpr ULONG FILE_REMOTE_DEVICE_VSMB = 0x00080000;
inline constexpr ULONG FILE_DEVICE_REQUIRE_SECURITY_CHECK = 0x00100000;

// Device types returned by FILE_STAT_BASIC_INFORMATION.DeviceType and
// FILE_FS_DEVICE_INFORMATION.DeviceType.
inline constexpr ULONG FILE_DEVICE_CD_ROM = 0x00000002;
inline constexpr ULONG FILE_DEVICE_CD_ROM_FILE_SYSTEM = 0x00000003;
inline constexpr ULONG FILE_DEVICE_CONTROLLER = 0x00000004;
inline constexpr ULONG FILE_DEVICE_DATALINK = 0x00000005;
inline constexpr ULONG FILE_DEVICE_DFS = 0x00000006;
inline constexpr ULONG FILE_DEVICE_DISK = 0x00000007;
inline constexpr ULONG FILE_DEVICE_DISK_FILE_SYSTEM = 0x00000008;
inline constexpr ULONG FILE_DEVICE_KEYBOARD = 0x0000000B;
inline constexpr ULONG FILE_DEVICE_MIDI_OUT = 0x0000000E;
inline constexpr ULONG FILE_DEVICE_NAMED_PIPE = 0x00000011;
inline constexpr ULONG FILE_DEVICE_NETWORK_FILE_SYSTEM = 0x00000014;
inline constexpr ULONG FILE_DEVICE_NULL = 0x00000015;
inline constexpr ULONG FILE_DEVICE_PHYSICAL_NETCARD = 0x00000017;
inline constexpr ULONG FILE_DEVICE_SERIAL_MOUSE_PORT = 0x0000001A;
inline constexpr ULONG FILE_DEVICE_SERIAL_PORT = 0x0000001B;
inline constexpr ULONG FILE_DEVICE_SCREEN = 0x0000001C;
inline constexpr ULONG FILE_DEVICE_VIRTUAL_DISK = 0x00000024;
inline constexpr ULONG FILE_DEVICE_MODEM = 0x0000002B;
inline constexpr ULONG FILE_DEVICE_CONSOLE = 0x00000050;

//===----------------------------------------------------------------------===//
// Named Pipe information structures
//===----------------------------------------------------------------------===//

// NtSetInformationFile — FileCompletionInformation.
// Associates a file handle with an I/O Completion Port (IOCP).
struct FILE_COMPLETION_INFORMATION {
  HANDLE Port; // IOCP handle to associate with.
  PVOID Key;   // Completion key returned by NtRemoveIoCompletion[Ex].
};

// NtQueryInformationFile / NtSetInformationFile — FilePipeInformation.
// Not endpoint-specific — describes pipe-wide read/completion modes.
// When setting ReadMode on a byte-stream-type pipe, attempts to switch to
// FILE_PIPE_MESSAGE_MODE fail with STATUS_INVALID_PARAMETER.
struct FILE_PIPE_INFORMATION {
  ULONG ReadMode;       // FILE_PIPE_BYTE_STREAM_MODE or FILE_PIPE_MESSAGE_MODE
  ULONG CompletionMode; // FILE_PIPE_QUEUE_OPERATION or FILE_PIPE_COMPLETE_OPERATION
};

// NtQueryInformationFile — FilePipeLocalInformation.
// Describes the local endpoint's view of the pipe.
struct FILE_PIPE_LOCAL_INFORMATION {
  ULONG NamedPipeType;          // FILE_PIPE_BYTE_STREAM_TYPE or FILE_PIPE_MESSAGE_TYPE
  ULONG NamedPipeConfiguration; // FILE_PIPE_INBOUND / _OUTBOUND / _FULL_DUPLEX
  ULONG MaximumInstances;       // Max concurrent pipe instances (or UNLIMITED)
  ULONG CurrentInstances;       // Currently active instances
  ULONG InboundQuota;           // Buffer size for client→server data (bytes)
  ULONG ReadDataAvailable;      // Bytes available for immediate read
  ULONG OutboundQuota;          // Buffer size for server→client data (bytes)
  ULONG WriteQuotaAvailable;    // Write buffer space remaining (bytes)
  ULONG NamedPipeState;         // FILE_PIPE_*_STATE
  ULONG NamedPipeEnd;           // FILE_PIPE_CLIENT_END or FILE_PIPE_SERVER_END
};

// FSCTL_PIPE_WAIT input buffer — wait for a named pipe instance to become
// available. NtFsControlFile on the pipe root directory (\Device\NamedPipe).
struct FILE_PIPE_WAIT_FOR_BUFFER {
  LARGE_INTEGER Timeout; // 100ns intervals, negative = relative
  ULONG NameLength;      // Byte length of Name (not including NUL)
  BOOLEAN TimeoutSpecified; // If FALSE, uses the pipe's DefaultTimeout
  WCHAR Name[1];         // Pipe name (variable-length, not NUL-terminated)
};

// FSCTL_PIPE_PEEK output buffer — non-destructive read (does not consume data).
struct FILE_PIPE_PEEK_BUFFER {
  ULONG NamedPipeState;     // Current pipe state (FILE_PIPE_*_STATE)
  ULONG ReadDataAvailable;  // Total bytes available in the pipe
  ULONG NumberOfMessages;   // Messages waiting (0 for byte-stream pipes)
  ULONG MessageLength;      // Size of the next message (bytes)
  CHAR Data[1];             // Peeked data (variable-length)
};

//===----------------------------------------------------------------------===//
// Named Pipe FSCTL codes (FILE_DEVICE_NAMED_PIPE = 0x0011)
//===----------------------------------------------------------------------===//
// Sent via NtFsControlFile. All use METHOD_BUFFERED, FILE_ANY_ACCESS.

// Disconnect the server end. The pipe returns to DISCONNECTED_STATE and
// can accept a new client via FSCTL_PIPE_LISTEN.
inline constexpr ULONG FSCTL_PIPE_DISCONNECT = 0x00110004;
// Wait for a client to connect. Blocks until a client opens the pipe's
// other end (or timeout/cancel). Returns STATUS_PIPE_CONNECTED on success.
inline constexpr ULONG FSCTL_PIPE_LISTEN = 0x00110008;
// Non-destructive read — returns data without consuming it.
// Output buffer receives FILE_PIPE_PEEK_BUFFER.
inline constexpr ULONG FSCTL_PIPE_PEEK = 0x0011000C;
// Atomic write-then-read (message-mode pipes only). Writes InputBuffer,
// then reads the response into OutputBuffer. METHOD_NEITHER.
inline constexpr ULONG FSCTL_PIPE_TRANSCEIVE = 0x0011C017;
// Wait for a pipe instance to become available. InputBuffer is
// FILE_PIPE_WAIT_FOR_BUFFER. Sent to the pipe root directory handle.
inline constexpr ULONG FSCTL_PIPE_WAIT = 0x00110018;

//===----------------------------------------------------------------------===//
// File Attributes (NtCreateFile FileAttributes, query results)
//===----------------------------------------------------------------------===//
// Set during creation (NtCreateFile FileAttributes) or queried from
// FILE_BASIC_INFORMATION / directory enumeration structures.

// File cannot be written or deleted.
inline constexpr ULONG FILE_ATTRIBUTE_READONLY = 0x00000001;
// File is hidden from normal directory enumeration.
inline constexpr ULONG FILE_ATTRIBUTE_HIDDEN = 0x00000002;
// File is used exclusively by the operating system.
inline constexpr ULONG FILE_ATTRIBUTE_SYSTEM = 0x00000004;
// Entry is a directory. Set by the filesystem, not by callers.
inline constexpr ULONG FILE_ATTRIBUTE_DIRECTORY = 0x00000010;
// File has changed since last backup. Set automatically on write;
// cleared by backup software.
inline constexpr ULONG FILE_ATTRIBUTE_ARCHIVE = 0x00000020;
// No other attributes are set. Valid only when used alone — cannot
// combine with any other attribute flag.
inline constexpr ULONG FILE_ATTRIBUTE_NORMAL = 0x00000080;
// File is intended for temporary storage. The filesystem may keep all
// data in memory and avoid flushing to disk.
inline constexpr ULONG FILE_ATTRIBUTE_TEMPORARY = 0x00000100;
// File is a reparse point (symlink, junction, etc.). The ReparseTag
// field in directory entries or FILE_STAT_BASIC_INFORMATION identifies
// the reparse type (IO_REPARSE_TAG_SYMLINK, IO_REPARSE_TAG_MOUNT_POINT).
inline constexpr ULONG FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400;

//===----------------------------------------------------------------------===//
// Special ByteOffset values for NtReadFile/NtWriteFile
//===----------------------------------------------------------------------===//
// These are placed in the LowPart of the LARGE_INTEGER ByteOffset parameter,
// with HighPart set to -1 (0xFFFFFFFF).

// Append: write starts at the current end-of-file.
// LARGE_INTEGER form: LowPart = 0xFFFFFFFF, HighPart = -1.
inline constexpr ULONG FILE_WRITE_TO_END_OF_FILE = 0xFFFFFFFF;
// Use the file object's internal position (maintained by synchronous I/O).
inline constexpr ULONG FILE_USE_FILE_POINTER_POSITION = 0xFFFFFFFE;

// 64-bit forms for IO Ring SQE offsets (both halves must be set).
inline constexpr ULONGLONG FILE_WRITE_TO_END_OF_FILE64 = 0xFFFFFFFFFFFFFFFFULL;
inline constexpr ULONGLONG FILE_USE_FILE_POINTER_POSITION64 =
    0xFFFFFFFFFFFFFFFEULL;

//===----------------------------------------------------------------------===//
// I/O Status Information values (NtCreateFile IoStatusBlock.Information)
//===----------------------------------------------------------------------===//
// After NtCreateFile/NtOpenFile succeeds, IoStatusBlock.Information
// indicates which action was taken. These values map directly to the
// CreateDisposition that triggered them.

inline constexpr ULONG_PTR FILE_SUPERSEDED_RESULT = 0;    // FILE_SUPERSEDE replaced existing file
inline constexpr ULONG_PTR FILE_OPENED_RESULT = 1;        // FILE_OPEN / FILE_OPEN_IF opened existing
inline constexpr ULONG_PTR FILE_CREATED_RESULT = 2;       // FILE_CREATE / FILE_OPEN_IF / FILE_OVERWRITE_IF created new
inline constexpr ULONG_PTR FILE_OVERWRITTEN_RESULT = 3;   // FILE_OVERWRITE / FILE_OVERWRITE_IF truncated existing
inline constexpr ULONG_PTR FILE_EXISTS_RESULT = 4;        // FILE_CREATE failed — file already exists
inline constexpr ULONG_PTR FILE_DOES_NOT_EXIST_RESULT = 5; // FILE_OPEN / FILE_OVERWRITE failed — not found


//===----------------------------------------------------------------------===//
// FILE_INFORMATION_CLASS — enum for NtQueryInformationFile/NtSetInformationFile
//===----------------------------------------------------------------------===//
// Annotations: q = query (NtQueryInformationFile), s = set (NtSetInformationFile),
// qs = both. Required access rights noted in parentheses.
// Classes also used with NtQueryDirectoryFile[Ex] are noted.

enum FILE_INFORMATION_CLASS {
  // q: FILE_DIRECTORY_INFORMATION (FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
  FileDirectoryInformation = 1,
  // q: FILE_BOTH_DIR_INFORMATION (FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
  FileBothDirectoryInformation = 3,
  // qs: FILE_BASIC_INFORMATION (q: FILE_READ_ATTRIBUTES; s: FILE_WRITE_ATTRIBUTES)
  // Timestamps and attributes. Setting a time to 0 preserves the current value;
  // -1 disables timestamp updates; -2 re-enables them.
  FileBasicInformation = 4,
  // q: FILE_STANDARD_INFORMATION
  // Allocation size, EOF, link count, delete-pending, directory flag.
  FileStandardInformation = 5,
  // q: FILE_INTERNAL_INFORMATION
  // 8-byte file reference number (NTFS MFT index). Maps to st_ino.
  FileInternalInformation = 6,
  // q: FILE_NAME_INFORMATION — relative name within the volume.
  FileNameInformation = 9,
  // s: FILE_RENAME_INFORMATION (requires DELETE)
  FileRenameInformation = 10,
  // s: FILE_LINK_INFORMATION — create a hard link.
  FileLinkInformation = 11,
  // q: FILE_NAMES_INFORMATION (FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
  // Minimal directory entry: just NextEntryOffset, FileIndex, and FileName.
  FileNamesInformation = 12,
  // s: FILE_DISPOSITION_INFORMATION (requires DELETE)
  // Mark file for deletion on last handle close.
  FileDispositionInformation = 13,
  // qs: FILE_POSITION_INFORMATION
  // Current byte offset for synchronous I/O handles.
  FilePositionInformation = 14,
  // qs: FILE_MODE_INFORMATION
  // File open mode flags (FILE_SYNCHRONOUS_IO_ALERT, etc.).
  FileModeInformation = 16,
  // q: FILE_ALL_INFORMATION — composite of Basic, Standard, Internal, EA,
  // Access, Position, Mode, Alignment, and Name information.
  FileAllInformation = 18,
  // s: FILE_ALLOCATION_INFORMATION (requires FILE_WRITE_DATA)
  // Preallocate disk space without changing the visible EOF.
  FileAllocationInformation = 19,
  // s: FILE_END_OF_FILE_INFORMATION (requires FILE_WRITE_DATA)
  // Set the logical file size (truncate or extend).
  FileEndOfFileInformation = 20,
  // qs: FILE_PIPE_INFORMATION — pipe read mode and completion mode.
  FilePipeInformation = 23,
  // s: FILE_COMPLETION_INFORMATION — associate handle with an IOCP.
  FileCompletionInformation = 30,
  // q: FILE_PIPE_LOCAL_INFORMATION — pipe state and buffer availability.
  FilePipeLocalInformation = 24,
  // q: FILE_ID_BOTH_DIR_INFORMATION (FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
  // Directory entry with 8-byte FileId and 8.3 short name.
  FileIdBothDirectoryInformation = 37,
  // q: FILE_ID_EXTD_BOTH_DIR_INFORMATION (FILE_LIST_DIRECTORY) (NtQueryDirectoryFile[Ex])
  // Directory entry with 128-bit FileId, ReparsePointTag, and short name.
  // Enables symlink detection without opening each entry. Since Threshold (Win10).
  FileIdExtdBothDirectoryInformation = 63,
  // s: FILE_DISPOSITION_INFORMATION_EX (requires DELETE) — since Redstone (RS1).
  // Supports POSIX unlink semantics (immediate name removal while handles open).
  FileDispositionInformationEx = 64,
  // s: FILE_RENAME_INFORMATION_EX — since Redstone (RS1).
  // Supports POSIX rename semantics (atomic replace of target).
  FileRenameInformationEx = 65,
  // q: FILE_STAT_INFORMATION — timestamps, sizes, attributes, link count,
  // reparse tag, and effective access in a single query. Since Redstone2 (RS2).
  FileStatInformation = 68,
  // q: FILE_STAT_LX_INFORMATION (FILE_READ_ATTRIBUTES + FILE_READ_EA)
  // Includes WSL metadata: uid, gid, mode, device IDs. Since Redstone4 (RS4).
  FileStatLxInformation = 70,
  // s: FILE_LINK_INFORMATION_EX — extended hard link with flags. Since RS5.
  FileLinkInformationEx = 72,
  // qs: FILE_STAT_BASIC_INFORMATION — single-call stat() with all fields:
  // timestamps, sizes, attributes, reparse tag, link count, device type,
  // volume serial, and 128-bit FileId. Since 23H2.
  FileStatBasicInformation = 77,
};

//===----------------------------------------------------------------------===//
// File Information Structures
//===----------------------------------------------------------------------===//

// FileBasicInformation — timestamps and attributes.
// When setting: a time value of 0 preserves the current value; -1 disables
// timestamp updates for I/O on this handle; -2 re-enables them.
// Setting requires FILE_WRITE_ATTRIBUTES access.
struct FILE_BASIC_INFORMATION {
  LARGE_INTEGER CreationTime;    // File creation time (100ns since 1601-01-01)
  LARGE_INTEGER LastAccessTime;  // Last read or execute time
  LARGE_INTEGER LastWriteTime;   // Last write time
  LARGE_INTEGER ChangeTime;      // Last metadata change time (NTFS-specific)
  ULONG FileAttributes;          // FILE_ATTRIBUTE_* flags
};

// FileNetworkOpenInformation — combined basic + standard info in one query.
// Returned by NtQueryFullAttributesFile (fast stat without opening the file).
struct FILE_NETWORK_OPEN_INFORMATION {
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER AllocationSize;
  LARGE_INTEGER EndOfFile;
  ULONG FileAttributes;
};

// FileStandardInformation — size, link count, and state.
// EndOfFile is the byte offset of the first free byte (i.e., the logical
// file size). AllocationSize is typically a multiple of the cluster size.
struct FILE_STANDARD_INFORMATION {
  LARGE_INTEGER AllocationSize;  // Disk space allocated (bytes)
  LARGE_INTEGER EndOfFile;       // Logical file size (bytes)
  ULONG NumberOfLinks;           // Hard link count
  BOOLEAN DeletePending;         // TRUE if deletion has been requested
  BOOLEAN Directory;             // TRUE if this is a directory
};

// FilePositionInformation — current byte offset for synchronous I/O.
// Only meaningful on handles opened with FILE_SYNCHRONOUS_IO_*.
struct FILE_POSITION_INFORMATION {
  LARGE_INTEGER CurrentByteOffset;
};

// FileEndOfFileInformation — set the logical file size.
// Truncates if smaller than current EOF, extends (zero-filled) if larger.
// Requires FILE_WRITE_DATA access.
struct FILE_END_OF_FILE_INFORMATION {
  LARGE_INTEGER EndOfFile;
};

// FileAllocationInformation — preallocate disk space without changing the
// visible file size (the logical EOF). The filesystem reserves contiguous
// clusters up to AllocationSize. Used by posix_fallocate.
// Requires FILE_WRITE_DATA access.
struct FILE_ALLOCATION_INFORMATION {
  LARGE_INTEGER AllocationSize;
};

// FileDispositionInformation — mark a file for deletion on last handle close.
// Legacy: only supports simple boolean delete. For POSIX semantics (immediate
// name removal), use FileDispositionInformationEx instead.
// Requires DELETE access.
struct FILE_DISPOSITION_INFORMATION {
  BOOLEAN DeleteFile;
};

// FileDispositionInformationEx flags (RS1+) — extended deletion control.
// Mark the file for deletion.
inline constexpr ULONG FILE_DISPOSITION_DELETE = 0x00000001;
// POSIX semantics: the name is removed immediately (while handles remain
// open). Other processes see the file as deleted. Without this flag,
// deletion is deferred until the last handle closes (Win32 behavior).
inline constexpr ULONG FILE_DISPOSITION_POSIX_SEMANTICS = 0x00000002;
// Force the image section check even for non-image files. Fails if the
// file has a mapped image section.
inline constexpr ULONG FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK = 0x00000004;
// Only set the delete-on-close flag when this handle closes (like
// FILE_DELETE_ON_CLOSE in CreateOptions). Without this flag, the
// disposition is set immediately.
inline constexpr ULONG FILE_DISPOSITION_ON_CLOSE = 0x00000008;
// Allow deletion of read-only files (ignore FILE_ATTRIBUTE_READONLY).
inline constexpr ULONG FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE = 0x00000010;

struct FILE_DISPOSITION_INFORMATION_EX {
  ULONG Flags; // Combination of FILE_DISPOSITION_* flags
};

// FileRenameInformation — move/rename a file via NtSetInformationFile.
// Variable-length: allocate sizeof(FILE_RENAME_INFORMATION) + extra bytes
// for the destination path (FileName is a flexible array member).
// Requires DELETE access on the source file.
struct FILE_RENAME_INFORMATION {
  union {
    BOOLEAN ReplaceIfExists; // FileRenameInformation (legacy)
    ULONG Flags;             // FileRenameInformationEx (RS1+)
  };
  HANDLE RootDirectory;  // If non-NULL, FileName is relative to this directory
  ULONG FileNameLength;  // In bytes, not including NUL
  WCHAR FileName[1];     // Destination path (variable-length)
};

// FileRenameInformationEx flags (RS1+).
// Atomically replace the target if it exists (like POSIX rename()).
inline constexpr ULONG FILE_RENAME_REPLACE_IF_EXISTS = 0x00000001;
// POSIX semantics: atomic replace even if the target is open elsewhere.
// Without this flag, replacing an open file fails with STATUS_ACCESS_DENIED.
inline constexpr ULONG FILE_RENAME_POSIX_SEMANTICS = 0x00000002;

// FileLinkInformation — create a hard link via NtSetInformationFile.
// Layout matches FILE_RENAME_INFORMATION. The source file gains an
// additional directory entry (hard link) at the specified path.
struct FILE_LINK_INFORMATION {
  union {
    BOOLEAN ReplaceIfExists; // FileLinkInformation (legacy)
    ULONG Flags;             // FileLinkInformationEx (RS5+)
  };
  HANDLE RootDirectory;  // If non-NULL, FileName is relative to this directory
  ULONG FileNameLength;  // In bytes, not including NUL
  WCHAR FileName[1];     // Link path (variable-length)
};

// FileLinkInformationEx flags (RS5+).
// Replace an existing file at the link target path.
inline constexpr ULONG FILE_LINK_REPLACE_IF_EXISTS = 0x00000001;
// POSIX semantics: atomic replace even if the target is open.
inline constexpr ULONG FILE_LINK_POSIX_SEMANTICS = 0x00000002;

// FileInternalInformation — 8-byte file reference number.
// On NTFS: low 48 bits = MFT record index, high 16 bits = sequence number.
// Same value as FileId in FILE_ID_BOTH_DIR_INFORMATION. Maps to st_ino.
struct FILE_INTERNAL_INFORMATION {
  LARGE_INTEGER IndexNumber;
};

// FileNameInformation — relative file name within the volume.
// Variable-length: FileName is not NUL-terminated; use FileNameLength.
struct FILE_NAME_INFORMATION {
  ULONG FileNameLength; // In bytes
  WCHAR FileName[1];    // Relative path (variable-length, not NUL-terminated)
};

// 128-bit file identifier. ReFS uses this natively; NTFS provides it via
// extended info classes (FileIdExtdBothDirectoryInformation,
// FileStatBasicInformation). Superset of the 8-byte NTFS file reference.
struct FILE_ID_128 {
  UCHAR Identifier[16];
};

// FileStatBasicInformation (23H2+) — single-call stat() with all fields.
// Replaces the open+query(FileBasicInformation)+query(FileStandardInformation)
// +query(FileInternalInformation)+close pattern.
// Can be used with NtQueryInformationByName for stat-by-path without opening.
struct FILE_STAT_BASIC_INFORMATION {
  LARGE_INTEGER FileId;          // 8-byte file reference (same as FileInternalInformation)
  LARGE_INTEGER CreationTime;    // File creation time (100ns since 1601-01-01)
  LARGE_INTEGER LastAccessTime;  // Last read/execute time
  LARGE_INTEGER LastWriteTime;   // Last write time
  LARGE_INTEGER ChangeTime;      // Last metadata change time
  LARGE_INTEGER AllocationSize;  // Disk space allocated (bytes)
  LARGE_INTEGER EndOfFile;       // Logical file size (bytes)
  ULONG FileAttributes;          // FILE_ATTRIBUTE_* flags
  ULONG ReparseTag;              // IO_REPARSE_TAG_* (0 if not a reparse point)
  ULONG NumberOfLinks;           // Hard link count
  DEVICE_TYPE DeviceType;        // FILE_DEVICE_NAMED_PIPE, FILE_DEVICE_DISK, etc.
  ULONG DeviceCharacteristics;   // FILE_REMOVABLE_MEDIA, FILE_REMOTE_DEVICE, etc.
  ULONG Reserved;
  LARGE_INTEGER VolumeSerialNumber; // Volume serial → st_dev
  FILE_ID_128 FileId128;         // 128-bit file ID (ReFS native)
};

// Reparse tags — high bit indicates Microsoft tag; bit 29 indicates name surrogate
// (the reparse point is a name for another named entity in the system).
// Junction: directory mount point redirecting to another directory path.
inline constexpr ULONG IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003UL;
// Symbolic link: file or directory reference (can be relative or absolute).
inline constexpr ULONG IO_REPARSE_TAG_SYMLINK = 0xA000000CUL;
// AF_UNIX socket endpoint on NTFS (Win10 1803+). Not a name surrogate.
inline constexpr ULONG IO_REPARSE_TAG_AF_UNIX = 0x80000023UL;

// Reparse point data buffer — retrieved/set via FSCTL_GET/SET_REPARSE_POINT.
// The Flags field in SymbolicLinkReparseBuffer distinguishes absolute vs.
// relative symlinks.
inline constexpr ULONG SYMLINK_FLAG_RELATIVE = 0x00000001;
inline constexpr ULONG MAXIMUM_REPARSE_DATA_BUFFER_SIZE = 16384;

struct REPARSE_DATA_BUFFER {
  ULONG ReparseTag;        // IO_REPARSE_TAG_* identifying the reparse type
  USHORT ReparseDataLength; // Byte size of the data after this header
  USHORT Reserved;
  union {
    // IO_REPARSE_TAG_SYMLINK
    struct {
      USHORT SubstituteNameOffset; // Byte offset in PathBuffer
      USHORT SubstituteNameLength; // Byte length (not including NUL)
      USHORT PrintNameOffset;      // Byte offset of display name in PathBuffer
      USHORT PrintNameLength;      // Byte length of display name
      ULONG Flags;                 // SYMLINK_FLAG_RELATIVE or 0 (absolute)
      WCHAR PathBuffer[1];         // Contains both SubstituteName and PrintName
    } SymbolicLinkReparseBuffer;
    // IO_REPARSE_TAG_MOUNT_POINT (junction)
    struct {
      USHORT SubstituteNameOffset;
      USHORT SubstituteNameLength;
      USHORT PrintNameOffset;
      USHORT PrintNameLength;
      WCHAR PathBuffer[1];
    } MountPointReparseBuffer;
    // Other reparse tags — opaque data.
    struct {
      UCHAR DataBuffer[1];
    } GenericReparseBuffer;
  };
};

// Filesystem control codes for reparse point manipulation.
// Sent via NtFsControlFile. Require FILE_OPEN_REPARSE_POINT to target
// the reparse point itself rather than following it.
// Set reparse data on a file/directory. Input: REPARSE_DATA_BUFFER.
inline constexpr ULONG FSCTL_SET_REPARSE_POINT = 0x000900A4;
// Retrieve reparse data. Output: REPARSE_DATA_BUFFER.
inline constexpr ULONG FSCTL_GET_REPARSE_POINT = 0x000900A8;
// Remove the reparse point. Input: REPARSE_DATA_BUFFER (tag + GUID only).
inline constexpr ULONG FSCTL_DELETE_REPARSE_POINT = 0x000900AC;

// Query NTFS volume metadata (MFT size, cluster geometry, etc.).
// Output: NTFS_VOLUME_DATA_BUFFER. Requires FILE_READ_ATTRIBUTES on a
// volume/directory handle. Fails with STATUS_INVALID_DEVICE_REQUEST on
// non-NTFS filesystems (ReFS, FAT, etc.).
inline constexpr ULONG FSCTL_GET_NTFS_VOLUME_DATA = 0x00090064;

struct NTFS_VOLUME_DATA_BUFFER {
  LARGE_INTEGER VolumeSerialNumber;
  LARGE_INTEGER NumberSectors;
  LARGE_INTEGER TotalClusters;
  LARGE_INTEGER FreeClusters;
  LARGE_INTEGER TotalReserved;
  ULONG BytesPerSector;
  ULONG BytesPerCluster;
  ULONG BytesPerFileRecordSegment;
  ULONG ClustersPerFileRecordSegment;
  LARGE_INTEGER MftValidDataLength;
  LARGE_INTEGER MftStartLcn;
  LARGE_INTEGER Mft2StartLcn;
  LARGE_INTEGER MftZoneStart;
  LARGE_INTEGER MftZoneEnd;
};

//===----------------------------------------------------------------------===//
// Directory Information Structures (for NtQueryDirectoryFile[Ex])
//===----------------------------------------------------------------------===//
// All directory enumeration structures are variable-length, linked-list style:
// NextEntryOffset is the byte offset to the next entry (0 = last entry).
// FileName is variable-length and NOT NUL-terminated; use FileNameLength.
// Entries are packed contiguously in the output buffer.

// FileNamesInformation — minimal directory entry (just name and index).
// ~12 bytes header vs ~120+ for FileIdExtdBothDirectoryInformation.
// Used for lightweight entry counting (e.g. seekdir skip phase).
struct FILE_NAMES_INFORMATION {
  ULONG NextEntryOffset;         // Byte offset to next entry (0 = last)
  ULONG FileIndex;               // Filesystem-specific index (opaque)
  ULONG FileNameLength;          // Byte length of FileName
  WCHAR FileName[1];             // Variable-length, not NUL-terminated
};

// FileDirectoryInformation — basic directory entry.
struct FILE_DIRECTORY_INFORMATION {
  ULONG NextEntryOffset;         // Byte offset to next entry (0 = last)
  ULONG FileIndex;               // Filesystem-specific index (opaque)
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER EndOfFile;       // Logical file size
  LARGE_INTEGER AllocationSize;  // Allocated disk space
  ULONG FileAttributes;          // FILE_ATTRIBUTE_* flags
  ULONG FileNameLength;          // Byte length of FileName
  WCHAR FileName[1];             // Variable-length, not NUL-terminated
};

// FileIdBothDirectoryInformation — includes 8-byte FileId and 8.3 short name.
// The most commonly used class for directory enumeration with inode tracking.
struct FILE_ID_BOTH_DIR_INFORMATION {
  ULONG NextEntryOffset;
  ULONG FileIndex;
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER EndOfFile;
  LARGE_INTEGER AllocationSize;
  ULONG FileAttributes;
  ULONG FileNameLength;
  ULONG EaSize;                  // Extended attribute size (bytes)
  CCHAR ShortNameLength;         // Byte length of ShortName (0 if none)
  WCHAR ShortName[12];           // 8.3 short name (fixed buffer, may be empty)
  LARGE_INTEGER FileId;          // 8-byte file reference number (st_ino)
  WCHAR FileName[1];
};

// FileIdExtdBothDirectoryInformation — extended variant (Threshold / Win10+).
// Adds 128-bit FileId (ReFS native) and ReparsePointTag, which enables
// symlink detection (IO_REPARSE_TAG_SYMLINK) without opening each entry.
struct FILE_ID_EXTD_BOTH_DIR_INFORMATION {
  ULONG NextEntryOffset;
  ULONG FileIndex;
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER LastWriteTime;
  LARGE_INTEGER ChangeTime;
  LARGE_INTEGER EndOfFile;
  LARGE_INTEGER AllocationSize;
  ULONG FileAttributes;
  ULONG FileNameLength;
  ULONG EaSize;
  ULONG ReparsePointTag;         // IO_REPARSE_TAG_* (0 if not a reparse point)
  FILE_ID_128 FileId;            // 128-bit file identifier
  CCHAR ShortNameLength;
  WCHAR ShortName[12];
  WCHAR FileName[1];
};

//===----------------------------------------------------------------------===//
// NtQueryDirectoryFileEx QueryFlags
//===----------------------------------------------------------------------===//

// Start the scan at the first entry in the directory. Without this flag,
// the scan resumes from where the last query ended.
inline constexpr ULONG FILE_QUERY_RESTART_SCAN = 0x00000001;
// Return at most one directory entry per call (less efficient but simpler).
inline constexpr ULONG FILE_QUERY_RETURN_SINGLE_ENTRY = 0x00000002;
// Start the scan at a filesystem-specific indexed position.
inline constexpr ULONG FILE_QUERY_INDEX_SPECIFIED = 0x00000004;
// Do not update per-FileObject cursor state. Enables true parallel
// enumeration from multiple threads using the same handle (RS5+).
// Each call behaves as if FILE_QUERY_RESTART_SCAN were set.
inline constexpr ULONG FILE_QUERY_NO_CURSOR_UPDATE = 0x00000010;

//===----------------------------------------------------------------------===//
// NtFlushBuffersFileEx Flags
//===----------------------------------------------------------------------===//

// Flush data + metadata from the Windows cache, then send SYNC to storage.
// Equivalent to NtFlushBuffersFile (fsync).
inline constexpr ULONG FLUSH_FLAGS_FILE_NORMAL = 0x00000000;
// Flush file data only from the Windows cache. No metadata flush, no
// storage SYNC. Not supported on volume handles.
inline constexpr ULONG FLUSH_FLAGS_FILE_DATA_ONLY = 0x00000001;
// Flush data + metadata from the cache but do NOT send SYNC to storage.
// Not supported on volume handles.
inline constexpr ULONG FLUSH_FLAGS_NO_SYNC = 0x00000002;
// Flush file data from cache + send SYNC to storage, skipping timestamp
// updates where possible. Equivalent to fdatasync. Not supported on
// volume or directory handles. (Redstone1+)
inline constexpr ULONG FLUSH_FLAGS_FILE_DATA_SYNC_ONLY = 0x00000004;

//===----------------------------------------------------------------------===//
// Volume / Filesystem Information — NtQueryVolumeInformationFile
//===----------------------------------------------------------------------===//

enum FS_INFORMATION_CLASS : ULONG {
  FileFsVolumeInformation = 1,       // q: FILE_FS_VOLUME_INFORMATION
  FileFsLabelInformation = 2,        // s: (set volume label, SeManageVolumePrivilege)
  FileFsSizeInformation = 3,         // q: FILE_FS_SIZE_INFORMATION
  FileFsDeviceInformation = 4,       // q: FILE_FS_DEVICE_INFORMATION
  FileFsAttributeInformation = 5,    // q: FILE_FS_ATTRIBUTE_INFORMATION
  FileFsControlInformation = 6,      // qs: (quota control, SeManageVolumePrivilege)
  FileFsFullSizeInformation = 7,     // q: FILE_FS_FULL_SIZE_INFORMATION
  FileFsObjectIdInformation = 8,     // qs: (volume GUID, SeRestorePrivilege)
  FileFsSectorSizeInformation = 11,  // q: (physical/logical sector sizes, Win8+)
};

// FileFsVolumeInformation — volume identity.
// VolumeSerialNumber maps to st_dev for POSIX stat().
// Variable-length: VolumeLabel is not NUL-terminated.
struct FILE_FS_VOLUME_INFORMATION {
  LARGE_INTEGER VolumeCreationTime; // Volume creation timestamp
  ULONG VolumeSerialNumber;         // Unique volume ID → st_dev
  ULONG VolumeLabelLength;          // Byte length of VolumeLabel
  BOOLEAN SupportsObjects;          // TRUE if volume supports object IDs
  WCHAR VolumeLabel[1];             // Volume label (variable-length)
};

// FileFsAttributeInformation — filesystem capabilities and name.
// FileSystemAttributes contains capability flags for detecting support
// for hard links, POSIX unlink, reparse points, etc.
// MaximumComponentNameLength maps to _PC_NAME_MAX (typically 255 on NTFS).
// Variable-length: FileSystemName is not NUL-terminated.
struct FILE_FS_ATTRIBUTE_INFORMATION {
  ULONG FileSystemAttributes;       // FILE_SUPPORTS_* / FILE_CASE_* flags
  LONG MaximumComponentNameLength;   // Max single path component (chars)
  ULONG FileSystemNameLength;        // Byte length of FileSystemName
  WCHAR FileSystemName[1];          // L"NTFS", L"ReFS", L"FAT32", etc.
};

// FileFsSizeInformation — volume capacity.
// Block size = SectorsPerAllocationUnit * BytesPerSector (typically 4096).
struct FILE_FS_SIZE_INFORMATION {
  LARGE_INTEGER TotalAllocationUnits;     // Total blocks on volume
  LARGE_INTEGER AvailableAllocationUnits; // Free blocks
  ULONG SectorsPerAllocationUnit;         // Sectors per cluster
  ULONG BytesPerSector;                   // Physical sector size
};

// FileFsDeviceInformation — underlying device class for the open handle.
// libc uses this for its NT-native file-type classifier.
struct FILE_FS_DEVICE_INFORMATION {
  DEVICE_TYPE DeviceType;  // FILE_DEVICE_* constant for this handle
  ULONG Characteristics;   // FILE_REMOVABLE_MEDIA, FILE_REMOTE_DEVICE, etc.
};

// FileFsFullSizeInformation — volume capacity with quota awareness.
// CallerAvailableAllocationUnits respects per-user disk quotas;
// ActualAvailableAllocationUnits ignores quotas.
struct FILE_FS_FULL_SIZE_INFORMATION {
  LARGE_INTEGER TotalAllocationUnits;
  LARGE_INTEGER CallerAvailableAllocationUnits;  // Quota-aware free space
  LARGE_INTEGER ActualAvailableAllocationUnits;  // Actual free space
  ULONG SectorsPerAllocationUnit;
  ULONG BytesPerSector;
};

// Filesystem capability flags (FILE_FS_ATTRIBUTE_INFORMATION.FileSystemAttributes).
// These indicate what features the filesystem supports. Query to decide
// whether POSIX-style operations (hard links, unlink, case sensitivity)
// are available on this volume.

// Filesystem supports case-sensitive name lookups.
inline constexpr ULONG FILE_CASE_SENSITIVE_SEARCH = 0x00000001;
// Filesystem preserves the case of file names when stored.
inline constexpr ULONG FILE_CASE_PRESERVED_NAMES = 0x00000002;
// Filesystem supports per-file compression.
inline constexpr ULONG FILE_FILE_COMPRESSION = 0x00000010;
// Filesystem supports per-user disk quotas.
inline constexpr ULONG FILE_VOLUME_QUOTAS = 0x00000020;
// Filesystem supports sparse files (unallocated ranges read as zeros).
inline constexpr ULONG FILE_SUPPORTS_SPARSE_FILES = 0x00000040;
// Filesystem supports reparse points (symlinks, junctions, etc.).
inline constexpr ULONG FILE_SUPPORTS_REPARSE_POINTS = 0x00000080;
// Filesystem supports POSIX-style unlink and rename semantics
// (FileDispositionInformationEx/FileRenameInformationEx with POSIX flags).
// NTFS: RS1+, ReFS: RS5+.
inline constexpr ULONG FILE_SUPPORTS_POSIX_UNLINK_RENAME = 0x00000400;
// Volume is read-only (e.g., CD-ROM, write-protected USB).
inline constexpr ULONG FILE_READ_ONLY_VOLUME = 0x00080000;
// Filesystem supports hard links (NTFS, ReFS).
inline constexpr ULONG FILE_SUPPORTS_HARD_LINKS = 0x00400000;
// Filesystem supports extended attributes (EAs).
inline constexpr ULONG FILE_SUPPORTS_EXTENDED_ATTRIBUTES = 0x00800000;
// Filesystem supports opening files by file ID (FILE_OPEN_BY_FILE_ID).
inline constexpr ULONG FILE_SUPPORTS_OPEN_BY_FILE_ID = 0x01000000;

// FILE_SEGMENT_ELEMENT — scatter/gather I/O element for NtReadFileScatter
// and NtWriteFileGather. Each element holds a page-aligned buffer address.
// The array must be terminated by a zero element.
union FILE_SEGMENT_ELEMENT {
  PVOID64 Buffer;
  ULONGLONG Alignment;
};

//===----------------------------------------------------------------------===//
// Directory Change Notification
//===----------------------------------------------------------------------===//

// CompletionFilter constants for NtNotifyChangeDirectoryFileEx.
inline constexpr ULONG FILE_NOTIFY_CHANGE_FILE_NAME = 0x00000001;
inline constexpr ULONG FILE_NOTIFY_CHANGE_DIR_NAME = 0x00000002;
inline constexpr ULONG FILE_NOTIFY_CHANGE_NAME = 0x00000003;
inline constexpr ULONG FILE_NOTIFY_CHANGE_ATTRIBUTES = 0x00000004;
inline constexpr ULONG FILE_NOTIFY_CHANGE_SIZE = 0x00000008;
inline constexpr ULONG FILE_NOTIFY_CHANGE_LAST_WRITE = 0x00000010;
inline constexpr ULONG FILE_NOTIFY_CHANGE_LAST_ACCESS = 0x00000020;
inline constexpr ULONG FILE_NOTIFY_CHANGE_CREATION = 0x00000040;
inline constexpr ULONG FILE_NOTIFY_CHANGE_EA = 0x00000080;
inline constexpr ULONG FILE_NOTIFY_CHANGE_SECURITY = 0x00000100;
inline constexpr ULONG FILE_NOTIFY_CHANGE_STREAM_NAME = 0x00000200;
inline constexpr ULONG FILE_NOTIFY_CHANGE_STREAM_SIZE = 0x00000400;
inline constexpr ULONG FILE_NOTIFY_CHANGE_STREAM_WRITE = 0x00000800;
inline constexpr ULONG FILE_NOTIFY_VALID_MASK = 0x00000FFF;

// Action codes returned in FILE_NOTIFY_INFORMATION.Action.
inline constexpr ULONG FILE_ACTION_ADDED = 0x00000001;
inline constexpr ULONG FILE_ACTION_REMOVED = 0x00000002;
inline constexpr ULONG FILE_ACTION_MODIFIED = 0x00000003;
inline constexpr ULONG FILE_ACTION_RENAMED_OLD_NAME = 0x00000004;
inline constexpr ULONG FILE_ACTION_RENAMED_NEW_NAME = 0x00000005;
inline constexpr ULONG FILE_ACTION_ADDED_STREAM = 0x00000006;
inline constexpr ULONG FILE_ACTION_REMOVED_STREAM = 0x00000007;
inline constexpr ULONG FILE_ACTION_MODIFIED_STREAM = 0x00000008;
inline constexpr ULONG FILE_ACTION_REMOVED_BY_DELETE = 0x00000009;
inline constexpr ULONG FILE_ACTION_ID_NOT_TUNNELLED = 0x0000000A;
inline constexpr ULONG FILE_ACTION_TUNNELLED_ID_COLLISION = 0x0000000B;

// Selects the output structure format for NtNotifyChangeDirectoryFileEx.
enum DIRECTORY_NOTIFY_INFORMATION_CLASS : ULONG {
  DirectoryNotifyInformation = 1,       // FILE_NOTIFY_INFORMATION
  DirectoryNotifyExtendedInformation,   // FILE_NOTIFY_EXTENDED_INFORMATION
  DirectoryNotifyFullInformation,       // FILE_NOTIFY_FULL_INFORMATION (22H2+)
};

// Output buffer entry for DirectoryNotifyInformation.
// Variable-length: FileName extends past the struct.
struct FILE_NOTIFY_INFORMATION {
  ULONG NextEntryOffset; // 0 if last entry
  ULONG Action;          // FILE_ACTION_* constant
  ULONG FileNameLength;  // bytes, not counting NUL
  WCHAR FileName[1];     // variable-length, not NUL-terminated
};

// Output buffer entry for DirectoryNotifyExtendedInformation.
// Adds full stat-like metadata to each change notification.
struct FILE_NOTIFY_EXTENDED_INFORMATION {
  ULONG NextEntryOffset;
  ULONG Action;
  LARGE_INTEGER CreationTime;
  LARGE_INTEGER LastModificationTime;
  LARGE_INTEGER LastChangeTime;
  LARGE_INTEGER LastAccessTime;
  LARGE_INTEGER AllocatedLength;
  LARGE_INTEGER FileSize;
  ULONG FileAttributes;
  union {
    ULONG ReparsePointTag;
    ULONG EaSize;
  };
  LARGE_INTEGER FileId;
  LARGE_INTEGER ParentFileId;
  ULONG FileNameLength;
  WCHAR FileName[1];
};

//===----------------------------------------------------------------------===//
// I/O Completion Ports — epoll/poll emulation
//===----------------------------------------------------------------------===//

// Access rights for I/O completion ports and wait completion packets.
inline constexpr ACCESS_MASK IO_COMPLETION_ALL_ACCESS = 0x001F0003;
inline constexpr ACCESS_MASK WAIT_COMPLETION_PACKET_ALL_ACCESS = 0x001F0003;

// Dequeued completion entry for NtRemoveIoCompletionEx.
struct FILE_IO_COMPLETION_INFORMATION {
  PVOID KeyContext;
  PVOID ApcContext;
  IO_STATUS_BLOCK IoStatusBlock;
};

// Information class for NtQueryIoCompletion.
enum IO_COMPLETION_INFORMATION_CLASS : ULONG {
  IoCompletionBasicInformation = 0,
};

// Returned by NtQueryIoCompletion(IoCompletionBasicInformation).
struct IO_COMPLETION_BASIC_INFORMATION {
  LONG Depth; // Current queue depth
};

//===----------------------------------------------------------------------===//
// Reserve Objects — pre-allocated kernel resources for guaranteed delivery
//===----------------------------------------------------------------------===//

enum MEMORY_RESERVE_TYPE : ULONG {
  MemoryReserveUserApc = 0,
  MemoryReserveIoCompletion = 1,
  MemoryReserveTypeMax = 2,
};

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_FILE_TYPES_H
