//===-- NT file I/O API declarations ------------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_FILE_API_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_FILE_API_H

#include "include/__llvm-libc-common.h"
#include "src/__support/OSUtil/windows/nt/nt_file_types.h"

extern "C" {

//===----------------------------------------------------------------------===//
// Volume / Filesystem Information — NtQueryVolumeInformationFile
//===----------------------------------------------------------------------===//

NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS NtQueryVolumeInformationFile(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation,
    ULONG Length, FS_INFORMATION_CLASS FsInformationClass);


//===----------------------------------------------------------------------===//
// NT File System Calls
//===----------------------------------------------------------------------===//

// NtCreateFile — create or open a file, directory, device, or volume.
// The primary entry point for all file open operations.
//
// Key parameter interactions:
// - AllocationSize: initial disk reservation (non-zero reduces fragmentation).
//   Only used when creating/superseding. NULL for open operations.
// - CreateDisposition: FILE_OPEN, FILE_CREATE, FILE_OPEN_IF, etc.
// - CreateOptions: FILE_SYNCHRONOUS_IO_ALERT for EINTR-capable sync I/O.
// - ObjectAttributes.RootDirectory: enables openat() semantics — FileName
//   is resolved relative to this directory handle.
//
// Returns: STATUS_SUCCESS, STATUS_OBJECT_NAME_COLLISION (FILE_CREATE exists),
//   STATUS_OBJECT_NAME_NOT_FOUND (FILE_OPEN doesn't exist), etc.
// IoStatusBlock.Information: FILE_OPENED_RESULT, FILE_CREATED_RESULT, etc.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
             PCOBJECT_ATTRIBUTES ObjectAttributes,
             PIO_STATUS_BLOCK IoStatusBlock, LARGE_INTEGER *AllocationSize,
             ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition,
             ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);

// NtCreateNamedPipeFile — create a named pipe server endpoint.
// The pipe name is in ObjectAttributes (typically under \Device\NamedPipe\).
//
// - NamedPipeType/ReadMode/CompletionMode: FILE_PIPE_* constants above.
// - InboundQuota/OutboundQuota: buffer sizes for each direction (bytes).
// - DefaultTimeout: used by FSCTL_PIPE_WAIT if the client doesn't specify
//   one. Negative = relative time in 100ns intervals.
// - Use FILE_SYNCHRONOUS_IO_ALERT for sync alertable handles (EINTR).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateNamedPipeFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                      PCOBJECT_ATTRIBUTES ObjectAttributes,
                      PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess,
                      ULONG CreateDisposition, ULONG CreateOptions,
                      ULONG NamedPipeType, ULONG ReadMode,
                      ULONG CompletionMode, ULONG MaximumInstances,
                      ULONG InboundQuota, ULONG OutboundQuota,
                      LARGE_INTEGER *DefaultTimeout);

// NtOpenFile — open an existing file, device, directory, or volume.
// Simpler variant of NtCreateFile: no AllocationSize, FileAttributes, or EA.
// OpenOptions corresponds to NtCreateFile's CreateOptions.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
           PCOBJECT_ATTRIBUTES ObjectAttributes,
           PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess,
           ULONG OpenOptions);

// NtReadFile — read data from an open file.
//
// - Event: optional event signaled on completion (async I/O).
// - ApcRoutine/ApcContext: optional APC callback for async completion.
// - ByteOffset: file position. NULL uses the handle's current position
//   (synchronous I/O). Use FILE_USE_FILE_POINTER_POSITION sentinel to be
//   explicit. FILE_WRITE_TO_END_OF_FILE is invalid for reads.
// - Key: byte-range lock key (NULL for normal reads).
//
// On alertable handles (FILE_SYNCHRONOUS_IO_ALERT), returns STATUS_ALERTED
// when an APC fires — enables EINTR-like cancellation.
// IoStatusBlock.Information receives the number of bytes actually read.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
           PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
           ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);

// NtWriteFile — write data to an open file.
// Same parameter semantics as NtReadFile. ByteOffset can additionally use
// FILE_WRITE_TO_END_OF_FILE for append operations.
// IoStatusBlock.Information receives the number of bytes actually written.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
            PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
            ULONG Length, LARGE_INTEGER *ByteOffset, ULONG *Key);

// NtReadFileScatter — scatter read: reads contiguous file data into
// discontinuous page-aligned memory buffers. SegmentArray is a
// zero-terminated array of FILE_SEGMENT_ELEMENT, each pointing to one page.
// Length must be a multiple of the sector size. Implements readv() semantics.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtReadFileScatter(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                  PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                  FILE_SEGMENT_ELEMENT *SegmentArray, ULONG Length,
                  LARGE_INTEGER *ByteOffset, ULONG *Key);

// NtWriteFileGather — gather write: writes discontinuous page-aligned memory
// buffers as contiguous file data. Mirror of NtReadFileScatter for writev().
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtWriteFileGather(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                  PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                  FILE_SEGMENT_ELEMENT *SegmentArray, ULONG Length,
                  LARGE_INTEGER *ByteOffset, ULONG *Key);

// NtCancelIoFileEx — cancel a specific pending I/O operation.
// IoRequestToCancel is the IO_STATUS_BLOCK passed to the original
// NtReadFile/NtWriteFile. If NULL, cancels all pending I/O on the handle
// regardless of which thread issued them.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCancelIoFileEx(HANDLE FileHandle, PIO_STATUS_BLOCK IoRequestToCancel,
                 PIO_STATUS_BLOCK IoStatusBlock);

// NtCancelSynchronousIoFile — cancel a synchronous I/O operation blocking on
// another thread. ThreadHandle identifies the thread whose synchronous I/O to
// cancel. IoRequestToCancel optionally identifies a specific operation; if NULL,
// cancels any pending synchronous I/O on that thread.
// Essential for pthread_cancel of threads blocked in read()/write().
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCancelSynchronousIoFile(HANDLE ThreadHandle,
                          PIO_STATUS_BLOCK IoRequestToCancel,
                          PIO_STATUS_BLOCK IoStatusBlock);

// NtDeleteFile — delete a file by path in a single syscall.
// No handle is opened or returned. The file must not be in use.
// More efficient than the open + FileDispositionInformation + close pattern
// when no handle is needed.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtDeleteFile(PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtFsControlFile — send a filesystem control code (FSCTL_*).
// Used for reparse point manipulation (FSCTL_SET/GET/DELETE_REPARSE_POINT),
// named pipe operations (FSCTL_PIPE_*), and other filesystem-specific ops.
// Event/ApcRoutine enable async completion notification.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtFsControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                ULONG FsControlCode, PVOID InputBuffer,
                ULONG InputBufferLength, PVOID OutputBuffer,
                ULONG OutputBufferLength);

// NtDeviceIoControlFile — send a device I/O control code (IOCTL_*).
// Same parameter layout as NtFsControlFile but targets device drivers
// (e.g., mount manager, disk driver) rather than filesystem drivers.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event,
                      PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                      PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
                      PVOID InputBuffer, ULONG InputBufferLength,
                      PVOID OutputBuffer, ULONG OutputBufferLength);

// NtFlushBuffersFile — flush file data and metadata to persistent storage.
// Equivalent to fsync(): writes cached data, commits metadata, sends SYNC
// command to the storage device. Blocks until the flush completes.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);

// NtFlushBuffersFileEx — flush with fine-grained control (Win8+).
//
// Flag selection for POSIX equivalents:
//   fsync()     → FLUSH_FLAGS_FILE_NORMAL (data + metadata + device SYNC)
//   fdatasync() → FLUSH_FLAGS_FILE_DATA_SYNC_ONLY (data + device SYNC, no metadata)
//
// NOT FLUSH_FLAGS_FILE_DATA_ONLY — that skips the device SYNC, which POSIX
// fsync/fdatasync both require. Parameters/ParametersSize must be NULL/0.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtFlushBuffersFileEx(HANDLE FileHandle, ULONG Flags, PVOID Parameters,
                     ULONG ParametersSize, PIO_STATUS_BLOCK IoStatusBlock);

// NtQueryInformationFile — query file metadata by information class.
// IoStatusBlock.Information receives the number of bytes written to the
// FileInformation buffer. Returns STATUS_BUFFER_OVERFLOW if the buffer
// is too small (partial data may be returned for variable-length classes).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                       PVOID FileInformation, ULONG Length,
                       FILE_INFORMATION_CLASS FileInformationClass);

// NtQueryInformationByName — query file info by path without opening (RS2+).
// More efficient than NtCreateFile + NtQueryInformationFile + NtClose for
// stat()-like operations. Supports FileStatBasicInformation (23H2+),
// FileBasicInformation, FileStandardInformation, FileNetworkOpenInformation.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryInformationByName(PCOBJECT_ATTRIBUTES ObjectAttributes,
                         PIO_STATUS_BLOCK IoStatusBlock,
                         PVOID FileInformation, ULONG Length,
                         FILE_INFORMATION_CLASS FileInformationClass);

// NtSetInformationFile — set file metadata by information class.
// Common uses:
//   FilePositionInformation      → lseek (sync handles only)
//   FileEndOfFileInformation     → ftruncate
//   FileAllocationInformation    → posix_fallocate
//   FileDispositionInformationEx → unlink (with FILE_DISPOSITION_POSIX_SEMANTICS)
//   FileRenameInformationEx      → rename (with FILE_RENAME_POSIX_SEMANTICS)
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                     PVOID FileInformation, ULONG Length,
                     FILE_INFORMATION_CLASS FileInformationClass);

// NtQueryDirectoryFile — enumerate directory entries.
// FileInformation buffer receives one or more entries of the requested
// FileInformationClass (linked by NextEntryOffset). Returns
// STATUS_NO_MORE_FILES when enumeration is complete.
//
// - ReturnSingleEntry: TRUE to return at most one entry per call.
// - FileName: optional wildcard filter (e.g., u"*.txt"). Only used on the
//   first call or when RestartScan is TRUE.
// - RestartScan: TRUE to restart from the beginning of the directory.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event,
                     PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                     PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                     ULONG Length,
                     FILE_INFORMATION_CLASS FileInformationClass,
                     BOOLEAN ReturnSingleEntry, PCUNICODE_STRING FileName,
                     BOOLEAN RestartScan);

// NtQueryDirectoryFileEx — enumerate with query flags (RS3+).
// Replaces the ReturnSingleEntry/RestartScan booleans with a single
// QueryFlags bitmask (FILE_QUERY_* constants). Also supports
// FILE_QUERY_NO_CURSOR_UPDATE for lock-free parallel enumeration.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryDirectoryFileEx(HANDLE FileHandle, HANDLE Event,
                       PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                       PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                       ULONG Length,
                       FILE_INFORMATION_CLASS FileInformationClass,
                       ULONG QueryFlags, PCUNICODE_STRING FileName);


//===----------------------------------------------------------------------===//
// File Locking — NtLockFile / NtUnlockFile
//===----------------------------------------------------------------------===//

// NtLockFile — lock a byte range in a file. Implements fcntl(F_SETLK/F_SETLKW)
// and flock(). Event/ApcRoutine enable async notification when a blocking lock
// is granted. FailImmediately=TRUE maps to F_SETLK (non-blocking);
// FailImmediately=FALSE maps to F_SETLKW (blocking wait).
// ExclusiveLock=TRUE for write locks (F_WRLCK), FALSE for read locks (F_RDLCK).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtLockFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
           PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
           LARGE_INTEGER *ByteOffset, LARGE_INTEGER *Length, ULONG Key,
           BOOLEAN FailImmediately, BOOLEAN ExclusiveLock);

// NtUnlockFile — unlock a previously locked byte range. ByteOffset, Length,
// and Key must exactly match a prior NtLockFile call. Implements F_UNLCK.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtUnlockFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
             LARGE_INTEGER *ByteOffset, LARGE_INTEGER *Length, ULONG Key);

//===----------------------------------------------------------------------===//
// Fast File Attribute Queries
//===----------------------------------------------------------------------===//

// NtQueryAttributesFile — fast path for basic stat() without opening the file.
// Returns FILE_BASIC_INFORMATION (timestamps + attributes) by path.
// Cheaper than NtCreateFile + NtQueryInformationFile + NtClose.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryAttributesFile(PCOBJECT_ATTRIBUTES ObjectAttributes,
                      FILE_BASIC_INFORMATION *FileInformation);

// NtQueryFullAttributesFile — richer fast-path stat returning timestamps,
// sizes, and attributes in one call. Returns FILE_NETWORK_OPEN_INFORMATION.
// Avoids opening the file; more data than NtQueryAttributesFile.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryFullAttributesFile(PCOBJECT_ATTRIBUTES ObjectAttributes,
                          FILE_NETWORK_OPEN_INFORMATION *FileInformation);

//===----------------------------------------------------------------------===//
// Kernel-Mode File Copy — NtCopyFileChunk
//===----------------------------------------------------------------------===//

// NtCopyFileChunk — copy a contiguous byte range between two open file handles
// entirely in kernel mode. Implements sendfile()/copy_file_range() semantics.
// Both handles must be open with appropriate read/write access.
// Windows 11 21H2+.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCopyFileChunk(HANDLE SourceHandle, HANDLE DestinationHandle, HANDLE Event,
                PIO_STATUS_BLOCK IoStatusBlock, ULONG Length,
                LARGE_INTEGER *SourceOffset, LARGE_INTEGER *DestOffset,
                ULONG *SourceKey, ULONG *DestKey, ULONG Flags);

//===----------------------------------------------------------------------===//
// Directory Change Notification
//===----------------------------------------------------------------------===//

// NtNotifyChangeDirectoryFileEx — watch a directory for changes (RS3+).
// Implements inotify-like filesystem monitoring. Buffer receives packed
// FILE_NOTIFY_*_INFORMATION entries. WatchTree=TRUE monitors subdirectories.
// CompletionFilter selects which change types to report.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtNotifyChangeDirectoryFileEx(HANDLE FileHandle, HANDLE Event,
                              PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                              PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer,
                              ULONG Length, ULONG CompletionFilter,
                              BOOLEAN WatchTree,
                              DIRECTORY_NOTIFY_INFORMATION_CLASS NotifyClass);

//===----------------------------------------------------------------------===//
// I/O Completion Ports — epoll/poll emulation
//===----------------------------------------------------------------------===//

// NtCreateIoCompletion — create an I/O completion port.
// NumberOfConcurrentThreads limits how many threads can concurrently dequeue
// completions (0 = number of processors). Core primitive for epoll() emulation.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateIoCompletion(HANDLE *IoCompletionHandle, ACCESS_MASK DesiredAccess,
                     PCOBJECT_ATTRIBUTES ObjectAttributes,
                     ULONG NumberOfConcurrentThreads);

// NtOpenIoCompletion — open an existing named I/O completion port.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtOpenIoCompletion(HANDLE *IoCompletionHandle, ACCESS_MASK DesiredAccess,
                   PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtQueryIoCompletion — query completion port state (queue depth).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtQueryIoCompletion(HANDLE IoCompletionHandle,
                    IO_COMPLETION_INFORMATION_CLASS IoCompletionInformationClass,
                    PVOID IoCompletionInformation,
                    ULONG IoCompletionInformationLength,
                    ULONG *ReturnLength);

// NtRemoveIoCompletionEx — dequeue multiple completions in one syscall.
// Batched epoll_wait() equivalent. Blocks until at least one completion is
// available or Timeout expires. Alertable=TRUE enables APC delivery.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtRemoveIoCompletionEx(HANDLE IoCompletionHandle,
                       FILE_IO_COMPLETION_INFORMATION *IoCompletionInformation,
                       ULONG Count, ULONG *NumEntriesRemoved,
                       LARGE_INTEGER *Timeout, BOOLEAN Alertable);

//===----------------------------------------------------------------------===//
// Wait Completion Packets — poll() on mixed handle types
//===----------------------------------------------------------------------===//

// NtCreateWaitCompletionPacket — create a wait completion packet object.
// Associates arbitrary waitable handles with an I/O completion port, enabling
// poll() on mixed handle types (events, mutants, processes, threads).
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCreateWaitCompletionPacket(HANDLE *WaitCompletionPacketHandle,
                             ACCESS_MASK DesiredAccess,
                             PCOBJECT_ATTRIBUTES ObjectAttributes);

// NtAssociateWaitCompletionPacket — link a packet to a completion port and
// target object. When TargetObjectHandle becomes signaled, a completion with
// the specified Key/Apc/IoStatus is queued to IoCompletionHandle.
// AlreadySignaled (optional) indicates if the target was already signaled.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAssociateWaitCompletionPacket(HANDLE WaitCompletionPacketHandle,
                                HANDLE IoCompletionHandle,
                                HANDLE TargetObjectHandle, PVOID KeyContext,
                                PVOID ApcContext, NTSTATUS IoStatus,
                                ULONG_PTR IoStatusInformation,
                                BOOLEAN *AlreadySignaled);

// NtCancelWaitCompletionPacket — cancel a previously associated packet.
// RemoveSignaledPacket=TRUE also removes an already-queued signaled packet.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtCancelWaitCompletionPacket(HANDLE WaitCompletionPacketHandle,
                             BOOLEAN RemoveSignaledPacket);

//===----------------------------------------------------------------------===//
// Reserve Objects — pre-allocated kernel resources for guaranteed delivery
//===----------------------------------------------------------------------===//

// NtAllocateReserveObject — pre-allocate a kernel reserve object.
// Type=MemoryReserveIoCompletion guarantees NtSetIoCompletionEx cannot fail
// under memory pressure. Reusable handle.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtAllocateReserveObject(HANDLE *MemoryReserveHandle,
                        PCOBJECT_ATTRIBUTES ObjectAttributes,
                        MEMORY_RESERVE_TYPE Type);

// NtSetIoCompletionEx — post a completion using a pre-allocated reserve.
// Cannot fail with STATUS_NO_MEMORY when MemoryReserveHandle is valid.
NTAPI __LIBC_EXTERN_DLLIMPORT_ATTR NTSTATUS
NtSetIoCompletionEx(HANDLE IoCompletionHandle, HANDLE IoCompletionReserveHandle,
                    PVOID KeyContext, PVOID ApcContext, NTSTATUS IoStatus,
                    ULONG_PTR IoStatusInformation);

} // extern "C"

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_FILE_API_H
