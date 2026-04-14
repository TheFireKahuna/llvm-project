//===-- Internal shm_open/shm_unlink/memfd_create implementation -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Engine layer for POSIX shared memory operations on Windows.
// All functions return value on success, -errno on failure.
// Never includes or sets libc_errno.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/shm_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "hdr/types/mode_t.h"
#include "include/llvm-libc-macros/sys-mman-macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/io/fd_ops.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/memory/shm_common.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/nt/section_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t shm_open(const char *name, int oflag, mode_t mode) {
  char path[shm_common::SHM_PATH_MAX];
  int path_ret = shm_common::build_path(name, path, sizeof(path));
  if (path_ret < 0)
    return static_cast<intptr_t>(path_ret);

  if (oflag & O_CREAT) {
    if (!shm_common::ensure_dir(path)) {
      return -EACCES;
    }
  }

  int fd = static_cast<int>(internal::open(path, oflag, mode));
  if (fd < 0) {
    return static_cast<intptr_t>(fd); // already -errno from internal::open
  }

  // Create a section backed by this file and store it in the fd entry.
  // All subsequent mmap(MAP_SHARED, fd, ...) calls reuse this section,
  // ensuring views share the same physical pages. Without this, two mmaps
  // of the same shm fd would create independent pagefile-backed sections
  // with no shared pages — a MAP_SHARED coherence violation.
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (ofd) {
    HANDLE file_handle = ofd->handle;

    // Build a DACL for the section from the POSIX mode so cross-process
    // access respects owner/group/other permissions.
    auto sd_s = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
    SECURITY_DESCRIPTOR *sd = sd_s ? windows_sec::build_creation_sd(
        reinterpret_cast<UCHAR *>(sd_s.data()), mode, nullptr, nullptr,
        windows_sec::mode_bits_to_section_access_mask) : nullptr;

    NTSTATUS sec_st;
    windows::SectionHandle section = windows::SectionHandle::create_file(
        file_handle,
        SECTION_MAP_READ | SECTION_MAP_WRITE |
            SECTION_QUERY | SECTION_EXTEND_SIZE,
        PAGE_READWRITE, SEC_COMMIT, sd, &sec_st);
    if (section)
      ofd->disk().section_handle.store(section.release(), cpp::MemoryOrder::RELEASE);
    // If section creation fails (e.g., new empty file with O_CREAT),
    // it will be created lazily on the first ftruncate/mmap via
    // map_file_into_placeholder's OFD check.
  }

  return static_cast<intptr_t>(fd);
}

intptr_t shm_unlink(const char *name) {
  char path[shm_common::SHM_PATH_MAX];
  int path_ret = shm_common::build_path(name, path, sizeof(path));
  if (path_ret < 0)
    return static_cast<intptr_t>(path_ret);

  // Convert to NT path and delete. SHM paths are capped at WIN_MAX_PATH,
  // so a small buffer suffices (avoids 64 KB stack allocation).
  WCHAR nt_path[shm_common::SHM_WIDE_BUF];
  size_t nt_len = to_nt_path(path, nt_path, shm_common::SHM_WIDE_BUF);
  if (nt_len == 0) {
    return -EINVAL;
  }

  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  init_object_attributes(&oa, &us, nt_path, nt_len);

  NTSTATUS status = ::NtDeleteFile(&oa);
  if (NT_ERROR(status)) {
    return -static_cast<intptr_t>(windows_util::ntstatus_to_errno(status));
  }

  return 0;
}

intptr_t memfd_create(const char *name, unsigned int flags) {
  if (LIBC_UNLIKELY(!name)) {
    return -EFAULT;
  }

  // Validate flags.
  constexpr unsigned int VALID_FLAGS =
      MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB;
  if (LIBC_UNLIKELY((flags & ~VALID_FLAGS) != 0)) {
    return -EINVAL;
  }

  // Create a temp file for read/write compatibility.
  // The file is created in the same shm directory as shm_open, with a
  // unique auto-generated name to avoid collisions.
  char path[shm_common::SHM_PATH_MAX];
  int path_ret = shm_common::build_memfd_path(name, path, sizeof(path));
  if (path_ret < 0)
    return static_cast<intptr_t>(path_ret);

  if (!shm_common::ensure_dir(path)) {
    return -EACCES;
  }

  int open_flags = O_RDWR | O_CREAT | O_EXCL;
  int fd = static_cast<int>(internal::open(path, open_flags, 0600));
  if (fd < 0) {
    return static_cast<intptr_t>(fd); // already -errno from internal::open
  }

  // Set FD_CLOEXEC and revoke OBJ_INHERIT if requested.
  if (flags & MFD_CLOEXEC)
    fd_table.set_fd_cloexec(fd, true);

  // Unlink the temp file immediately — matches Linux memfd semantics where
  // the file has no filesystem path. The handle survives via SHARE_DELETE.
  shm_common::unlink_path(path);

  // Create the pagefile-backed section. Initial size is 0 — ftruncate
  // will extend it via NtExtendSection before the first mmap.
  // For now, create with a minimal size (1 page) that can be extended.
  // NtCreateSectionEx with size 0 from a file uses the file's size,
  // which is 0 for our new temp file.
  //
  // The section is created lazily on the first ftruncate or mmap call
  // via the OpenFileDescription::section_handle check in map_file_into_placeholder.
  // This avoids creating a section for fds that are never mmapped.
  //
  // For memfd, we DO create it eagerly so that ftruncate can extend it
  // and multiple mmap calls share the same section.
  OpenFileDescription *ofd = fd_table.get_ofd(fd);
  if (ofd) {
    HANDLE file_handle = ofd->handle;

    // The section is pagefile-backed (SEC_COMMIT) with max protection
    // PAGE_EXECUTE_READWRITE so any mmap protection level is supported.
    ULONG sec_flags = SEC_COMMIT;
    if (flags & MFD_HUGETLB)
      sec_flags |= SEC_LARGE_PAGES;

    // Use the file handle for the section so ftruncate (NtSetInformationFile
    // to extend the file) is reflected in the section size. The section's
    // max size tracks the file size.
    NTSTATUS sec_st;
    windows::SectionHandle section = windows::SectionHandle::create_file(
        file_handle,
        SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE |
            SECTION_QUERY | SECTION_EXTEND_SIZE,
        PAGE_EXECUTE_READWRITE, sec_flags, nullptr, &sec_st);
    if (section)
      ofd->disk().section_handle.store(section.release(), cpp::MemoryOrder::RELEASE);
    // If section creation fails (e.g., file is size 0), it will be created
    // lazily on the first ftruncate/mmap. Not a fatal error.
  }

  return static_cast<intptr_t>(fd);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
