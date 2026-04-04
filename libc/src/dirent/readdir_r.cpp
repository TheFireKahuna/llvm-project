//===-- Implementation of readdir_r ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "readdir_r.h"

#include "src/__support/File/dir.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

#include <dirent.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, readdir_r,
                    (::DIR *__restrict dirp, struct ::dirent *__restrict entry,
                     struct ::dirent **__restrict result)) {
  auto *d = reinterpret_cast<LIBC_NAMESPACE::Dir *>(dirp);
  auto dirent_val = d->read();
  if (!dirent_val) {
    // I/O or other error — return errno value, signal no result.
    *result = nullptr;
    return dirent_val.error();
  }
  struct ::dirent *src = dirent_val.value();
  if (src == nullptr) {
    // End of directory stream — success, no more entries.
    *result = nullptr;
    return 0;
  }
  // Copy the variable-length dirent into the caller-supplied buffer.
  // The caller is required by POSIX to allocate entry with d_name sized to
  // at least {NAME_MAX}+1 bytes, so d_reclen will not overflow.
  __builtin_memcpy(entry, src, src->d_reclen);
  *result = entry;
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
