//===-- Implementation of fsetpos -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/fsetpos.h"
#include "src/__support/File/file.h"

#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"

#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
#include "src/__support/wchar/mbstate.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, fsetpos,
                   (::FILE *__restrict stream, const fpos_t *__restrict pos)) {
  auto *f = reinterpret_cast<LIBC_NAMESPACE::File *>(stream);

  // Hold the lock across seek + state restore so fgetwc on another thread
  // never observes a byte position with stale mbstate (or vice versa).
  // seek_unlocked clears wide_mbstate as a side effect (C11 §7.21.9.2);
  // we overwrite it immediately with the snapshot from the matching
  // fgetpos to honor C11 §7.21.9.1's restore-the-parse-state contract.
  f->lock();
  auto result = f->seek_unlocked(pos->__pos, SEEK_SET);
  if (!result.has_value()) {
    f->unlock();
    libc_errno = result.error();
    return -1;
  }
#ifdef LIBC_COPT_FILE_WIDE_SUPPORT
  static_assert(sizeof(internal::mbstate) <= sizeof(pos->__state),
                "fpos_t::__state too small for wide parse state");
  __builtin_memcpy(f->get_wide_mbstate(), pos->__state,
                   sizeof(internal::mbstate));
#endif
  f->unlock();
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
