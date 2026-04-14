//===--- Windows implementation of the Dir helpers ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Uses NtQueryDirectoryFileEx with FileIdExtdBothDirectoryInformation to
// enumerate directory entries. Each NT entry is repacked into a struct dirent
// with d_reclen for buffer traversal, d_ino from the 128-bit FileId (lower
// 64 bits, with fallback to FileIndex if zero), and d_type derived from
// FileAttributes + ReparsePointTag.
//
// d_type mapping:
//   DT_LNK  — IO_REPARSE_TAG_SYMLINK (file or directory symlinks)
//   DT_SOCK — IO_REPARSE_TAG_AF_UNIX (Win10 1803+ AF_UNIX endpoints)
//   DT_DIR  — FILE_ATTRIBUTE_DIRECTORY (including mount points/junctions)
//   DT_FIFO — FIFO marker files (SYSTEM|HIDDEN, ≤16 bytes, "LLVMFIFO" magic)
//   DT_REG  — everything else
//
// FIFO detection requires a relative NtOpenFile + 8-byte read per candidate.
// The candidate filter (SYSTEM|HIDDEN + size check) ensures this probe only
// fires for actual marker files, not regular hidden/system files.
//
// seekdir is O(n) on NT (no kernel directory position cookie). The skip
// phase uses FileNamesInformation (~12 bytes/entry vs ~120) with a 16KB
// buffer to minimize kernel transitions during the restart-and-skip.
// Concurrent directory modification between telldir/seekdir is POSIX-
// undefined; our monotonic counter handles all edge cases gracefully.
//
// The directory handle is opened with FILE_SYNCHRONOUS_IO_NONALERT (not
// _ALERT) — directory enumeration doesn't need EINTR/APC support.
//
//===----------------------------------------------------------------------===//

#include "src/__support/File/dir.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/ipc/fifo.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"

#include <dirent.h> // DT_* macros, struct dirent

namespace LIBC_NAMESPACE_DECL {

ErrorOr<int> platform_opendir(const char *name) {
  // POSIX: opendir("") shall fail with ENOENT.
  if (name[0] == '\0')
    return Error(ENOENT);

  WCHAR path_buf[MAX_NT_PATH_WCHARS];
  size_t path_len = to_nt_path(name, path_buf, MAX_NT_PATH_WCHARS);
  if (path_len == 0)
    return Error(ENAMETOOLONG);

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, path_buf, path_len);

  HANDLE handle;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = NtCreateFile(
      &handle, FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE, &oa, &iosb,
      nullptr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr,
      0);
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));

  auto fd = internal::fd_table.alloc(handle, O_RDONLY | O_DIRECTORY);
  if (!fd) {
    NtClose(handle);
    return Error(fd.error());
  }
  return fd.value();
}

// Map NT attributes + reparse tag to DT_* type.
// Symlinks (including directory symlinks) are DT_LNK per POSIX.
// Mount points/junctions are directory redirections (like bind mounts on
// Linux) and are reported as DT_DIR.
// AF_UNIX socket endpoints (Win10 1803+) use IO_REPARSE_TAG_AF_UNIX.
// FIFO marker files require a separate probe (see probe_fifo_marker).
static unsigned char attributes_to_dtype(ULONG attrs, ULONG reparse_tag) {
  // Check reparse tags before the directory bit — a directory symlink has
  // both FILE_ATTRIBUTE_DIRECTORY and FILE_ATTRIBUTE_REPARSE_POINT set.
  if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
    if (reparse_tag == IO_REPARSE_TAG_SYMLINK)
      return DT_LNK;
    if (reparse_tag == IO_REPARSE_TAG_AF_UNIX)
      return DT_SOCK;
    // Mount points/junctions fall through to DT_DIR below.
  }
  if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    return DT_DIR;
  return DT_REG;
}

// Probe whether a regular file is actually a FIFO marker. Opens the entry
// relative to the parent directory handle for a quick magic-byte read.
// Returns DT_FIFO if the file is a FIFO marker, DT_REG otherwise.
// Only called for candidate entries (SYSTEM|HIDDEN, non-directory, small size).
static unsigned char probe_fifo_marker(HANDLE parent_dir,
                                       const WCHAR *name, ULONG name_len_bytes) {
  UNICODE_STRING us;
  us.Length = static_cast<USHORT>(name_len_bytes);
  us.MaximumLength = us.Length;
  us.Buffer = const_cast<WCHAR *>(name);

  OBJECT_ATTRIBUTES oa;
  oa.Length = sizeof(oa);
  oa.RootDirectory = parent_dir;
  oa.ObjectName = &us;
  oa.Attributes = 0;
  oa.SecurityDescriptor = nullptr;
  oa.SecurityQualityOfService = nullptr;

  HANDLE h;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = NtOpenFile(
      &h, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  if (!NT_SUCCESS(status))
    return DT_REG;

  unsigned char result =
      internal::is_fifo_marker(h) ? DT_FIFO : DT_REG;
  NtClose(h);
  return result;
}

// Check if an entry is a FIFO marker candidate: SYSTEM+HIDDEN, not a
// directory, and small enough to be a marker file (≤16 bytes).
static bool is_fifo_candidate(
    const FILE_ID_EXTD_BOTH_DIR_INFORMATION *entry) {
  constexpr ULONG FIFO_ATTRS =
      FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN;
  if ((entry->FileAttributes & (FIFO_ATTRS | FILE_ATTRIBUTE_DIRECTORY)) !=
      FIFO_ATTRS)
    return false;
  return entry->EndOfFile.QuadPart >= 8 &&
         entry->EndOfFile.QuadPart <=
             static_cast<LONGLONG>(internal::FIFO_MARKER_SIZE);
}

// Sentinel returned by pack_dirent when the name cannot be converted to
// UTF-8 (e.g. unpaired surrogates). The caller should skip the entry.
static constexpr size_t DIRENT_SKIP = SIZE_MAX;

// Repack one NT directory entry as a struct dirent into |out|.
// |parent_dir| is the directory handle, used for FIFO marker probing.
// Returns d_reclen (bytes written), 0 if the entry doesn't fit in |avail|
// bytes, or DIRENT_SKIP if the filename cannot be converted to UTF-8.
static size_t
pack_dirent(uint8_t *out, size_t avail,
            const FILE_ID_EXTD_BOTH_DIR_INFORMATION *nt_entry,
            HANDLE parent_dir) {
  // NTFS allows up to 255 UTF-16 code units. UTF-8 worst case is 3 bytes
  // per BMP code point — 765 bytes. 1024 is sufficient.
  char name_buf[1024];
  ULONG utf8_bytes = 0;
  NTSTATUS cvt = ::RtlUnicodeToUTF8N(
      name_buf, sizeof(name_buf) - 1, &utf8_bytes, nt_entry->FileName,
      static_cast<ULONG>(nt_entry->FileNameLength));
  if (!NT_SUCCESS(cvt))
    return DIRENT_SKIP;
  name_buf[utf8_bytes] = '\0';

  size_t name_len = static_cast<size_t>(utf8_bytes);
  size_t header_size = __builtin_offsetof(struct ::dirent, d_name);
  size_t reclen = header_size + name_len + 1;
  // Align to pointer size for safe struct casting.
  reclen = (reclen + alignof(void *) - 1) & ~(alignof(void *) - 1);

  if (reclen > avail)
    return 0;

  auto *d = reinterpret_cast<struct ::dirent *>(out);

  // Use lower 64 bits of the 128-bit FileId for d_ino. Fall back to
  // FileIndex if FileId is zero (FAT32/exFAT don't provide real IDs).
  uint64_t ino;
  __builtin_memcpy(&ino, nt_entry->FileId.Identifier, sizeof(ino));
  if (ino == 0)
    ino = static_cast<uint64_t>(nt_entry->FileIndex) | 1;

  d->d_ino = static_cast<ino_t>(ino);
  // d_off is set by the caller (monotonic counter for seekdir cookie).
  d->d_reclen = static_cast<unsigned short>(reclen);
  d->d_type =
      attributes_to_dtype(nt_entry->FileAttributes, nt_entry->ReparsePointTag);
  // Probe FIFO marker files: SYSTEM|HIDDEN regular files with the right size.
  // This opens the entry relative to the parent handle — fast path since it's
  // a relative open (no path reparsing). Only triggered for rare candidates.
  if (d->d_type == DT_REG && is_fifo_candidate(nt_entry))
    d->d_type = probe_fifo_marker(parent_dir, nt_entry->FileName,
                                  nt_entry->FileNameLength);
  __builtin_memcpy(d->d_name, name_buf, name_len + 1);

  // Zero padding between the null terminator and the aligned record end.
  size_t used = header_size + name_len + 1;
  if (reclen > used)
    __builtin_memset(out + used, 0, reclen - used);

  return reclen;
}

ErrorOr<size_t> platform_fetch_dirents(int fd, cpp::span<uint8_t> buffer,
                                       bool restart, long last_cookie) {
  long counter = last_cookie;
  auto *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return Error(EBADF);

  HANDLE handle = ofd->handle;

  // NT buffer for the full info class. Sized to hold a meaningful batch:
  // ~120 bytes/entry → ~50 entries at 6KB, reducing kernel transitions
  // compared to the previous 2KB buffer (~17 entries).
  constexpr size_t NT_BUFSIZE = 6144;
  alignas(8) uint8_t nt_buf[NT_BUFSIZE];
  IO_STATUS_BLOCK iosb = {};

  ULONG query_flags = 0;
  if (restart)
    query_flags |= FILE_QUERY_RESTART_SCAN;

  // seekdir case: restart=true with a non-zero cookie means we need to
  // skip |last_cookie| entries from the beginning before packing results.
  // This keeps skip and pack in one call, avoiding batch-boundary issues.
  //
  // Optimization: the skip phase uses FileNamesInformation (~12 bytes/entry
  // header vs ~120 for FileIdExtdBothDirectoryInformation) with a 16KB
  // buffer, fitting ~6x more entries per syscall. After skipping, the NT
  // cursor is positioned correctly and we switch to the full info class.
  //
  // Concurrent modification safety: if entries were added or removed between
  // telldir and seekdir, POSIX says the behavior is undefined. Our monotonic
  // counter means the N-th entry may differ, but the skip loop handles all
  // edge cases gracefully:
  //   - Cookie > current directory size: skip exhausts → returns 0 entries.
  //   - Entries added: skip lands on a different entry — POSIX-legal.
  //   - Entries deleted: same treatment, skip may overshoot → 0 entries.
  if (restart && last_cookie > 0) {
    constexpr size_t SKIP_BUFSIZE = 16384;
    alignas(8) uint8_t skip_buf[SKIP_BUFSIZE];

    long skipped = 0;
    while (skipped < last_cookie) {
      NTSTATUS skip_status = NtQueryDirectoryFileEx(
          handle, nullptr, nullptr, nullptr, &iosb, skip_buf,
          sizeof(skip_buf), FileNamesInformation, query_flags, nullptr);
      query_flags = 0; // Only restart on first call.

      if (skip_status == STATUS_NO_MORE_FILES)
        return static_cast<size_t>(0); // Past end — no entries to return.
      if (!NT_SUCCESS(skip_status))
        return Error(windows_util::ntstatus_to_errno(skip_status));

      // Count entries in this batch.
      auto *cur =
          reinterpret_cast<const FILE_NAMES_INFORMATION *>(skip_buf);
      for (;;) {
        ++skipped;
        if (skipped >= last_cookie)
          break;
        if (cur->NextEntryOffset == 0)
          break;
        cur = reinterpret_cast<const FILE_NAMES_INFORMATION *>(
            reinterpret_cast<const uint8_t *>(cur) + cur->NextEntryOffset);
      }

      if (skipped >= last_cookie) {
        query_flags = 0; // Continue from where the skip left off.
        break;
      }
    }
  }

  // Normal fetch: query the next batch of NT entries.
  NTSTATUS status = NtQueryDirectoryFileEx(
      handle, nullptr, nullptr, nullptr, &iosb, nt_buf, sizeof(nt_buf),
      FileIdExtdBothDirectoryInformation, query_flags, nullptr);

  if (status == STATUS_NO_MORE_FILES)
    return static_cast<size_t>(0);
  if (!NT_SUCCESS(status))
    return Error(windows_util::ntstatus_to_errno(status));

  size_t written = 0;
  auto *cur =
      reinterpret_cast<const FILE_ID_EXTD_BOTH_DIR_INFORMATION *>(nt_buf);

  for (;;) {
    size_t reclen =
        pack_dirent(buffer.data() + written, buffer.size() - written, cur,
                    handle);
    if (reclen == 0)
      break; // Buffer full.
    if (reclen != DIRENT_SKIP) {
      // Assign monotonic d_off cookie for telldir/seekdir.
      ++counter;
      auto *d = reinterpret_cast<struct ::dirent *>(buffer.data() + written);
      d->d_off = static_cast<off_t>(counter);
      written += reclen;
    }
    // DIRENT_SKIP: unconvertible filename — skip entry, continue.

    if (cur->NextEntryOffset == 0)
      break;
    cur = reinterpret_cast<const FILE_ID_EXTD_BOTH_DIR_INFORMATION *>(
        reinterpret_cast<const uint8_t *>(cur) + cur->NextEntryOffset);
  }

  return written;
}

int platform_closedir(int fd) {
  auto *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return EBADF;
  HANDLE handle = ofd->handle;
  NTSTATUS status = NtClose(handle);
  // Always free the slot — POSIX requires closedir to invalidate the
  // stream even if the underlying close fails.
  internal::fd_table.free_slot(fd);
  return NT_SUCCESS(status) ? 0 : EIO;
}

int platform_fdopendir(int fd) {
  auto *ofd = internal::fd_table.get_ofd(fd);
  if (!ofd)
    return EBADF;

  HANDLE handle = ofd->handle;

  // Validate that the handle refers to a directory.
  FILE_STANDARD_INFORMATION info;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS status = ::NtQueryInformationFile(
      handle, &iosb, &info, sizeof(info), FileStandardInformation);
  if (!NT_SUCCESS(status))
    return EIO;
  if (!info.Directory)
    return ENOTDIR;

  // Ensure the OFD is tagged as a directory for consistency with opendir.
  // The status_flags atomic allows safe concurrent fcntl access.
  int cur = ofd->status_flags.load(cpp::MemoryOrder::RELAXED);
  if (!(cur & O_DIRECTORY))
    ofd->status_flags.store(cur | O_DIRECTORY, cpp::MemoryOrder::RELAXED);

  return 0;
}

bool platform_seekdir(int /*fd*/, long /*cookie*/) {
  // NT has no directory position cookie. The actual skip happens inside
  // platform_fetch_dirents when restart=true and last_cookie > 0 — this
  // avoids batch-boundary issues by keeping skip and pack in one call.
  return true; // Tell Dir to set restart_scan for the next fetch.
}

} // namespace LIBC_NAMESPACE_DECL
