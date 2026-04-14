//===-- Internal mkstemp engine implementation -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX mkstemp: replaces the trailing "XXXXXX" in the template with random
// characters, creates the file exclusively, and returns an open fd.
//
// Returns fd (>= 0) on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#include "mkstemp_ops.h"

#include "hdr/errno_macros.h"
#include "hdr/fcntl_macros.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/fd/fd_table.h"
#include "src/__support/OSUtil/windows/io/io_ring_helpers.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/security/security.h"
#include "src/__support/macros/config.h"
#include "src/string/string_utils.h"
#include "src/unistd/getentropy.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

// Fill 6 bytes of the template suffix with random alphanumeric characters.
void fill_suffix(char *suffix) {
  static constexpr char chars[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static constexpr int NUM_CHARS = 62;
  uint8_t rand_bytes[6];
  LIBC_NAMESPACE::getentropy(rand_bytes, 6);
  for (int i = 0; i < 6; ++i)
    suffix[i] = chars[rand_bytes[i] % NUM_CHARS];
}

} // namespace

namespace internal {

intptr_t mkstemp(char *tmpl) {
  size_t len = internal::string_length(tmpl);
  if (len < 6)
    return -EINVAL;

  // Verify trailing XXXXXX.
  char *suffix = tmpl + len - 6;
  for (int i = 0; i < 6; ++i) {
    if (suffix[i] != 'X')
      return -EINVAL;
  }

  // Retry on name collision. With CSPRNG entropy per attempt, collision is
  // astronomically unlikely — this bound matches glibc's TMP_MAX (62^3).
  static constexpr int MAX_ATTEMPTS = 238328;
  for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    fill_suffix(suffix);

    // Convert the modified template path to NT format and open exclusively.
    auto path_buf_s = path_scratch();
    if (!path_buf_s) return -ENOMEM;
    WCHAR *path_buf = path_buf_s.data();
    size_t path_len = to_nt_path(tmpl, path_buf, path_buf_s.size());
    if (path_len == 0)
      return -EINVAL;

    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    init_object_attributes(&oa, &us, path_buf, path_len);

    // POSIX: mkstemp creates with mode 0600. Apply atomically via SD.
    auto sd_s = internal::byte_scratch(windows_sec::CREATION_SD_BUF_SIZE);
    if (!sd_s)
      return -ENOMEM;
    oa.SecurityDescriptor = windows_sec::build_creation_sd(
        reinterpret_cast<UCHAR *>(sd_s.data()), 0600);

    ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES |
                         FILE_READ_DATA | FILE_WRITE_DATA |
                         FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
    // Match open(O_RDWR|O_CREAT|O_EXCL) share mode — POSIX specifies
    // mkstemp as equivalent to open() with those flags. FILE_CREATE
    // already guarantees no pre-existing file; share mode governs
    // subsequent opens and unlink, which POSIX allows on open files.
    ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    HANDLE handle;
    NTSTATUS status =
        open_overlapped(&oa, access, FILE_CREATE, share, &handle);

    if (status == STATUS_OBJECT_NAME_COLLISION)
      continue; // Name collision — retry with new random suffix.

    if (!NT_SUCCESS(status)) {
      if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
          status == STATUS_OBJECT_PATH_NOT_FOUND)
        return -ENOENT;
      else if (status == STATUS_ACCESS_DENIED)
        return -EACCES;
      else
        return -EIO;
    }

    // EA + READONLY — DACL was set atomically via SecurityDescriptor.
    windows_sec::post_create_perms(handle, 0600);

    // Allocate fd from the table. Ring + event created internally.
    int open_flags = O_RDWR | O_CREAT | O_EXCL;
    auto fd_result = internal::fd_table.alloc(handle, open_flags);
    if (!fd_result.has_value()) {
      NtClose(handle);
      return -fd_result.error();
    }

    return fd_result.value();
  }

  // All retries exhausted.
  return -EEXIST;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
