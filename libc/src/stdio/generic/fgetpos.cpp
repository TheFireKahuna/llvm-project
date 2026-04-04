//===-- Implementation of fgetpos -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/fgetpos.h"
#include "src/__support/File/file.h"

#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
#include "src/__support/wchar/mbstate.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, fgetpos,
                   (::FILE *__restrict stream, fpos_t *__restrict pos)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);

  // Atomic snapshot: the byte position and the wide parse state must come
  // from the same instant so fsetpos can restore them coherently (C11
  // §7.21.9.1). Held under the file lock; we use tell_unlocked to avoid
  // re-entering the recursive mutex.
  f->lock();
  auto result = f->tell_unlocked();
  if (!result.has_value()) {
    f->unlock();
    libc_errno = result.error();
    return -1;
  }
  pos->__pos = result.value();
#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
  static_assert(sizeof(internal::mbstate) <= sizeof(pos->__state),
                "fpos_t::__state too small for wide parse state");
  __builtin_memcpy(pos->__state, f->get_wide_mbstate(),
                   sizeof(internal::mbstate));
#else
  __builtin_memset(pos->__state, 0, sizeof(pos->__state));
#endif
  f->unlock();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
