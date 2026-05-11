//===-- NTSTATUS to POSIX errno conversion ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Maps NT status codes to POSIX errno values. Pure ntdll-level — no
// dependency on kernel32 or Win32 error codes.
//
// Single canonical switch in ntstatus_to_errno(); ntstatus_to_kerr() delegates
// to it and negates. This eliminates the prior duplicate switch statements.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_ERROR_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_ERROR_H

#include "src/__support/OSUtil/windows/nt/nt_process_types.h"

#include "hdr/errno_macros.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows_util {

// Error code conversion: Win32 last-error value to POSIX errno.
LIBC_INLINE int win32_to_errno(DWORD error) {
  switch (error) {
  case ERROR_SUCCESS:
    return 0;
  case ERROR_INVALID_HANDLE:
    return EBADF;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    return ENOMEM;
  case ERROR_INVALID_PARAMETER:
    return EINVAL;
  case ERROR_ACCESS_DENIED:
    return EACCES;
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return ENOENT;
  case ERROR_FILE_EXISTS:
  case ERROR_ALREADY_EXISTS:
    return EEXIST;
  case ERROR_BROKEN_PIPE:
    return EPIPE;
  case ERROR_NOT_SUPPORTED:
    return ENOTSUP;
  case ERROR_TIMEOUT:
    return ETIMEDOUT;
  case ERROR_NOT_LOCKED:
    return ENOMEM;
  case ERROR_WORKING_SET_QUOTA:
    return ENOMEM;
  case ERROR_BUSY:
    return EBUSY;
  case ERROR_INVALID_ADDRESS:
    return EINVAL;
  case ERROR_SHARING_VIOLATION:
    return ETXTBSY;
  case ERROR_BAD_EXE_FORMAT:
    return ENOEXEC;
  case ERROR_FILENAME_EXCED_RANGE:
    return ENAMETOOLONG;
  case ERROR_DIRECTORY:
    return ENOTDIR;
  case ERROR_CANT_RESOLVE_FILENAME:
    return ELOOP;
  default:
    return EIO;
  }
}

LIBC_INLINE int get_last_error_as_errno() {
  return win32_to_errno(::NtGetLastError());
}

// Error code conversion: NTSTATUS to POSIX errno.
//
// Design notes:
//   - Context-dependent overrides (e.g., STATUS_ACCESS_DENIED → EBADF in
//     posix_fallocate, or → EPERM in clock_getcpuclockid) remain at their
//     call sites. This table provides the most common/general mapping.
//   - STATUS_USER_APC/STATUS_ALERTED/STATUS_CANCELLED map to EINTR here as a
//     safety net. STATUS_PENDING and STATUS_TIMEOUT are deliberately excluded —
//     they are success-class flow control codes that callers must handle
//     contextually (async-in-flight vs. timed-out-wait).
LIBC_INLINE int ntstatus_to_errno(NTSTATUS status) {
  switch (status) {
  // --- Success / informational (not errors) ---
  case STATUS_SUCCESS:
  case STATUS_END_OF_FILE: // Caller interprets as EOF.
    return 0;

  // --- Interruption / cancellation ---
  case STATUS_CANCELLED:
  case STATUS_USER_APC:
  case STATUS_ALERTED:
    return EINTR;

  // --- Invalid argument family ---
  case STATUS_INVALID_PARAMETER:
  case STATUS_CONFLICTING_ADDRESSES:
  case STATUS_INVALID_VIEW_SIZE:
  case STATUS_NOT_MAPPED_VIEW:
  case STATUS_NOT_A_REPARSE_POINT:
  case STATUS_ILLEGAL_FUNCTION: // ConDrv: invalid IOCTL sub-function.
  case STATUS_INVALID_CID:
    return EINVAL;

  // --- Exec / image format ---
  case STATUS_INVALID_IMAGE_FORMAT:
  case STATUS_INVALID_IMAGE_NOT_MZ:
  case STATUS_INVALID_IMAGE_NE_FORMAT:
  case STATUS_INVALID_IMAGE_PROTECT:
    return ENOEXEC;

  // --- Permission / access ---
  case STATUS_ACCESS_DENIED:
  case STATUS_INVALID_PAGE_PROTECTION:
  // mprotect on a file section with incompatible access (e.g., PROT_WRITE on
  // a read-only mapping). POSIX requires EACCES for this case.
  case STATUS_SECTION_PROTECTION:
  // ACG (Arbitrary Code Guard) active: W^X policy violation — making an
  // executable page writable or a writable page executable.
  case STATUS_DYNAMIC_CODE_BLOCKED:
    return EACCES;

  case STATUS_PRIVILEGE_NOT_HELD:
  case STATUS_LOGON_FAILURE:
  case STATUS_ACCOUNT_RESTRICTION:
  case STATUS_WRONG_PASSWORD:
  case STATUS_ACCOUNT_DISABLED:
  case STATUS_ACCOUNT_EXPIRED:
  case STATUS_PASSWORD_EXPIRED:
    return EPERM;

  // --- Memory / resource exhaustion ---
  case STATUS_NO_MEMORY:
  case STATUS_COMMITMENT_LIMIT:
  case STATUS_WORKING_SET_QUOTA:
  case STATUS_INSUFFICIENT_RESOURCES:
  case STATUS_TOO_MANY_PAGING_FILES:
  case STATUS_SECTION_NOT_EXTENDED:
  // ENOLCK would be semantically wrong (file locking concept). ENOMEM is
  // what POSIX specifies for munlock on non-locked pages, though in
  // practice we handle STATUS_NOT_LOCKED as success before reaching here.
  case STATUS_NOT_LOCKED:
    return ENOMEM;

  case STATUS_QUOTA_EXCEEDED:
    return EDQUOT;

  // --- Busy / contention ---
  case STATUS_ALREADY_COMMITTED:
  case STATUS_SHARING_VIOLATION:
  case STATUS_CANNOT_DELETE:
  case STATUS_DEVICE_BUSY:
  // File has user-mode mappings blocking destructive op (truncate /
  // delete / unlink). POSIX answer is EBUSY so the caller can retry
  // once the mapping is released.
  case STATUS_USER_MAPPED_FILE:
    return EBUSY;

  // --- Not-found family ---
  case STATUS_OBJECT_NAME_NOT_FOUND:
  case STATUS_OBJECT_PATH_NOT_FOUND:
  case STATUS_OBJECT_NAME_INVALID:
  case STATUS_OBJECT_PATH_SYNTAX_BAD:
  case STATUS_OBJECT_PATH_INVALID:
  case STATUS_NO_SUCH_FILE:
  case STATUS_DLL_NOT_FOUND:
  case STATUS_NOT_FOUND:
  case STATUS_DELETE_PENDING: // File is being deleted — effectively gone.
  case STATUS_NO_SUCH_USER:
    return ENOENT;

  case STATUS_OBJECT_NAME_COLLISION:
    return EEXIST;

  // --- Bad file descriptor / handle ---
  case STATUS_INVALID_HANDLE:
  case STATUS_OBJECT_TYPE_MISMATCH: // Handle exists but wrong type.
  case STATUS_FILE_CLOSED:
    return EBADF;

  // --- Directory operations ---
  case STATUS_FILE_IS_A_DIRECTORY:
    return EISDIR;
  case STATUS_NOT_A_DIRECTORY:
    return ENOTDIR;
  case STATUS_DIRECTORY_NOT_EMPTY:
    return ENOTEMPTY;

  // --- Cross-device / rename ---
  case STATUS_NOT_SAME_DEVICE:
    return EXDEV;
  case STATUS_FILE_RENAMED:
  case STATUS_NETWORK_NAME_DELETED:
    return ESTALE;

  // --- Pipe / FIFO ---
  case STATUS_PIPE_DISCONNECTED:
  case STATUS_PIPE_BROKEN:
  case STATUS_PIPE_CLOSING:
  case STATUS_LOCAL_DISCONNECT:
    return EPIPE;
  case STATUS_PIPE_NOT_AVAILABLE:
  case STATUS_INSTANCE_NOT_AVAILABLE:
    return ENXIO;

  // --- Disk / filesystem ---
  case STATUS_DISK_FULL:
    return ENOSPC;
  case STATUS_MEDIA_WRITE_PROTECTED:
    return EROFS;
  case STATUS_FILE_CORRUPT_ERROR:
  case STATUS_DISK_CORRUPT_ERROR:
    return EIO;

  // --- Not supported / not implemented ---
  case STATUS_NOT_SUPPORTED:
    return ENOTSUP;
  case STATUS_ENTRYPOINT_NOT_FOUND:
  case STATUS_NOT_IMPLEMENTED:
  case STATUS_INVALID_SYSTEM_SERVICE: // Bad syscall number.
    return ENOSYS;
  case STATUS_INVALID_DEVICE_REQUEST:
    return ENOTTY;

  // --- Size / overflow ---
  case STATUS_BUFFER_OVERFLOW:
    return EMSGSIZE;
  case STATUS_BUFFER_TOO_SMALL:
    return ERANGE;
  case STATUS_TOO_MANY_LINKS:
    return EMLINK;
  case STATUS_NAME_TOO_LONG:
    return ENAMETOOLONG;
  case STATUS_FILE_TOO_LARGE:
  // Section creation request exceeds either the kernel's per-section
  // ceiling or the backing file's size. Surfaces from
  // `NtCreateSectionEx` on file-backed mappings.
  case STATUS_SECTION_TOO_BIG:
    return EFBIG;
  case STATUS_INTEGER_OVERFLOW:
    return EOVERFLOW;

  // --- Memory fault ---
  case STATUS_INVALID_USER_BUFFER:
  case STATUS_ACCESS_VIOLATION: // Bad pointer dereference.
    return EFAULT;

  // --- File locking ---
  case STATUS_LOCK_NOT_GRANTED:
  case STATUS_FILE_LOCK_CONFLICT:
    return EAGAIN;
  case STATUS_RANGE_NOT_LOCKED:
    return ENOLCK;

  // --- Network / sockets ---
  case STATUS_CONNECTION_ABORTED:
    return ECONNABORTED;
  case STATUS_CONNECTION_RESET:
  case STATUS_CONNECTION_DISCONNECTED:
  case STATUS_REMOTE_DISCONNECT:
  case STATUS_GRACEFUL_DISCONNECT:
    return ECONNRESET;
  case STATUS_CONNECTION_REFUSED:
  case STATUS_PORT_UNREACHABLE:
    return ECONNREFUSED;
  case STATUS_HOST_UNREACHABLE:
    return EHOSTUNREACH;
  case STATUS_NETWORK_UNREACHABLE:
    return ENETUNREACH;
  case STATUS_ADDRESS_ALREADY_EXISTS:
    return EADDRINUSE;
  case STATUS_ADDRESS_CLOSED:
    return EADDRNOTAVAIL;
  case STATUS_CONNECTION_INVALID:
    return ENOTCONN;
  case STATUS_IO_TIMEOUT:
    return ETIMEDOUT;
  case STATUS_DEVICE_NOT_READY:
    return EAGAIN;

  // --- Process / thread lifecycle ---
  case STATUS_THREAD_IS_TERMINATING:
  case STATUS_PROCESS_IS_TERMINATING:
    return ESRCH;

  // --- Too many open files ---
  case STATUS_TOO_MANY_OPENED_FILES:
    return EMFILE;
  case STATUS_TOO_MANY_THREADS:
    return EAGAIN;

  // --- Synchronization ---
  // STATUS_MUTANT_NOT_OWNED (EPERM) and STATUS_SEMAPHORE_LIMIT_EXCEEDED
  // (EOVERFLOW) are defined in nt_process_types.h; callers that include
  // that header should map them locally or use ntstatus_to_errno_ext().

  // --- Illegal instruction (hardware trap) ---
  case STATUS_ILLEGAL_INSTRUCTION:
    return EINVAL; // SIGILL territory; errno rarely relevant.

  // --- Generic failure ---
  case STATUS_UNSUCCESSFUL:
    return EIO;

  default:
    return EIO;
  }
}

// Kernel-convention conversion: NTSTATUS to negative errno.
// Returns 0 on success, -EINVAL / -ENOMEM / -EACCES / etc. on failure.
// Use in internal:: kernel functions that return long with -errno.
LIBC_INLINE long ntstatus_to_kerr(NTSTATUS status) {
  int e = ntstatus_to_errno(status);
  return e ? -static_cast<long>(e) : 0;
}

// Shorthand for `Error(ntstatus_to_errno(status))`. Lets callers write
// `return nt_error(status);` at sites that return an ErrorOr<T>.
LIBC_INLINE LIBC_NAMESPACE::Error nt_error(NTSTATUS status) {
  return LIBC_NAMESPACE::Error(ntstatus_to_errno(status));
}

// Shorthand for `-ntstatus_to_errno(status)`. Lets callers that return
// negative-errno ints write `return nt_neg_errno(status);`.
LIBC_INLINE int nt_neg_errno(NTSTATUS status) {
  return -ntstatus_to_errno(status);
}

} // namespace windows_util
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_NT_ERROR_H
