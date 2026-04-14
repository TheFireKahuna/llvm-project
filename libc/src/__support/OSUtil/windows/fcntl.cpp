//===-- Windows implementation of internal fcntl --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// File I/O kernel functions for Windows. These implement Linux syscall
// semantics in userspace: return value on success, -errno on failure.
// Called from the syscall_impl dispatch (syscall.h) after constant folding.
//
// internal::fcntl retains ErrorOr<int> for cross-platform compatibility
// (Linux uses it too). All other functions use the kernel convention.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/fcntl.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/off_t.h"
#include "hdr/types/struct_flock.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/fifo.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/io/virtual_fs.h"
#include "src/__support/OSUtil/windows/fcntl_lock_table.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/path_resolver.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t openat(int dirfd, const char *path, int flags, mode_t mode) {
  // Classify and resolve the path up front. This determines whether the path
  // is a virtual device (/dev/null, /dev/ptmx, /dev/fd/N, etc.) or a real
  // filesystem path that needs NtCreateFile.
  auto path_buf_s = path_scratch();
  if (!path_buf_s) return -ENOMEM;
  WCHAR *path_buf = path_buf_s.data();
  ResolvedPath rp = resolve_path(path, path_buf, path_buf_s.size());
  if (rp.error)
    return -static_cast<intptr_t>(rp.error);

  // Virtual path dispatch — handled entirely in userspace, no NtCreateFile.
  // Also handle relative paths (e.g. "pts/0") when dirfd is a virtual
  // directory (/dev or /dev/pts) — virtual_fs::openat resolves these
  // relative to the virtual directory state stored in the OFD.
  bool is_virtual_relative = false;
  if (rp.kind == PathKind::DosRelative && path[0] != '\0') {
    OpenFileDescription *ofd = fd_table.get_ofd(dirfd);
    if (ofd && ofd->is_virtual_dir())
      is_virtual_relative = true;
  }

  if (rp.kind == PathKind::DevPty || is_virtual_relative) {
    // PTY nodes (/dev, /dev/pts, /dev/ptmx, /dev/pts/<N>) and relative
    // paths within virtual directories are handled by the virtual
    // filesystem layer which manages pseudo-terminal state.
    ErrorOr<int> vr = virtual_fs::openat(dirfd, path, flags);
    if (!vr.has_value())
      return -static_cast<intptr_t>(vr.error());
    return static_cast<intptr_t>(vr.value());
  }

  switch (rp.kind) {
  case PathKind::DevFd: {
    // /dev/fd/<n>, /dev/stdin, /dev/stdout, /dev/stderr: dup the fd.
    int new_fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    auto result = fd_table.dup(rp.virtual_fd, 0, new_fd_flags);
    if (!result.has_value())
      return -static_cast<intptr_t>(result.error());
    return static_cast<intptr_t>(result.value());
  }
  case PathKind::Invalid:
    return -EINVAL;
  default:
    break; // Fall through to NT kernel open path.
  }

  // For device paths (DevNull, DevZero, DevRandom, DevUrandom, DevTty),
  // path_buf already contains the NT device path (e.g. \Device\Null).
  // For filesystem paths, we still need resolve_at_path for dirfd support.
  // Build OBJECT_ATTRIBUTES from the resolved path.
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;

  bool is_device_node = rp.kind == PathKind::DevNull ||
                        rp.kind == PathKind::DevZero ||
                        rp.kind == PathKind::DevRandom ||
                        rp.kind == PathKind::DevUrandom ||
                        rp.kind == PathKind::DevTty;

  if (is_device_node) {
    // Device paths are absolute NT object paths — dirfd is irrelevant.
    // O_CREAT/O_EXCL/O_TRUNC don't make sense for devices.
    if (flags & (O_CREAT | O_EXCL | O_TRUNC))
      return -EINVAL;
    init_object_attributes(&oa, &us, path_buf, rp.nt_len);
  } else if (dirfd == AT_FDCWD || rp.nt_len > 0) {
    // For AT_FDCWD or absolute paths: resolve_path already wrote the NT
    // path to path_buf — reuse it directly, no need to re-resolve.
    // rp.nt_len > 0 means the path was fully resolved (absolute POSIX,
    // DOS, UNC, rooted, /tmp — all produce an NT path regardless of dirfd).
    if (rp.nt_len == 0) {
      // AT_FDCWD with a relative path: resolve_path wrote the CWD-resolved
      // NT path. This can't be 0 for valid relative paths.
      return -EINVAL;
    }
    init_object_attributes(&oa, &us, path_buf, rp.nt_len);
  } else {
    // Relative path with a non-AT_FDCWD dirfd: need to resolve relative
    // to the directory handle. resolve_at_path handles this via
    // RootDirectory in OBJECT_ATTRIBUTES.
    int err = resolve_at_path(dirfd, path, path_buf, path_buf_s.size(),
                              &oa, &us);
    if (err)
      return -static_cast<intptr_t>(err);
  }

  // O_PATH: open a path-only descriptor for metadata queries and dirfd use.
  // The handle has minimal access rights (no FILE_READ_DATA/FILE_WRITE_DATA)
  // and I/O operations (read, write, mmap, etc.) are rejected by is_path_only()
  // checks throughout the kernel layer.
  if (flags & O_PATH) {
    // Mutation flags are meaningless for path-only descriptors.
    if (flags & (O_CREAT | O_EXCL | O_TRUNC))
      return -EINVAL;

    ACCESS_MASK path_access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
    ULONG path_share =
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    // FILE_OPEN_FOR_BACKUP_INTENT is required to open directories with
    // minimal access rights — without it, NtCreateFile returns
    // STATUS_FILE_IS_A_DIRECTORY on some configurations.
    ULONG path_options = FILE_OPEN_FOR_BACKUP_INTENT;
    if (flags & O_DIRECTORY)
      path_options |= FILE_DIRECTORY_FILE;
    if (flags & O_NOFOLLOW)
      path_options |= FILE_OPEN_REPARSE_POINT;

    HANDLE handle;
    NTSTATUS status = open_overlapped(&oa, path_access, FILE_OPEN,
                                      path_share, &handle, path_options);
    if (!NT_SUCCESS(status))
      return windows_util::ntstatus_to_kerr(status);

    // O_NOFOLLOW + O_PATH: do NOT reject symlinks. The combination
    // opens an fd referring to the symlink itself — this is the primary
    // mechanism for getting a handle to a symlink (for readlinkat with
    // AT_EMPTY_PATH, fstatat, etc.). FILE_OPEN_REPARSE_POINT above
    // already ensures we opened the reparse point, not the target.

    // Allocate fd. O_PATH is stored in ofd->immutable_flags by init().
    // O_ACCMODE is forced to O_RDONLY — the handle has no data access.
    int alloc_flags = O_RDONLY | O_PATH;
    if (flags & O_CLOEXEC)
      alloc_flags |= O_CLOEXEC;
    auto fd_result = fd_table.alloc(handle, alloc_flags);
    if (!fd_result.has_value()) {
      NtClose(handle);
      return -static_cast<intptr_t>(fd_result.error());
    }

    if (flags & O_CLOEXEC)
      fd_table.set_fd_cloexec(fd_result.value(), true);

    return static_cast<intptr_t>(fd_result.value());
  }

  // Translate O_* flags to NT access mask and disposition.
  ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
  ULONG disposition;
  int accmode = flags & O_ACCMODE;

  if (accmode == O_RDONLY)
    access |= FILE_READ_DATA;
  else if (accmode == O_WRONLY)
    access |= FILE_WRITE_DATA;
  else if (accmode == O_RDWR)
    access |= FILE_READ_DATA | FILE_WRITE_DATA;

  if (flags & O_APPEND)
    access |= FILE_APPEND_DATA;

  // Disposition logic: O_CREAT/O_EXCL/O_TRUNC combinations.
  if (is_device_node) {
    disposition = FILE_OPEN; // Devices always exist, just open.
  } else if (flags & O_EXCL) {
    disposition = FILE_CREATE; // fail if exists
  } else if ((flags & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC)) {
    disposition = FILE_OVERWRITE_IF; // create or truncate
  } else if (flags & O_CREAT) {
    disposition = FILE_OPEN_IF; // create if absent, open if present
  } else if (flags & O_TRUNC) {
    disposition = FILE_OVERWRITE; // must exist, truncate
  } else {
    disposition = FILE_OPEN; // must exist
  }

  ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

  // Map O_DIRECTORY and O_NOFOLLOW to NT create options.
  // Default: no FILE_NON_DIRECTORY_FILE — allows both files and directories.
  // POSIX requires open(dir, O_RDONLY) to succeed for use with fstatat, etc.
  ULONG options = 0;
  if (flags & O_DIRECTORY) {
    // Must be a directory — fail with ENOTDIR otherwise.
    options = FILE_DIRECTORY_FILE;
  } else if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
    // Write/create/truncate operations are not valid on directories.
    options = FILE_NON_DIRECTORY_FILE;
  }
  if (flags & O_NOFOLLOW) {
    // Open reparse points directly instead of following them.
    // After open, we check for symlinks and fail with ELOOP.
    options |= FILE_OPEN_REPARSE_POINT;
  }
  if (flags & O_DIRECT) {
    // Bypass the filesystem cache — all reads/writes go directly to disk.
    // Size and offset must be sector-aligned. Enables MDL pre-pinning
    // benefit in the IoRing registered buffer path.
    options |= FILE_NO_INTERMEDIATE_BUFFERING;
  }

  // Build an atomic SECURITY_DESCRIPTOR for file creation. The SD is
  // passed via oa.SecurityDescriptor so NtCreateFile applies the DACL
  // at creation time — no race window, no WRITE_DAC needed on the handle.
  // For EA + READONLY we still need FILE_WRITE_EA and FILE_WRITE_ATTRIBUTES.
  auto sd_s = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
  SECURITY_DESCRIPTOR *creation_sd = nullptr;
  if ((flags & O_CREAT) && !is_device_node && sd_s) {
    mode_t effective = mode & ~windows_sec::get_umask();
    creation_sd = windows_sec::build_creation_sd(
        reinterpret_cast<UCHAR *>(sd_s.data()), effective);
    oa.SecurityDescriptor = creation_sd; // nullptr if build failed → inherit.
    access |= FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
  }

  // Open overlapped handle. IO Ring + event created by fd_table.alloc().
  HANDLE handle;
  ULONG_PTR create_info = 0;
  NTSTATUS status = open_overlapped(&oa, access, disposition, share, &handle,
                                    options, FILE_ATTRIBUTE_NORMAL,
                                    &create_info);
  if (!NT_SUCCESS(status))
    return windows_util::ntstatus_to_kerr(status);

  // Device nodes skip FIFO detection and symlink checks.
  if (!is_device_node) {
    // Query basic info once — used by O_NOFOLLOW and FIFO detection.
    FILE_BASIC_INFORMATION basic = {};
    IO_STATUS_BLOCK basic_iosb = {};
    NtQueryInformationFile(handle, &basic_iosb, &basic, sizeof(basic),
                            FileBasicInformation);

    // O_NOFOLLOW: reject symlinks. NtCreateFile with FILE_OPEN_REPARSE_POINT
    // opens the reparse point itself — check if it's actually a symlink.
    if ((flags & O_NOFOLLOW) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      NtClose(handle);
      return -ELOOP;
    }

    // FIFO detection: check if this is a mkfifo marker file.
    // Filter: SYSTEM attribute + small size → read magic bytes.
    if ((basic.FileAttributes & FILE_ATTRIBUTE_SYSTEM) &&
        !(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      FILE_STANDARD_INFORMATION std_info = {};
      IO_STATUS_BLOCK std_iosb = {};
      NTSTATUS qst = NtQueryInformationFile(handle, &std_iosb, &std_info,
                                             sizeof(std_info),
                                             FileStandardInformation);
      if (NT_SUCCESS(qst) && !std_info.Directory &&
          std_info.EndOfFile.QuadPart >= 8 &&
          std_info.EndOfFile.QuadPart <= static_cast<LONGLONG>(
                                             FIFO_MARKER_SIZE)) {
        if (is_fifo_marker(handle)) {
          // Check permissions before opening the channel.
          mode_t fifo_mode = read_fifo_mode(handle);
          NtClose(handle);
          int perr = check_fifo_perms(fifo_mode, flags);
          if (perr)
            return -static_cast<intptr_t>(perr);
          auto fifo_result = open_fifo(path, flags, fifo_mode);
          if (!fifo_result.has_value())
            return -static_cast<intptr_t>(fifo_result.error());
          return static_cast<intptr_t>(fifo_result.value());
        }
      }
    }

    // Write EA + sync READONLY for newly created files. The DACL was
    // already applied atomically via oa.SecurityDescriptor above.
    if (create_info == FILE_CREATED_RESULT) {
      mode_t effective = mode & ~windows_sec::get_umask();
      windows_sec::post_create_perms(handle, effective);
    }
  }

  // Allocate fd from the table. /dev/zero gets a DevZero kind override
  // so the read path returns zero-filled buffers instead of real I/O.
  FileKind kind_hint = (rp.kind == PathKind::DevZero) ? FileKind::DevZero
                                                      : FileKind::Auto;
  auto fd_result = fd_table.alloc(handle, flags, 0, kind_hint);
  if (!fd_result.has_value()) {
    NtClose(handle);
    return -static_cast<intptr_t>(fd_result.error());
  }

  // O_CLOEXEC -> set FD_CLOEXEC and revoke OBJ_INHERIT on the handle.
  if (flags & O_CLOEXEC)
    fd_table.set_fd_cloexec(fd_result.value(), true);

  return static_cast<intptr_t>(fd_result.value());
}

intptr_t open(const char *path, int flags, mode_t mode) {
  return openat(AT_FDCWD, path, flags, mode);
}

intptr_t lseek(int fd, off_t offset, int whence) {
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return -EBADF;

  if (ofd->is_path_only())
    return -EBADF;

  if (!ofd->is_seekable())
    return -ESPIPE;

  int64_t newpos;

  if (whence == SEEK_SET) {
    newpos = static_cast<int64_t>(offset);
    ofd->disk().position.store(newpos, cpp::MemoryOrder::RELEASE);
  } else if (whence == SEEK_CUR) {
    newpos = ofd->disk().position.fetch_add(static_cast<int64_t>(offset),
                                     cpp::MemoryOrder::ACQ_REL) +
             static_cast<int64_t>(offset);
  } else if (whence == SEEK_END) {
    // Lock-free SEEK_END: query file size, compute target, store position.
    // No lock needed — SEEK_END's result is fully determined by the file
    // size at the moment of the query, independent of the current position.
    HANDLE h = ofd->handle;
    FILE_STANDARD_INFORMATION std_info;
    IO_STATUS_BLOCK iosb = {};
    NTSTATUS status = NtQueryInformationFile(
        h, &iosb, &std_info, static_cast<ULONG>(sizeof(std_info)),
        FileStandardInformation);
    if (!NT_SUCCESS(status))
      return -EIO;
    newpos = std_info.EndOfFile.QuadPart + static_cast<int64_t>(offset);
    // Unconditional store: SEEK_END sets position to file_size + offset
    // regardless of the current position value. No CAS needed — the result
    // is fully determined by the file size query above. Concurrent position
    // mutations (read/write fetch_add, other seeks) are POSIX-unspecified
    // for SEEK_END and handled correctly by last-writer-wins.
    ofd->disk().position.store(newpos, cpp::MemoryOrder::RELEASE);
  } else {
    return -EINVAL;
  }

  if (newpos < 0)
    return -EINVAL;

  return static_cast<intptr_t>(newpos);
}

intptr_t dup2(int oldfd, int newfd) {
  if (oldfd == newfd) {
    // Verify oldfd is valid.
    if (!fd_table.get(oldfd).has_value())
      return -EBADF;
    return oldfd;
  }

  auto result = fd_table.dup_to(oldfd, newfd);
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return newfd;
}

intptr_t close(int fd) {
  // Release the fd slot and decrement the OFD refcount.
  // OFD::release() handles FIFO cleanup, handle close, ring/event close.
  auto result = fd_table.release(fd);
  if (!result.has_value())
    return -static_cast<intptr_t>(result.error());
  return 0;
}

ErrorOr<int> fcntl(int fd, int cmd, void *arg) {
  // F_GETFD / F_SETFD operate on the per-fd slot (FD_CLOEXEC).
  // F_GETFL / F_SETFL operate on the shared OFD (status flags).
  if (cmd == F_GETFD || cmd == F_SETFD) {
    FdSlot *slot = fd_table.get_slot(fd);
    if (!slot || !slot->load_ofd(cpp::MemoryOrder::ACQUIRE))
      return Error(EBADF);
    if (cmd == F_GETFD)
      return slot->cloexec() ? FD_CLOEXEC : 0;
    int val = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    bool cloexec = (val & FD_CLOEXEC) != 0;
    // Set the slot bit AND toggle OBJ_INHERIT on the kernel handle.
    fd_table.set_fd_cloexec(fd, cloexec);
    return 0;
  }

  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (!ofd)
    return Error(EBADF);

  switch (cmd) {
  case F_GETFL: {
    // Reconstruct full flags: access_mode | status_flags | immutable flags.
    int result = ofd->access_mode |
                 ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    if (ofd->is_path_only())
      result |= O_PATH;
    return result;
  }

  case F_SETFL: {
    // O_PATH descriptors have no mutable status flags.
    if (ofd->is_path_only())
      return Error(EBADF);
    int val = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    // Only O_APPEND and O_NONBLOCK are modifiable per POSIX.
    int old_flags = ofd->status_flags.load(cpp::MemoryOrder::ACQUIRE);
    int new_flags = (old_flags & ~(O_APPEND | O_NONBLOCK)) |
                    (val & (O_APPEND | O_NONBLOCK));
    ofd->status_flags.store(new_flags, cpp::MemoryOrder::RELEASE);
    return 0;
  }

  case F_DUPFD:
  case F_DUPFD_CLOEXEC: {
    int min_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    int fd_flags = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
    return fd_table.dup(fd, min_fd, fd_flags);
  }

  case F_SETLK:
  case F_SETLKW:
  case F_GETLK: {
    if (ofd->is_path_only())
      return Error(EBADF);

    struct flock *flk = reinterpret_cast<struct flock *>(arg);
    if (!flk)
      return Error(EFAULT);

    // Resolve the byte range from l_whence/l_start/l_len to absolute offset.
    int64_t base;
    if (flk->l_whence == SEEK_SET) {
      base = 0;
    } else if (flk->l_whence == SEEK_CUR) {
      base = ofd->disk().position.load(cpp::MemoryOrder::ACQUIRE);
    } else if (flk->l_whence == SEEK_END) {
      FILE_STANDARD_INFORMATION std_info;
      IO_STATUS_BLOCK iosb = {};
      NTSTATUS st = NtQueryInformationFile(
          ofd->handle, &iosb, &std_info,
          static_cast<ULONG>(sizeof(std_info)), FileStandardInformation);
      if (!NT_SUCCESS(st))
        return Error(EIO);
      base = std_info.EndOfFile.QuadPart;
    } else {
      return Error(EINVAL);
    }

    int64_t start = base + static_cast<int64_t>(flk->l_start);
    if (start < 0)
      return Error(EINVAL);

    // l_len == 0 means "to end of file" per POSIX -- use max range.
    int64_t length;
    if (flk->l_len == 0) {
      length = INT64_MAX - start;
    } else if (flk->l_len > 0) {
      length = static_cast<int64_t>(flk->l_len);
    } else {
      // Negative l_len: lock region before l_start.
      length = -static_cast<int64_t>(flk->l_len);
      start -= length;
      if (start < 0)
        return Error(EINVAL);
    }

    int64_t end = start + length;
    // Overflow check: start and length are both non-negative here.
    if (end < start)
      return Error(EINVAL);

    if (cmd == F_GETLK) {
      // F_GETLK: probe for locks held by OTHER processes.
      // The lock table temporarily releases our own overlapping locks so
      // the NtLockFile probe detects only external conflicts.
      int16_t out_type = 0;
      pid_t out_pid = 0;
      int rc =
          lock_table_getlk(ofd->handle, start, end, flk->l_type, out_type,
                           out_pid);
      if (rc < 0)
        return Error(-rc);
      flk->l_type = out_type;
      flk->l_pid = (out_type == F_UNLCK) ? 0 : out_pid;
      return 0;
    }

    // F_SETLK / F_SETLKW -- routed through the lock table for POSIX
    // merge/split semantics and fork/close tracking.
    bool blocking = (cmd == F_SETLKW);
    int rc = lock_table_set(ofd->handle, start, end, flk->l_type, blocking);
    if (rc < 0)
      return Error(-rc);
    return 0;
  }

  default:
    return Error(EINVAL);
  }
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
