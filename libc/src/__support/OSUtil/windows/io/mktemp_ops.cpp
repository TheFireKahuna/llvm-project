//===-- Internal mktemp engine implementation ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX mktemp: replaces the trailing "XXXXXX" in the template with random
// characters and verifies the resulting name does not exist.
//
// Returns 0 on success, -errno on failure.
//
//===----------------------------------------------------------------------===//

#include "mktemp_ops.h"

#include "hdr/errno_macros.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/nt/nt_path.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/string/string_utils.h"
#include "src/unistd/getentropy.h"

namespace LIBC_NAMESPACE_DECL {
namespace {

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

int mktemp(char *tmpl) {
  size_t len = internal::string_length(tmpl);
  if (len < 6)
    return -EINVAL;

  // Verify trailing XXXXXX.
  char *suffix = tmpl + len - 6;
  for (int i = 0; i < 6; ++i) {
    if (suffix[i] != 'X')
      return -EINVAL;
  }

  static constexpr int MAX_ATTEMPTS = 238328; // 62^3, matches glibc TMP_MAX.
  for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    fill_suffix(suffix);

    // Convert to NT path and check existence via NtQueryAttributesFile.
    auto path_buf_s = path_scratch();
    if (!path_buf_s) return -ENOMEM;
    WCHAR *path_buf = path_buf_s.data();
    size_t path_len = to_nt_path(tmpl, path_buf, path_buf_s.size());
    if (path_len == 0)
      return -EINVAL;

    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    init_object_attributes(&oa, &us, path_buf, path_len);

    FILE_BASIC_INFORMATION basic_info;
    NTSTATUS status = NtQueryAttributesFile(&oa, &basic_info);

    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND) {
      // Name does not exist — success.
      return 0;
    }

    if (NT_SUCCESS(status))
      continue; // Name exists — retry with new random suffix.

    // Unexpected error querying the path.
    if (status == STATUS_ACCESS_DENIED)
      return -EACCES;
    return -EIO;
  }

  return -EEXIST;
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
